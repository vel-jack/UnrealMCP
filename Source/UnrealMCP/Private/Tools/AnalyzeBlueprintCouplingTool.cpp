#include "Tools/AnalyzeBlueprintCouplingTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SQLitePreparedStatement.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    constexpr int32 DefaultMaxResults = 50;
    constexpr int32 MaximumMaxResults = 200;
    constexpr int32 HighTotalDegreeThreshold = 30;
    constexpr int32 HighExternalDegreeThreshold = 15;
    constexpr int32 HighIncomingDegreeThreshold = 20;
    constexpr int32 MediumTotalDegreeThreshold = 15;
    constexpr int32 MediumExternalDegreeThreshold = 7;
    constexpr int32 MediumIncomingDegreeThreshold = 10;

    struct FCouplingRow
    {
        FString ObjectPath;
        FString AssetName;
        FString ClassPath;
        FString PackageName;
        FString PackagePath;
        int32 InternalOutgoingCount = 0;
        int32 ExternalOutgoingCount = 0;
        int32 InternalIncomingCount = 0;
        int32 ExternalIncomingCount = 0;
        int32 TotalOutgoingCount = 0;
        int32 TotalIncomingCount = 0;
        int32 TotalDegree = 0;
        int32 ExternalDegree = 0;
        FString RiskLevel;
        TArray<FString> Reasons;
    };

    FString NormalizePackagePath(FString Path)
    {
        Path = Path.TrimStartAndEnd().Replace(TEXT("\\"), TEXT("/"));
        while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
        {
            Path.LeftChopInline(1);
        }
        return Path;
    }

    void AddRiskReason(TArray<FString>& Reasons, const FString& Reason)
    {
        Reasons.AddUnique(Reason);
    }

    void AssignRisk(FCouplingRow& Row)
    {
        if (Row.TotalDegree >= HighTotalDegreeThreshold
            || Row.ExternalDegree >= HighExternalDegreeThreshold
            || Row.TotalIncomingCount >= HighIncomingDegreeThreshold)
        {
            Row.RiskLevel = TEXT("high");
            if (Row.TotalDegree >= HighTotalDegreeThreshold)
            {
                AddRiskReason(Row.Reasons, TEXT("total_degree_at_least_30"));
            }
            if (Row.ExternalDegree >= HighExternalDegreeThreshold)
            {
                AddRiskReason(Row.Reasons, TEXT("external_degree_at_least_15"));
            }
            if (Row.TotalIncomingCount >= HighIncomingDegreeThreshold)
            {
                AddRiskReason(Row.Reasons, TEXT("incoming_degree_at_least_20"));
            }
            return;
        }

        if (Row.TotalDegree >= MediumTotalDegreeThreshold
            || Row.ExternalDegree >= MediumExternalDegreeThreshold
            || Row.TotalIncomingCount >= MediumIncomingDegreeThreshold)
        {
            Row.RiskLevel = TEXT("medium");
            if (Row.TotalDegree >= MediumTotalDegreeThreshold)
            {
                AddRiskReason(Row.Reasons, TEXT("total_degree_at_least_15"));
            }
            if (Row.ExternalDegree >= MediumExternalDegreeThreshold)
            {
                AddRiskReason(Row.Reasons, TEXT("external_degree_at_least_7"));
            }
            if (Row.TotalIncomingCount >= MediumIncomingDegreeThreshold)
            {
                AddRiskReason(Row.Reasons, TEXT("incoming_degree_at_least_10"));
            }
            return;
        }

        Row.RiskLevel = TEXT("low");
        AddRiskReason(Row.Reasons, Row.TotalDegree == 0 ? TEXT("no_indexed_package_dependencies") : TEXT("below_coupling_risk_thresholds"));
    }

    TArray<TSharedPtr<FJsonValue>> SerializeReasons(const TArray<FString>& Reasons)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Reasons.Num());
        for (const FString& Reason : Reasons)
        {
            Values.Add(MakeShared<FJsonValueString>(Reason));
        }
        return Values;
    }

    TSharedRef<FJsonObject> SerializeAsset(const FCouplingRow& Row, const int32 Rank)
    {
        TSharedRef<FJsonObject> Asset = MakeShared<FJsonObject>();
        Asset->SetNumberField(TEXT("rank"), Rank);
        Asset->SetStringField(TEXT("objectPath"), Row.ObjectPath);
        Asset->SetStringField(TEXT("assetName"), Row.AssetName);
        Asset->SetStringField(TEXT("classPath"), Row.ClassPath);
        Asset->SetStringField(TEXT("packageName"), Row.PackageName);
        Asset->SetStringField(TEXT("packagePath"), Row.PackagePath);
        Asset->SetStringField(TEXT("contentScope"), TEXT("project"));
        Asset->SetNumberField(TEXT("internalOutgoingDependencyCount"), Row.InternalOutgoingCount);
        Asset->SetNumberField(TEXT("externalOutgoingDependencyCount"), Row.ExternalOutgoingCount);
        Asset->SetNumberField(TEXT("totalOutgoingDependencyCount"), Row.TotalOutgoingCount);
        Asset->SetNumberField(TEXT("internalIncomingDependencyCount"), Row.InternalIncomingCount);
        Asset->SetNumberField(TEXT("externalIncomingDependencyCount"), Row.ExternalIncomingCount);
        Asset->SetNumberField(TEXT("totalIncomingDependencyCount"), Row.TotalIncomingCount);
        Asset->SetNumberField(TEXT("internalDegree"), Row.InternalOutgoingCount + Row.InternalIncomingCount);
        Asset->SetNumberField(TEXT("externalDegree"), Row.ExternalDegree);
        Asset->SetNumberField(TEXT("totalDegree"), Row.TotalDegree);
        Asset->SetStringField(TEXT("riskLevel"), Row.RiskLevel);
        Asset->SetArrayField(TEXT("reasons"), SerializeReasons(Row.Reasons));
        return Asset;
    }
}

