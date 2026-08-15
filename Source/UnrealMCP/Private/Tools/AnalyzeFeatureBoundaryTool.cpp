#include "Tools/AnalyzeFeatureBoundaryTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SQLitePreparedStatement.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    constexpr int32 DefaultMaxResults = 100;
    constexpr int32 MaximumMaxResults = 500;

    struct FAssetRow
    {
        FString ObjectPath;
        FString AssetName;
        FString ClassPath;
        FString PackageName;
        FString PackagePath;
        FString ContentScope;
        bool bIsBlueprint = false;
    };

    struct FDependencyEdge
    {
        FString SourcePackage;
        FString TargetPackage;
        FString Direction;
    };

    struct FBoundaryCounts
    {
        int32 Outgoing = 0;
        int32 Incoming = 0;
    };

    struct FScopeGroup
    {
        TSet<FString> Packages;
        int32 OutgoingEdges = 0;
        int32 IncomingEdges = 0;
    };

    FString NormalizePackagePath(FString Path)
    {
        Path.TrimStartAndEndInline();
        Path.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
        {
            Path.LeftChopInline(1);
        }
        return Path;
    }

    bool IsWithinBoundary(const FString& PackageName, const FString& RootPath)
    {
        return PackageName.Equals(RootPath, ESearchCase::IgnoreCase)
            || PackageName.StartsWith(RootPath + TEXT("/"), ESearchCase::IgnoreCase);
    }

    FString ResolveScope(const FString& PackageName, const TMap<FString, FAssetRow>& AssetsByPackage)
    {
        if (const FAssetRow* Asset = AssetsByPackage.Find(PackageName))
        {
            if (Asset->ContentScope == TEXT("project") || Asset->ContentScope == TEXT("engine") || Asset->ContentScope == TEXT("plugin"))
            {
                return Asset->ContentScope;
            }
        }
        return UnrealMCP::IndexedQueryToolUtils::GetContentScopeFromPackageName(PackageName);
    }

    TArray<TSharedPtr<FJsonValue>> StringValues(TArray<FString> Values)
    {
        Values.Sort();
        TArray<TSharedPtr<FJsonValue>> Result;
        Result.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            Result.Add(MakeShared<FJsonValueString>(Value));
        }
        return Result;
    }

    TSharedRef<FJsonObject> SerializeEdge(
        const FDependencyEdge& Edge,
        const TMap<FString, FAssetRow>& AssetsByPackage)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("direction"), Edge.Direction);
        Json->SetStringField(TEXT("sourcePackageName"), Edge.SourcePackage);
        Json->SetStringField(TEXT("targetPackageName"), Edge.TargetPackage);
        const FString OutsidePackage = Edge.Direction == TEXT("outgoing") ? Edge.TargetPackage : Edge.SourcePackage;
        Json->SetStringField(TEXT("outsidePackageName"), OutsidePackage);
        Json->SetStringField(TEXT("outsideContentScope"), ResolveScope(OutsidePackage, AssetsByPackage));
        Json->SetBoolField(TEXT("outsidePackageIndexed"), AssetsByPackage.Contains(OutsidePackage));
        return Json;
    }

    void AssignRisk(
        const int32 CrossingEdges,
        const int32 DependencyEvidenceCount,
        const int32 InsideAssetCount,
        const double ExternalCouplingRatio,
        const double BoundaryAssetRatio,
        FString& OutRiskLevel,
        TArray<FString>& OutReasons)
    {
        const bool bHighExternalRatio = DependencyEvidenceCount >= 10 && ExternalCouplingRatio >= 0.60;
        const bool bHighBoundaryRatio = InsideAssetCount >= 10 && CrossingEdges >= 10 && BoundaryAssetRatio >= 0.50;
        if (CrossingEdges >= 20 || bHighExternalRatio || bHighBoundaryRatio)
        {
            OutRiskLevel = TEXT("high");
            if (CrossingEdges >= 20) OutReasons.Add(TEXT("crossing_edge_count_at_least_20"));
            if (bHighExternalRatio) OutReasons.Add(TEXT("external_coupling_ratio_at_least_0_60_with_10_edges"));
            if (bHighBoundaryRatio) OutReasons.Add(TEXT("boundary_asset_ratio_at_least_0_50_with_10_assets_and_edges"));
            return;
        }
        const bool bMediumExternalRatio = DependencyEvidenceCount >= 5 && ExternalCouplingRatio >= 0.30;
        const bool bMediumBoundaryRatio = InsideAssetCount >= 4 && CrossingEdges >= 3 && BoundaryAssetRatio >= 0.25;
        if (CrossingEdges >= 5 || bMediumExternalRatio || bMediumBoundaryRatio)
        {
            OutRiskLevel = TEXT("medium");
            if (CrossingEdges >= 5) OutReasons.Add(TEXT("crossing_edge_count_at_least_5"));
            if (bMediumExternalRatio) OutReasons.Add(TEXT("external_coupling_ratio_at_least_0_30_with_5_edges"));
            if (bMediumBoundaryRatio) OutReasons.Add(TEXT("boundary_asset_ratio_at_least_0_25_with_4_assets_and_3_crossing_edges"));
            return;
        }
        OutRiskLevel = TEXT("low");
        OutReasons.Add(CrossingEdges == 0 ? TEXT("no_crossing_dependencies") : TEXT("below_boundary_risk_thresholds"));
    }
}

