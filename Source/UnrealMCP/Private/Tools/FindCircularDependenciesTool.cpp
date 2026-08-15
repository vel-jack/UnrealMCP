#include "Tools/FindCircularDependenciesTool.h"

#include "Dom/JsonObject.h"
#include "Index/UnrealMCPProjectIndex.h"
#include "SQLitePreparedStatement.h"
#include "Tools/IndexedQueryToolUtils.h"
#include "UnrealMCPModule.h"

namespace
{
    struct FIndexedBlueprint
    {
        FString PackageName;
        FString PackagePath;
        FString ObjectPath;
        FString AssetName;
    };

    struct FDependencyEdge
    {
        FString SourcePackageName;
        FString TargetPackageName;
    };

    struct FCircularComponent
    {
        TArray<FString> PackageNames;
        TArray<FDependencyEdge> InternalEdges;
        bool bSelfCycle = false;
    };

    FString NormalizePackagePath(FString PackagePath)
    {
        PackagePath.TrimStartAndEndInline();
        PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (PackagePath.Len() > 1 && PackagePath.EndsWith(TEXT("/")))
        {
            PackagePath.LeftChopInline(1);
        }
        return PackagePath;
    }

    bool ReadPositiveInteger(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* FieldName,
        const int32 DefaultValue,
        const int32 MaximumValue,
        int32& OutValue,
        FString& OutError)
    {
        double Number = DefaultValue;
        if (Params.IsValid() && Params->HasField(FieldName))
        {
            if (!Params->TryGetNumberField(FieldName, Number)
                || !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number))
                || Number < 1.0
                || Number > MaximumValue)
            {
                OutError = FString::Printf(TEXT("%s must be an integer from 1 to %d."), FieldName, MaximumValue);
                return false;
            }
        }

        OutValue = FMath::RoundToInt(Number);
        return true;
    }

    TSharedRef<FJsonObject> SerializeBlueprint(const FIndexedBlueprint& Blueprint)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("packageName"), Blueprint.PackageName);
        Json->SetStringField(TEXT("packagePath"), Blueprint.PackagePath);
        Json->SetStringField(TEXT("objectPath"), Blueprint.ObjectPath);
        Json->SetStringField(TEXT("assetName"), Blueprint.AssetName);
        return Json;
    }
}

FFindCircularDependenciesTool::FFindCircularDependenciesTool()
    : FMCPToolBase(
        TEXT("FindCircularDependencies"),
        TEXT("Finds deterministic circular dependency groups among indexed project Blueprints. Use the reported internal edges to identify dependency seams to break."))
{
}