FAnalyzeBlueprintCouplingTool::FAnalyzeBlueprintCouplingTool()
    : FMCPToolBase(
        TEXT("AnalyzeBlueprintCoupling"),
        TEXT("Ranks project Blueprint assets by indexed package dependency coupling. This is static package-reference analysis, not runtime call coupling."))
{
}

UnrealMCP::FMCPResponse FAnalyzeBlueprintCouplingTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString RootPath;
    FString PackagePath;
    FString ObjectPath;
    int32 MaxResults = DefaultMaxResults;

    if (Request.Params.IsValid())
    {
        Request.Params->TryGetStringField(TEXT("rootPath"), RootPath);
        Request.Params->TryGetStringField(TEXT("packagePath"), PackagePath);
        Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath);

        double RequestedMaxResults = DefaultMaxResults;
        if (Request.Params->TryGetNumberField(TEXT("maxResults"), RequestedMaxResults))
        {
            MaxResults = FMath::Clamp(FMath::RoundToInt(RequestedMaxResults), 1, MaximumMaxResults);
        }
    }

    RootPath = NormalizePackagePath(RootPath);
    PackagePath = NormalizePackagePath(PackagePath);
    ObjectPath = ObjectPath.TrimStartAndEnd();

    if (!RootPath.IsEmpty() && !PackagePath.IsEmpty() && !RootPath.Equals(PackagePath, ESearchCase::IgnoreCase))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeBlueprintCoupling rootPath and packagePath must match when both are supplied."));
    }

    const FString ScopePath = !PackagePath.IsEmpty() ? PackagePath : RootPath;
    if (!ScopePath.IsEmpty() && !ScopePath.StartsWith(TEXT("/Game"), ESearchCase::IgnoreCase))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeBlueprintCoupling rootPath/packagePath must be a project package path beginning with /Game."));
    }

    TArray<FCouplingRow> Rows;
    FString Error;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            FSQLitePreparedStatement Statement(
                Database,
                TEXT("SELECT a.object_path, a.asset_name, a.class_path, a.package_name, a.package_path, "
                     "(SELECT COUNT(*) FROM asset_dependencies d "
                     " WHERE d.source_package_name = a.package_name "
                     " AND EXISTS (SELECT 1 FROM assets target "
                     "             WHERE target.package_name = d.target_package_name "
                     "             AND target.content_scope = 'project' AND target.is_blueprint = 1 "
                     "             AND (?2 = '' OR target.package_path = ?2 OR target.package_path LIKE ?2 || '/%'))), "
                     "(SELECT COUNT(*) FROM asset_dependencies d WHERE d.source_package_name = a.package_name), "
                     "(SELECT COUNT(*) FROM asset_dependencies d "
                     " WHERE d.target_package_name = a.package_name "
                     " AND EXISTS (SELECT 1 FROM assets source "
                     "             WHERE source.package_name = d.source_package_name "
                     "             AND source.content_scope = 'project' AND source.is_blueprint = 1 "
                     "             AND (?2 = '' OR source.package_path = ?2 OR source.package_path LIKE ?2 || '/%'))), "
                     "(SELECT COUNT(*) FROM asset_dependencies d WHERE d.target_package_name = a.package_name) "
                     "FROM assets a "
                     "WHERE a.content_scope = 'project' AND a.is_blueprint = 1 "
                     "AND (?1 = '' OR a.object_path = ?1 COLLATE NOCASE) "
                     "AND (?2 = '' OR a.package_path = ?2 OR a.package_path LIKE ?2 || '/%') "
                     "ORDER BY a.object_path ASC;"),
                ESQLitePreparedStatementFlags::None);

            if (!Statement.IsValid()
                || !Statement.SetBindingValueByIndex(1, ObjectPath)
                || !Statement.SetBindingValueByIndex(2, ScopePath))
            {
                OutExecError = TEXT("AnalyzeBlueprintCoupling could not prepare the project index query.");
                return false;
            }

            const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FCouplingRow Coupling;
                int32 InternalOutgoing = 0;
                int32 TotalOutgoing = 0;
                int32 InternalIncoming = 0;
                int32 TotalIncoming = 0;
                if (!Row.GetColumnValueByIndex(0, Coupling.ObjectPath)
                    || !Row.GetColumnValueByIndex(1, Coupling.AssetName)
                    || !Row.GetColumnValueByIndex(2, Coupling.ClassPath)
                    || !Row.GetColumnValueByIndex(3, Coupling.PackageName)
                    || !Row.GetColumnValueByIndex(4, Coupling.PackagePath)
                    || !Row.GetColumnValueByIndex(5, InternalOutgoing)
                    || !Row.GetColumnValueByIndex(6, TotalOutgoing)
                    || !Row.GetColumnValueByIndex(7, InternalIncoming)
                    || !Row.GetColumnValueByIndex(8, TotalIncoming))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }

                Coupling.InternalOutgoingCount = InternalOutgoing;
                Coupling.TotalOutgoingCount = TotalOutgoing;
                Coupling.ExternalOutgoingCount = FMath::Max(0, TotalOutgoing - InternalOutgoing);
                Coupling.InternalIncomingCount = InternalIncoming;
                Coupling.TotalIncomingCount = TotalIncoming;
                Coupling.ExternalIncomingCount = FMath::Max(0, TotalIncoming - InternalIncoming);
                Coupling.ExternalDegree = Coupling.ExternalOutgoingCount + Coupling.ExternalIncomingCount;
                Coupling.TotalDegree = TotalOutgoing + TotalIncoming;
                AssignRisk(Coupling);
                Rows.Add(MoveTemp(Coupling));
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });

            if (QueryResult == INDEX_NONE)
            {
                OutExecError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
                return false;
            }
            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("AnalyzeBlueprintCoupling failed to query the project index: %s"), *Error));
    }

    if (!ObjectPath.IsEmpty() && Rows.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AnalyzeBlueprintCoupling could not find the requested project Blueprint in the selected package scope."));
    }

    Rows.Sort([](const FCouplingRow& A, const FCouplingRow& B)
    {
        if (A.TotalDegree != B.TotalDegree)
        {
            return A.TotalDegree > B.TotalDegree;
        }
        if (A.ExternalDegree != B.ExternalDegree)
        {
            return A.ExternalDegree > B.ExternalDegree;
        }
        if (A.TotalIncomingCount != B.TotalIncomingCount)
        {
            return A.TotalIncomingCount > B.TotalIncomingCount;
        }
        if (A.TotalOutgoingCount != B.TotalOutgoingCount)
        {
            return A.TotalOutgoingCount > B.TotalOutgoingCount;
        }
        return A.ObjectPath < B.ObjectPath;
    });

    int32 HighRiskCount = 0;
    int32 MediumRiskCount = 0;
    int32 LowRiskCount = 0;
    int64 TotalInternalOutgoing = 0;
    int64 TotalExternalOutgoing = 0;
    int64 TotalInternalIncoming = 0;
    int64 TotalExternalIncoming = 0;
    int64 TotalDegreeSum = 0;
    int32 MaximumTotalDegree = 0;
    for (const FCouplingRow& Row : Rows)
    {
        HighRiskCount += Row.RiskLevel == TEXT("high") ? 1 : 0;
        MediumRiskCount += Row.RiskLevel == TEXT("medium") ? 1 : 0;
        LowRiskCount += Row.RiskLevel == TEXT("low") ? 1 : 0;
        TotalInternalOutgoing += Row.InternalOutgoingCount;
        TotalExternalOutgoing += Row.ExternalOutgoingCount;
        TotalInternalIncoming += Row.InternalIncomingCount;
        TotalExternalIncoming += Row.ExternalIncomingCount;
        TotalDegreeSum += Row.TotalDegree;
        MaximumTotalDegree = FMath::Max(MaximumTotalDegree, Row.TotalDegree);
    }

    const int32 MatchingAssetCount = Rows.Num();
    const int32 ReturnedAssetCount = FMath::Min(MatchingAssetCount, MaxResults);
    TArray<TSharedPtr<FJsonValue>> Assets;
    Assets.Reserve(ReturnedAssetCount);
    for (int32 Index = 0; Index < ReturnedAssetCount; ++Index)
    {
        Assets.Add(MakeShared<FJsonValueObject>(SerializeAsset(Rows[Index], Index + 1)));
    }

    TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("matchingAssetCount"), MatchingAssetCount);
    Summary->SetNumberField(TEXT("returnedAssetCount"), ReturnedAssetCount);
    Summary->SetBoolField(TEXT("truncated"), ReturnedAssetCount < MatchingAssetCount);
    Summary->SetNumberField(TEXT("highRiskAssetCount"), HighRiskCount);
    Summary->SetNumberField(TEXT("mediumRiskAssetCount"), MediumRiskCount);
    Summary->SetNumberField(TEXT("lowRiskAssetCount"), LowRiskCount);
    Summary->SetNumberField(TEXT("totalInternalOutgoingDependencies"), static_cast<double>(TotalInternalOutgoing));
    Summary->SetNumberField(TEXT("totalExternalOutgoingDependencies"), static_cast<double>(TotalExternalOutgoing));
    Summary->SetNumberField(TEXT("totalInternalIncomingDependencies"), static_cast<double>(TotalInternalIncoming));
    Summary->SetNumberField(TEXT("totalExternalIncomingDependencies"), static_cast<double>(TotalExternalIncoming));
    Summary->SetNumberField(TEXT("averageTotalDegree"), MatchingAssetCount > 0 ? static_cast<double>(TotalDegreeSum) / MatchingAssetCount : 0.0);
    Summary->SetNumberField(TEXT("maximumTotalDegree"), MaximumTotalDegree);

    TSharedRef<FJsonObject> Definitions = MakeShared<FJsonObject>();
    Definitions->SetStringField(TEXT("analysisType"), TEXT("indexed_package_dependency_coupling"));
    Definitions->SetStringField(TEXT("internalDependency"), TEXT("The opposite package is another project Blueprint inside rootPath/packagePath, or anywhere in /Game when no root is supplied."));
    Definitions->SetStringField(TEXT("externalDependency"), TEXT("The opposite package is outside that project Blueprint scope, including non-Blueprint, plugin, engine, or unresolved indexed packages."));
    Definitions->SetStringField(TEXT("limitation"), TEXT("Counts static indexed package dependencies only; they do not prove runtime calls, execution order, or runtime coupling."));

    TSharedRef<FJsonObject> Thresholds = MakeShared<FJsonObject>();
    Thresholds->SetNumberField(TEXT("highTotalDegree"), HighTotalDegreeThreshold);
    Thresholds->SetNumberField(TEXT("highExternalDegree"), HighExternalDegreeThreshold);
    Thresholds->SetNumberField(TEXT("highIncomingDegree"), HighIncomingDegreeThreshold);
    Thresholds->SetNumberField(TEXT("mediumTotalDegree"), MediumTotalDegreeThreshold);
    Thresholds->SetNumberField(TEXT("mediumExternalDegree"), MediumExternalDegreeThreshold);
    Thresholds->SetNumberField(TEXT("mediumIncomingDegree"), MediumIncomingDegreeThreshold);

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("analysisType"), TEXT("indexed_package_dependency_coupling"));
    Result->SetStringField(TEXT("rootPath"), ScopePath);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetNumberField(TEXT("maxResults"), MaxResults);
    Result->SetObjectField(TEXT("definitions"), Definitions);
    Result->SetObjectField(TEXT("riskThresholds"), Thresholds);
    Result->SetObjectField(TEXT("summary"), Summary);
    Result->SetArrayField(TEXT("assets"), Assets);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAnalyzeBlueprintCouplingTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> RootPathProperty = MakeShared<FJsonObject>();
    RootPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    RootPathProperty->SetStringField(TEXT("description"), TEXT("Optional project package-path prefix such as /Game/Features. Alias of packagePath."));
    Properties->SetObjectField(TEXT("rootPath"), RootPathProperty);

    TSharedRef<FJsonObject> PackagePathProperty = MakeShared<FJsonObject>();
    PackagePathProperty->SetStringField(TEXT("type"), TEXT("string"));
    PackagePathProperty->SetStringField(TEXT("description"), TEXT("Optional project package-path prefix such as /Game/Features. Alias of rootPath."));
    Properties->SetObjectField(TEXT("packagePath"), PackagePathProperty);

    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Optional exact Blueprint object path to analyze one asset while retaining the selected package scope as the internal boundary."));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);

    TSharedRef<FJsonObject> MaxResultsProperty = MakeShared<FJsonObject>();
    MaxResultsProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxResultsProperty->SetStringField(TEXT("description"), TEXT("Maximum ranked assets to return. Defaults to 50 and is clamped from 1 to 200."));
    MaxResultsProperty->SetNumberField(TEXT("default"), DefaultMaxResults);
    MaxResultsProperty->SetNumberField(TEXT("minimum"), 1);
    MaxResultsProperty->SetNumberField(TEXT("maximum"), MaximumMaxResults);
    Properties->SetObjectField(TEXT("maxResults"), MaxResultsProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetBoolField(TEXT("additionalProperties"), false);
    return Schema;
}