FAnalyzeFeatureBoundaryTool::FAnalyzeFeatureBoundaryTool()
    : FMCPToolBase(
        TEXT("AnalyzeFeatureBoundary"),
        TEXT("Analyzes static indexed package dependencies crossing a required /Game feature boundary. It does not infer runtime architecture."))
{
}

UnrealMCP::FMCPResponse FAnalyzeFeatureBoundaryTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString RootPath;
    FString PackagePath;
    int32 MaxResults = DefaultMaxResults;
    if (Request.Params.IsValid())
    {
        Request.Params->TryGetStringField(TEXT("rootPath"), RootPath);
        Request.Params->TryGetStringField(TEXT("packagePath"), PackagePath);
        double RequestedMaxResults = DefaultMaxResults;
        if (Request.Params->TryGetNumberField(TEXT("maxResults"), RequestedMaxResults))
        {
            if (!FMath::IsNearlyEqual(RequestedMaxResults, FMath::RoundToDouble(RequestedMaxResults))
                || RequestedMaxResults < 1.0 || RequestedMaxResults > MaximumMaxResults)
            {
                return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeFeatureBoundary maxResults must be an integer from 1 to 500."));
            }
            MaxResults = FMath::RoundToInt(RequestedMaxResults);
        }
    }

    RootPath = NormalizePackagePath(RootPath);
    PackagePath = NormalizePackagePath(PackagePath);
    if (!RootPath.IsEmpty() && !PackagePath.IsEmpty() && !RootPath.Equals(PackagePath, ESearchCase::IgnoreCase))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeFeatureBoundary rootPath and packagePath must match when both are supplied."));
    }
    const FString BoundaryPath = !PackagePath.IsEmpty() ? PackagePath : RootPath;
    if (BoundaryPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeFeatureBoundary requires rootPath or packagePath."));
    }
    if (!(BoundaryPath.Equals(TEXT("/Game"), ESearchCase::IgnoreCase)
        || BoundaryPath.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase)))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeFeatureBoundary rootPath/packagePath must be under /Game."));
    }

    TMap<FString, FAssetRow> AssetsByPackage;
    TSet<FString> InsidePackages;
    TArray<FAssetRow> InsideAssets;
    TArray<FDependencyEdge> CrossingEdges;
    int32 InternalEdgeCount = 0;
    FString QueryError;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutError)
        {
            FSQLitePreparedStatement AssetStatement(
                Database,
                TEXT("SELECT object_path, asset_name, class_path, package_name, package_path, content_scope, is_blueprint "
                     "FROM assets ORDER BY package_name ASC, object_path ASC;"),
                ESQLitePreparedStatementFlags::None);
            if (!AssetStatement.IsValid())
            {
                OutError = TEXT("AnalyzeFeatureBoundary could not prepare the indexed asset query.");
                return false;
            }
            const int64 AssetResult = AssetStatement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FAssetRow Asset;
                int32 bIsBlueprint = 0;
                if (!Row.GetColumnValueByIndex(0, Asset.ObjectPath)
                    || !Row.GetColumnValueByIndex(1, Asset.AssetName)
                    || !Row.GetColumnValueByIndex(2, Asset.ClassPath)
                    || !Row.GetColumnValueByIndex(3, Asset.PackageName)
                    || !Row.GetColumnValueByIndex(4, Asset.PackagePath)
                    || !Row.GetColumnValueByIndex(5, Asset.ContentScope)
                    || !Row.GetColumnValueByIndex(6, bIsBlueprint))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }
                Asset.bIsBlueprint = bIsBlueprint != 0;
                if (!AssetsByPackage.Contains(Asset.PackageName)) AssetsByPackage.Add(Asset.PackageName, Asset);
                if (Asset.ContentScope == TEXT("project") && IsWithinBoundary(Asset.PackageName, BoundaryPath))
                {
                    InsidePackages.Add(Asset.PackageName);
                    InsideAssets.Add(MoveTemp(Asset));
                }
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });
            if (AssetResult == INDEX_NONE)
            {
                OutError = Database.GetLastError().IsEmpty() ? TEXT("AnalyzeFeatureBoundary indexed asset query failed.") : Database.GetLastError();
                return false;
            }

            FSQLitePreparedStatement DependencyStatement(
                Database,
                TEXT("SELECT source_package_name, target_package_name FROM asset_dependencies "
                     "ORDER BY source_package_name ASC, target_package_name ASC;"),
                ESQLitePreparedStatementFlags::None);
            if (!DependencyStatement.IsValid())
            {
                OutError = TEXT("AnalyzeFeatureBoundary could not prepare the dependency query.");
                return false;
            }
            const int64 DependencyResult = DependencyStatement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FString Source;
                FString Target;
                if (!Row.GetColumnValueByIndex(0, Source) || !Row.GetColumnValueByIndex(1, Target))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }
                const bool bSourceIndexedInside = InsidePackages.Contains(Source);
                const bool bTargetIndexedInside = InsidePackages.Contains(Target);
                const bool bSourceWithinPath = IsWithinBoundary(Source, BoundaryPath);
                const bool bTargetWithinPath = IsWithinBoundary(Target, BoundaryPath);
                if (bSourceIndexedInside && bTargetWithinPath)
                {
                    ++InternalEdgeCount;
                }
                else if (bSourceIndexedInside)
                {
                    CrossingEdges.Add({Source, Target, TEXT("outgoing")});
                }
                else if (bTargetIndexedInside && !bSourceWithinPath)
                {
                    CrossingEdges.Add({Source, Target, TEXT("incoming")});
                }
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });
            if (DependencyResult == INDEX_NONE)
            {
                OutError = Database.GetLastError().IsEmpty() ? TEXT("AnalyzeFeatureBoundary dependency query failed.") : Database.GetLastError();
                return false;
            }
            return true;
        }, QueryError);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("AnalyzeFeatureBoundary failed to query the project index: %s"), *QueryError));
    }

    TMap<FString, FBoundaryCounts> CountsByPackage;
    TMap<FString, FScopeGroup> Groups;
    Groups.Add(TEXT("project"));
    Groups.Add(TEXT("engine"));
    Groups.Add(TEXT("plugin"));
    int32 OutgoingCount = 0;
    int32 IncomingCount = 0;
    for (const FDependencyEdge& Edge : CrossingEdges)
    {
        const bool bOutgoing = Edge.Direction == TEXT("outgoing");
        const FString InsidePackage = bOutgoing ? Edge.SourcePackage : Edge.TargetPackage;
        const FString OutsidePackage = bOutgoing ? Edge.TargetPackage : Edge.SourcePackage;
        FBoundaryCounts& Counts = CountsByPackage.FindOrAdd(InsidePackage);
        FScopeGroup& Group = Groups.FindChecked(ResolveScope(OutsidePackage, AssetsByPackage));
        Group.Packages.Add(OutsidePackage);
        if (bOutgoing) { ++Counts.Outgoing; ++Group.OutgoingEdges; ++OutgoingCount; }
        else { ++Counts.Incoming; ++Group.IncomingEdges; ++IncomingCount; }
    }

    TArray<FAssetRow> BoundaryBlueprints = InsideAssets.FilterByPredicate([&](const FAssetRow& Asset)
    {
        return Asset.bIsBlueprint && CountsByPackage.Contains(Asset.PackageName);
    });
    const int32 BoundaryAssetCount = InsideAssets.FilterByPredicate([&](const FAssetRow& Asset)
    {
        return CountsByPackage.Contains(Asset.PackageName);
    }).Num();
    BoundaryBlueprints.Sort([&](const FAssetRow& A, const FAssetRow& B)
    {
        const FBoundaryCounts& AC = CountsByPackage.FindChecked(A.PackageName);
        const FBoundaryCounts& BC = CountsByPackage.FindChecked(B.PackageName);
        const int32 ATotal = AC.Outgoing + AC.Incoming;
        const int32 BTotal = BC.Outgoing + BC.Incoming;
        if (ATotal != BTotal) return ATotal > BTotal;
        if (AC.Outgoing != BC.Outgoing) return AC.Outgoing > BC.Outgoing;
        if (AC.Incoming != BC.Incoming) return AC.Incoming > BC.Incoming;
        return A.ObjectPath < B.ObjectPath;
    });

    TArray<TSharedPtr<FJsonValue>> BoundaryValues;
    const int32 ReturnedBoundaryCount = FMath::Min(BoundaryBlueprints.Num(), MaxResults);
    for (int32 Index = 0; Index < ReturnedBoundaryCount; ++Index)
    {
        const FAssetRow& Asset = BoundaryBlueprints[Index];
        const FBoundaryCounts& Counts = CountsByPackage.FindChecked(Asset.PackageName);
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("rank"), Index + 1);
        Json->SetStringField(TEXT("objectPath"), Asset.ObjectPath);
        Json->SetStringField(TEXT("assetName"), Asset.AssetName);
        Json->SetStringField(TEXT("classPath"), Asset.ClassPath);
        Json->SetStringField(TEXT("packageName"), Asset.PackageName);
        Json->SetNumberField(TEXT("outgoingCrossingDependencyCount"), Counts.Outgoing);
        Json->SetNumberField(TEXT("incomingCrossingDependencyCount"), Counts.Incoming);
        Json->SetNumberField(TEXT("totalCrossingDependencyCount"), Counts.Outgoing + Counts.Incoming);
        BoundaryValues.Add(MakeShared<FJsonValueObject>(Json));
    }

    TArray<TSharedPtr<FJsonValue>> EdgeValues;
    const int32 ReturnedEdgeCount = FMath::Min(CrossingEdges.Num(), MaxResults);
    for (int32 Index = 0; Index < ReturnedEdgeCount; ++Index)
    {
        EdgeValues.Add(MakeShared<FJsonValueObject>(SerializeEdge(CrossingEdges[Index], AssetsByPackage)));
    }

    TArray<TSharedPtr<FJsonValue>> GroupValues;
    for (const FString Scope : {FString(TEXT("project")), FString(TEXT("engine")), FString(TEXT("plugin"))})
    {
        const FScopeGroup& Group = Groups.FindChecked(Scope);
        TArray<FString> Packages = Group.Packages.Array();
        Packages.Sort();
        const int32 ReturnedPackages = FMath::Min(Packages.Num(), MaxResults);
        Packages.SetNum(ReturnedPackages);
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("contentScope"), Scope);
        Json->SetNumberField(TEXT("packageCount"), Group.Packages.Num());
        Json->SetNumberField(TEXT("outgoingCrossingDependencyCount"), Group.OutgoingEdges);
        Json->SetNumberField(TEXT("incomingCrossingDependencyCount"), Group.IncomingEdges);
        Json->SetNumberField(TEXT("totalCrossingDependencyCount"), Group.OutgoingEdges + Group.IncomingEdges);
        Json->SetBoolField(TEXT("packagesTruncated"), ReturnedPackages < Group.Packages.Num());
        Json->SetArrayField(TEXT("packages"), StringValues(MoveTemp(Packages)));
        GroupValues.Add(MakeShared<FJsonValueObject>(Json));
    }

    const int32 CrossingCount = CrossingEdges.Num();
    const int32 DependencyEvidenceCount = InternalEdgeCount + CrossingCount;
    const double ExternalCouplingRatio = DependencyEvidenceCount > 0 ? static_cast<double>(CrossingCount) / DependencyEvidenceCount : 0.0;
    const double CohesionRatio = DependencyEvidenceCount > 0 ? static_cast<double>(InternalEdgeCount) / DependencyEvidenceCount : 0.0;
    const double BoundaryAssetRatio = InsideAssets.Num() > 0 ? static_cast<double>(BoundaryAssetCount) / InsideAssets.Num() : 0.0;
    FString RiskLevel;
    TArray<FString> RiskReasons;
    AssignRisk(CrossingCount, DependencyEvidenceCount, InsideAssets.Num(), ExternalCouplingRatio, BoundaryAssetRatio, RiskLevel, RiskReasons);

    TSharedRef<FJsonObject> Thresholds = MakeShared<FJsonObject>();
    Thresholds->SetStringField(TEXT("high"), TEXT("crossingEdges >= 20 OR (dependencyEvidence >= 10 AND externalCouplingRatio >= 0.60) OR (insideAssets >= 10 AND crossingEdges >= 10 AND boundaryAssetRatio >= 0.50)"));
    Thresholds->SetStringField(TEXT("medium"), TEXT("crossingEdges >= 5 OR (dependencyEvidence >= 5 AND externalCouplingRatio >= 0.30) OR (insideAssets >= 4 AND crossingEdges >= 3 AND boundaryAssetRatio >= 0.25)"));
    Thresholds->SetStringField(TEXT("low"), TEXT("all metrics below medium thresholds"));

    TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("insideAssetCount"), InsideAssets.Num());
    Summary->SetNumberField(TEXT("internalDependencyEdgeCount"), InternalEdgeCount);
    Summary->SetNumberField(TEXT("outgoingCrossingDependencyCount"), OutgoingCount);
    Summary->SetNumberField(TEXT("incomingCrossingDependencyCount"), IncomingCount);
    Summary->SetNumberField(TEXT("totalCrossingDependencyCount"), CrossingCount);
    Summary->SetNumberField(TEXT("boundaryAssetCount"), BoundaryAssetCount);
    Summary->SetNumberField(TEXT("boundaryBlueprintCount"), BoundaryBlueprints.Num());
    Summary->SetNumberField(TEXT("cohesionRatio"), CohesionRatio);
    Summary->SetNumberField(TEXT("externalCouplingRatio"), ExternalCouplingRatio);
    Summary->SetNumberField(TEXT("boundaryAssetRatio"), BoundaryAssetRatio);
    Summary->SetBoolField(TEXT("hasDependencyEvidence"), DependencyEvidenceCount > 0);

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("analysisType"), TEXT("indexed_static_package_feature_boundary"));
    Result->SetStringField(TEXT("rootPath"), BoundaryPath);
    Result->SetNumberField(TEXT("maxResults"), MaxResults);
    Result->SetStringField(TEXT("riskLevel"), RiskLevel);
    Result->SetArrayField(TEXT("riskReasons"), StringValues(MoveTemp(RiskReasons)));
    Result->SetObjectField(TEXT("riskThresholds"), Thresholds);
    Result->SetObjectField(TEXT("summary"), Summary);
    Result->SetArrayField(TEXT("outsideScopeGroups"), GroupValues);
    Result->SetNumberField(TEXT("matchingBoundaryBlueprintCount"), BoundaryBlueprints.Num());
    Result->SetNumberField(TEXT("returnedBoundaryBlueprintCount"), ReturnedBoundaryCount);
    Result->SetBoolField(TEXT("boundaryBlueprintsTruncated"), ReturnedBoundaryCount < BoundaryBlueprints.Num());
    Result->SetArrayField(TEXT("boundaryBlueprints"), BoundaryValues);
    Result->SetNumberField(TEXT("matchingCrossingEdgeCount"), CrossingEdges.Num());
    Result->SetNumberField(TEXT("returnedCrossingEdgeCount"), ReturnedEdgeCount);
    Result->SetBoolField(TEXT("crossingEdgesTruncated"), ReturnedEdgeCount < CrossingEdges.Num());
    Result->SetArrayField(TEXT("crossingEdges"), EdgeValues);
    Result->SetStringField(TEXT("limitation"), TEXT("This analysis uses static indexed package dependencies only. It does not prove runtime calls, execution order, dynamic loading, reflection use, or runtime architecture."));
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAnalyzeFeatureBoundaryTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    for (const FString Field : {FString(TEXT("rootPath")), FString(TEXT("packagePath"))})
    {
        TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
        Property->SetStringField(TEXT("type"), TEXT("string"));
        Property->SetStringField(TEXT("description"), TEXT("Required feature boundary under /Game. rootPath and packagePath are aliases."));
        Properties->SetObjectField(Field, Property);
    }
    TSharedRef<FJsonObject> MaxResultsProperty = MakeShared<FJsonObject>();
    MaxResultsProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxResultsProperty->SetStringField(TEXT("description"), TEXT("Maximum exact crossing edges, ranked boundary Blueprints, and packages per outside-scope group to return. Defaults to 100."));
    MaxResultsProperty->SetNumberField(TEXT("default"), DefaultMaxResults);
    MaxResultsProperty->SetNumberField(TEXT("minimum"), 1);
    MaxResultsProperty->SetNumberField(TEXT("maximum"), MaximumMaxResults);
    Properties->SetObjectField(TEXT("maxResults"), MaxResultsProperty);
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> AnyOf;
    for (const FString Field : {FString(TEXT("rootPath")), FString(TEXT("packagePath"))})
    {
        TSharedRef<FJsonObject> Alternative = MakeShared<FJsonObject>();
        Alternative->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(Field)});
        AnyOf.Add(MakeShared<FJsonValueObject>(Alternative));
    }
    Schema->SetArrayField(TEXT("anyOf"), AnyOf);
    Schema->SetBoolField(TEXT("additionalProperties"), false);
    Schema->SetStringField(TEXT("description"), TEXT("Provide either rootPath or packagePath; one is required and both must match when supplied."));
    return Schema;
}