UnrealMCP::FMCPResponse FFindCircularDependenciesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString RootPath;
    FString PackagePath;
    if (Request.Params.IsValid())
    {
        Request.Params->TryGetStringField(TEXT("rootPath"), RootPath);
        Request.Params->TryGetStringField(TEXT("packagePath"), PackagePath);
    }

    RootPath = NormalizePackagePath(RootPath);
    PackagePath = NormalizePackagePath(PackagePath);
    if (!RootPath.IsEmpty() && !PackagePath.IsEmpty() && RootPath != PackagePath)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindCircularDependencies accepts rootPath or packagePath; when both are supplied they must match."));
    }

    const FString PathFilter = !PackagePath.IsEmpty() ? PackagePath : RootPath;
    if (!PathFilter.IsEmpty() && !PathFilter.StartsWith(TEXT("/")))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindCircularDependencies rootPath/packagePath must be an Unreal package path beginning with '/', for example /Game/Features."));
    }

    int32 MinCycleSize = 2;
    int32 MaxResults = 50;
    FString ParameterError;
    if (!ReadPositiveInteger(Request.Params, TEXT("minCycleSize"), 2, 100000, MinCycleSize, ParameterError)
        || !ReadPositiveInteger(Request.Params, TEXT("maxResults"), 50, 1000, MaxResults, ParameterError))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ParameterError);
    }

    TMap<FString, FIndexedBlueprint> BlueprintsByPackage;
    TArray<FDependencyEdge> Edges;
    FString QueryError;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutError)
        {
            FString AssetSql = TEXT(
                "SELECT package_name, package_path, object_path, asset_name "
                "FROM assets WHERE is_blueprint = 1 AND content_scope = 'project'");
            if (!PathFilter.IsEmpty())
            {
                AssetSql += TEXT(" AND (package_path = ?1 OR substr(package_path, 1, length(?2)) = ?2)");
            }
            AssetSql += TEXT(" ORDER BY package_name ASC, object_path ASC;");

            FSQLitePreparedStatement AssetStatement(Database, *AssetSql, ESQLitePreparedStatementFlags::None);
            const FString ChildPathPrefix = PathFilter + TEXT("/");
            if (!AssetStatement.IsValid()
                || (!PathFilter.IsEmpty()
                    && (!AssetStatement.SetBindingValueByIndex(1, PathFilter)
                        || !AssetStatement.SetBindingValueByIndex(2, ChildPathPrefix))))
            {
                OutError = TEXT("FindCircularDependencies could not prepare the indexed Blueprint query.");
                return false;
            }

            const int64 AssetQueryResult = AssetStatement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FIndexedBlueprint Blueprint;
                if (!Row.GetColumnValueByIndex(0, Blueprint.PackageName)
                    || !Row.GetColumnValueByIndex(1, Blueprint.PackagePath)
                    || !Row.GetColumnValueByIndex(2, Blueprint.ObjectPath)
                    || !Row.GetColumnValueByIndex(3, Blueprint.AssetName))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }

                // A package normally owns one Blueprint asset. Keep the first row from the stable query if malformed data contains more.
                if (!BlueprintsByPackage.Contains(Blueprint.PackageName))
                {
                    const FString PackageName = Blueprint.PackageName;
                    BlueprintsByPackage.Add(PackageName, MoveTemp(Blueprint));
                }
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });

            if (AssetQueryResult == INDEX_NONE)
            {
                OutError = Database.GetLastError().IsEmpty() ? TEXT("FindCircularDependencies Blueprint query failed.") : Database.GetLastError();
                return false;
            }

            FSQLitePreparedStatement DependencyStatement(
                Database,
                TEXT("SELECT source_package_name, target_package_name FROM asset_dependencies ORDER BY source_package_name ASC, target_package_name ASC;"),
                ESQLitePreparedStatementFlags::None);
            if (!DependencyStatement.IsValid())
            {
                OutError = TEXT("FindCircularDependencies could not prepare the dependency query.");
                return false;
            }

            const int64 DependencyQueryResult = DependencyStatement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FDependencyEdge Edge;
                if (!Row.GetColumnValueByIndex(0, Edge.SourcePackageName)
                    || !Row.GetColumnValueByIndex(1, Edge.TargetPackageName))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }

                if (BlueprintsByPackage.Contains(Edge.SourcePackageName) && BlueprintsByPackage.Contains(Edge.TargetPackageName))
                {
                    Edges.Add(MoveTemp(Edge));
                }
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });

            if (DependencyQueryResult == INDEX_NONE)
            {
                OutError = Database.GetLastError().IsEmpty() ? TEXT("FindCircularDependencies dependency query failed.") : Database.GetLastError();
                return false;
            }

            return true;
        },
        QueryError);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("FindCircularDependencies failed to query the project index: %s"), *QueryError));
    }

    TArray<FString> PackageNames;
    BlueprintsByPackage.GetKeys(PackageNames);
    PackageNames.Sort();

    TMap<FString, TArray<FString>> Adjacency;
    TMap<FString, TArray<FString>> ReverseAdjacency;
    for (const FString& PackageName : PackageNames)
    {
        Adjacency.Add(PackageName);
        ReverseAdjacency.Add(PackageName);
    }
    for (const FDependencyEdge& Edge : Edges)
    {
        Adjacency.FindChecked(Edge.SourcePackageName).Add(Edge.TargetPackageName);
        ReverseAdjacency.FindChecked(Edge.TargetPackageName).Add(Edge.SourcePackageName);
    }
    for (const FString& PackageName : PackageNames)
    {
        Adjacency.FindChecked(PackageName).Sort();
        ReverseAdjacency.FindChecked(PackageName).Sort();
    }

    // Iterative Kosaraju traversal avoids consuming the C++ call stack on large projects.
    TSet<FString> Visited;
    TArray<FString> FinishOrder;
    for (const FString& Start : PackageNames)
    {
        if (Visited.Contains(Start))
        {
            continue;
        }

        struct FTraversalFrame
        {
            FString PackageName;
            int32 NextNeighborIndex = 0;
        };

        TArray<FTraversalFrame> Stack;
        Stack.Add({Start, 0});
        Visited.Add(Start);
        while (!Stack.IsEmpty())
        {
            FTraversalFrame& Frame = Stack.Last();
            const TArray<FString>& Neighbors = Adjacency.FindChecked(Frame.PackageName);
            if (Frame.NextNeighborIndex < Neighbors.Num())
            {
                const FString& Neighbor = Neighbors[Frame.NextNeighborIndex++];
                if (!Visited.Contains(Neighbor))
                {
                    Visited.Add(Neighbor);
                    Stack.Add({Neighbor, 0});
                }
                continue;
            }

            FinishOrder.Add(Frame.PackageName);
            Stack.Pop(EAllowShrinking::No);
        }
    }

    TArray<FCircularComponent> CircularComponents;
    TSet<FString> Assigned;
    for (int32 OrderIndex = FinishOrder.Num() - 1; OrderIndex >= 0; --OrderIndex)
    {
        const FString& Start = FinishOrder[OrderIndex];
        if (Assigned.Contains(Start))
        {
            continue;
        }

        TArray<FString> Component;
        TArray<FString> Stack{Start};
        Assigned.Add(Start);
        while (!Stack.IsEmpty())
        {
            const FString Current = Stack.Pop(EAllowShrinking::No);
            Component.Add(Current);
            const TArray<FString>& Neighbors = ReverseAdjacency.FindChecked(Current);
            for (int32 NeighborIndex = Neighbors.Num() - 1; NeighborIndex >= 0; --NeighborIndex)
            {
                const FString& Neighbor = Neighbors[NeighborIndex];
                if (!Assigned.Contains(Neighbor))
                {
                    Assigned.Add(Neighbor);
                    Stack.Add(Neighbor);
                }
            }
        }
        Component.Sort();

        TSet<FString> ComponentPackages;
        ComponentPackages.Reserve(Component.Num());
        for (const FString& PackageName : Component)
        {
            ComponentPackages.Add(PackageName);
        }
        TArray<FDependencyEdge> InternalEdges;
        bool bHasSelfEdge = false;
        for (const FString& SourcePackageName : Component)
        {
            for (const FString& TargetPackageName : Adjacency.FindChecked(SourcePackageName))
            {
                if (ComponentPackages.Contains(TargetPackageName))
                {
                    InternalEdges.Add({SourcePackageName, TargetPackageName});
                    bHasSelfEdge |= SourcePackageName == TargetPackageName;
                }
            }
        }

        const bool bIsCycle = Component.Num() > 1 || (Component.Num() == 1 && bHasSelfEdge);
        if (bIsCycle && Component.Num() >= MinCycleSize)
        {
            FCircularComponent& Circular = CircularComponents.AddDefaulted_GetRef();
            Circular.PackageNames = MoveTemp(Component);
            Circular.InternalEdges = MoveTemp(InternalEdges);
            Circular.bSelfCycle = Circular.PackageNames.Num() == 1;
        }
    }

    CircularComponents.Sort([](const FCircularComponent& Left, const FCircularComponent& Right)
    {
        if (Left.PackageNames.Num() != Right.PackageNames.Num())
        {
            return Left.PackageNames.Num() > Right.PackageNames.Num();
        }
        return Left.PackageNames[0] < Right.PackageNames[0];
    });

    int32 CyclicBlueprintCount = 0;
    int32 SelfCycleCount = 0;
    for (const FCircularComponent& Component : CircularComponents)
    {
        CyclicBlueprintCount += Component.PackageNames.Num();
        SelfCycleCount += Component.bSelfCycle ? 1 : 0;
    }

    const int32 TotalCycleCount = CircularComponents.Num();
    const int32 ReturnedCycleCount = FMath::Min(TotalCycleCount, MaxResults);
    TArray<TSharedPtr<FJsonValue>> CyclesJson;
    CyclesJson.Reserve(ReturnedCycleCount);
    for (int32 CycleIndex = 0; CycleIndex < ReturnedCycleCount; ++CycleIndex)
    {
        const FCircularComponent& Component = CircularComponents[CycleIndex];
        TArray<TSharedPtr<FJsonValue>> BlueprintJson;
        for (const FString& PackageName : Component.PackageNames)
        {
            BlueprintJson.Add(MakeShared<FJsonValueObject>(SerializeBlueprint(BlueprintsByPackage.FindChecked(PackageName))));
        }

        TArray<TSharedPtr<FJsonValue>> EdgeJson;
        for (const FDependencyEdge& Edge : Component.InternalEdges)
        {
            TSharedRef<FJsonObject> EdgeObject = MakeShared<FJsonObject>();
            EdgeObject->SetStringField(TEXT("sourcePackageName"), Edge.SourcePackageName);
            EdgeObject->SetStringField(TEXT("sourceObjectPath"), BlueprintsByPackage.FindChecked(Edge.SourcePackageName).ObjectPath);
            EdgeObject->SetStringField(TEXT("targetPackageName"), Edge.TargetPackageName);
            EdgeObject->SetStringField(TEXT("targetObjectPath"), BlueprintsByPackage.FindChecked(Edge.TargetPackageName).ObjectPath);
            EdgeJson.Add(MakeShared<FJsonValueObject>(EdgeObject));
        }

        TSharedRef<FJsonObject> CycleObject = MakeShared<FJsonObject>();
        CycleObject->SetNumberField(TEXT("cycleIndex"), CycleIndex);
        CycleObject->SetNumberField(TEXT("blueprintCount"), Component.PackageNames.Num());
        CycleObject->SetNumberField(TEXT("internalEdgeCount"), Component.InternalEdges.Num());
        CycleObject->SetBoolField(TEXT("selfCycle"), Component.bSelfCycle);
        CycleObject->SetStringField(
            TEXT("description"),
            Component.bSelfCycle
                ? FString::Printf(TEXT("%s directly depends on itself."), *Component.PackageNames[0])
                : FString::Printf(TEXT("%d Blueprints form a circular dependency group; inspect the internal edges to choose a dependency to invert or extract."), Component.PackageNames.Num()));
        CycleObject->SetStringField(
            TEXT("recommendedAction"),
            Component.bSelfCycle
                ? TEXT("Inspect the self-reference and remove it or replace it with runtime ownership that is not an asset dependency.")
                : TEXT("Break one internal edge by extracting a shared contract, using an interface/event boundary, or moving shared state to a lower-level dependency."));
        CycleObject->SetArrayField(TEXT("blueprints"), BlueprintJson);
        CycleObject->SetArrayField(TEXT("internalEdges"), EdgeJson);
        CyclesJson.Add(MakeShared<FJsonValueObject>(CycleObject));
    }

    const FUnrealMCPProjectIndex::FStatusSnapshot Snapshot = FUnrealMCPModule::Get().GetProjectIndex().GetStatusSnapshot();
    TSharedRef<FJsonObject> IndexMetadata = MakeShared<FJsonObject>();
    IndexMetadata->SetBoolField(TEXT("hasUsableIndex"), Snapshot.bHasUsableIndex);
    IndexMetadata->SetBoolField(TEXT("isDirty"), Snapshot.bIndexDirty);
    IndexMetadata->SetStringField(TEXT("indexedScope"), Snapshot.IndexedScope);
    IndexMetadata->SetNumberField(TEXT("schemaVersion"), Snapshot.SchemaVersion);
    IndexMetadata->SetStringField(TEXT("lastFullBuildUtc"), Snapshot.LastFullBuildUtc);
    IndexMetadata->SetStringField(TEXT("lastUpdateUtc"), Snapshot.LastUpdateUtc);

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetObjectField(TEXT("index"), IndexMetadata);
    Result->SetStringField(TEXT("rootPath"), PathFilter);
    Result->SetNumberField(TEXT("minCycleSize"), MinCycleSize);
    Result->SetNumberField(TEXT("maxResults"), MaxResults);
    Result->SetNumberField(TEXT("indexedBlueprintCount"), BlueprintsByPackage.Num());
    Result->SetNumberField(TEXT("indexedDependencyEdgeCount"), Edges.Num());
    Result->SetNumberField(TEXT("cycleCount"), TotalCycleCount);
    Result->SetNumberField(TEXT("returnedCycleCount"), ReturnedCycleCount);
    Result->SetNumberField(TEXT("cyclicBlueprintCount"), CyclicBlueprintCount);
    Result->SetNumberField(TEXT("selfCycleCount"), SelfCycleCount);
    Result->SetBoolField(TEXT("truncated"), ReturnedCycleCount < TotalCycleCount);
    Result->SetStringField(
        TEXT("summaryText"),
        TotalCycleCount == 0
            ? TEXT("No circular dependencies were found among the filtered indexed project Blueprints.")
            : FString::Printf(TEXT("Found %d circular dependency group(s) involving %d Blueprint(s)."), TotalCycleCount, CyclicBlueprintCount));
    Result->SetStringField(
        TEXT("nextStepHint"),
        TotalCycleCount == 0
            ? TEXT("Expand or remove rootPath/packagePath if you need to inspect a wider project scope.")
            : TEXT("Start with the largest cycle and inspect its internalEdges; break one edge at a stable interface, event, or ownership boundary."));
    Result->SetArrayField(TEXT("cycles"), CyclesJson);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindCircularDependenciesTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> RootPathProperty = MakeShared<FJsonObject>();
    RootPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    RootPathProperty->SetStringField(TEXT("description"), TEXT("Optional project package-path prefix to analyze, for example /Game/Features. Alias of packagePath."));
    Properties->SetObjectField(TEXT("rootPath"), RootPathProperty);

    TSharedRef<FJsonObject> PackagePathProperty = MakeShared<FJsonObject>();
    PackagePathProperty->SetStringField(TEXT("type"), TEXT("string"));
    PackagePathProperty->SetStringField(TEXT("description"), TEXT("Optional project package-path prefix to analyze recursively, for example /Game/Features."));
    Properties->SetObjectField(TEXT("packagePath"), PackagePathProperty);

    TSharedRef<FJsonObject> MinCycleSizeProperty = MakeShared<FJsonObject>();
    MinCycleSizeProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MinCycleSizeProperty->SetNumberField(TEXT("minimum"), 1);
    MinCycleSizeProperty->SetNumberField(TEXT("default"), 2);
    MinCycleSizeProperty->SetStringField(TEXT("description"), TEXT("Minimum number of Blueprints in a returned circular group. Defaults to 2; use 1 to include direct self-cycles."));
    Properties->SetObjectField(TEXT("minCycleSize"), MinCycleSizeProperty);

    TSharedRef<FJsonObject> MaxResultsProperty = MakeShared<FJsonObject>();
    MaxResultsProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxResultsProperty->SetNumberField(TEXT("minimum"), 1);
    MaxResultsProperty->SetNumberField(TEXT("maximum"), 1000);
    MaxResultsProperty->SetNumberField(TEXT("default"), 50);
    MaxResultsProperty->SetStringField(TEXT("description"), TEXT("Maximum circular groups returned after deterministic size/name ordering. Defaults to 50."));
    Properties->SetObjectField(TEXT("maxResults"), MaxResultsProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
    return Schema;
}
