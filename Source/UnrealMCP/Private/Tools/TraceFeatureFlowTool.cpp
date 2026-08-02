#include "Tools/TraceFeatureFlowTool.h"

#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    void IncrementCount(TMap<FString, int32>& Counts, const FString& Key)
    {
        int32& Value = Counts.FindOrAdd(Key);
        ++Value;
    }

    void IncrementDepthCount(TMap<int32, int32>& Counts, const int32 Depth)
    {
        int32& Value = Counts.FindOrAdd(Depth);
        ++Value;
    }
}

FTraceFeatureFlowTool::FTraceFeatureFlowTool()
    : FMCPToolBase(TEXT("TraceFeatureFlow"), TEXT("Traces a small indexed feature flow around a starting Blueprint or asset using dependencies and referencers.")) 
{
}

UnrealMCP::FMCPResponse FTraceFeatureFlowTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FName PackageName;
    if (!UnrealMCP::IndexedQueryToolUtils::ResolvePackageName(Request.Params, ObjectPath, PackageName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("TraceFeatureFlow requires params.objectPath or params.packageName."));
    }

    int32 MaxDepth = 2;
    int32 MaxNodes = 20;
    if (Request.Params.IsValid())
    {
        Request.Params->TryGetNumberField(TEXT("maxDepth"), MaxDepth);
        Request.Params->TryGetNumberField(TEXT("maxNodes"), MaxNodes);
    }
    MaxDepth = FMath::Clamp(MaxDepth, 1, 3);
    MaxNodes = FMath::Clamp(MaxNodes, 1, 50);

    TArray<FString> Frontier;
    Frontier.Add(PackageName.ToString());

    TSet<FString> VisitedPackages;
    VisitedPackages.Add(PackageName.ToString());

    TArray<TSharedPtr<FJsonValue>> Nodes;
    TArray<TSharedPtr<FJsonValue>> Edges;
    TMap<FString, int32> EdgeKindCounts;
    TMap<int32, int32> DepthCounts;
    TMap<FString, int32> ScopeCounts;
    bool bTruncated = false;
    TSharedPtr<FJsonObject> StartAsset;

    FString Error;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            for (int32 Depth = 0; Depth < MaxDepth && Frontier.Num() > 0 && VisitedPackages.Num() < MaxNodes; ++Depth)
            {
                TArray<FString> NextFrontier;

                for (const FString& CurrentPackage : Frontier)
                {
                    if (VisitedPackages.Num() > MaxNodes)
                    {
                        break;
                    }

                    TSharedPtr<FJsonObject> CurrentAsset;
                    FString AssetError;
                    if (UnrealMCP::IndexedQueryToolUtils::QueryAssetByPackageName(Database, CurrentPackage, CurrentAsset, AssetError) && CurrentAsset.IsValid())
                    {
                        if (Depth == 0)
                        {
                            StartAsset = CurrentAsset;
                        }

                        TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
                        NodeObject->SetStringField(TEXT("packageName"), CurrentPackage);
                        NodeObject->SetNumberField(TEXT("depth"), Depth);
                        NodeObject->SetObjectField(TEXT("asset"), CurrentAsset.ToSharedRef());
                        Nodes.Add(MakeShared<FJsonValueObject>(NodeObject));
                        IncrementDepthCount(DepthCounts, Depth);
                        IncrementCount(ScopeCounts, CurrentAsset->GetStringField(TEXT("contentScope")).IsEmpty() ? TEXT("unknown") : CurrentAsset->GetStringField(TEXT("contentScope")).ToLower());
                    }

                    auto ExpandQuery = [&](const TCHAR* Sql, const TCHAR* EdgeKind) -> bool
                    {
                        FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
                        if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, CurrentPackage))
                        {
                            return false;
                        }

                        return Statement.Execute([&](const FSQLitePreparedStatement& Row)
                        {
                            FString RelatedPackage;
                            if (!Row.GetColumnValueByIndex(0, RelatedPackage))
                            {
                                return ESQLitePreparedStatementExecuteRowResult::Error;
                            }

                            TSharedRef<FJsonObject> EdgeObject = MakeShared<FJsonObject>();
                            EdgeObject->SetStringField(TEXT("fromPackage"), CurrentPackage);
                            EdgeObject->SetStringField(TEXT("toPackage"), RelatedPackage);
                            EdgeObject->SetStringField(TEXT("kind"), EdgeKind);
                            EdgeObject->SetNumberField(TEXT("depth"), Depth);
                            Edges.Add(MakeShared<FJsonValueObject>(EdgeObject));
                            IncrementCount(EdgeKindCounts, EdgeKind);

                            if (VisitedPackages.Num() < MaxNodes && !VisitedPackages.Contains(RelatedPackage))
                            {
                                VisitedPackages.Add(RelatedPackage);
                                NextFrontier.Add(RelatedPackage);
                            }

                            return ESQLitePreparedStatementExecuteRowResult::Continue;
                        }) != INDEX_NONE;
                    };

                    if (!ExpandQuery(TEXT("SELECT target_package_name FROM asset_dependencies WHERE source_package_name = ?1 ORDER BY target_package_name ASC;"), TEXT("depends_on"))
                        || !ExpandQuery(TEXT("SELECT source_package_name FROM asset_dependencies WHERE target_package_name = ?1 ORDER BY source_package_name ASC;"), TEXT("referenced_by")))
                    {
                        OutExecError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
                        return false;
                    }
                }

                Frontier = MoveTemp(NextFrontier);
            }

            bTruncated = VisitedPackages.Num() >= MaxNodes;
            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("TraceFeatureFlow failed to query the project index: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("packageName"), PackageName.ToString());
    Result->SetNumberField(TEXT("maxDepth"), MaxDepth);
    Result->SetNumberField(TEXT("maxNodes"), MaxNodes);
    Result->SetNumberField(TEXT("nodeCount"), Nodes.Num());
    Result->SetNumberField(TEXT("edgeCount"), Edges.Num());
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    if (StartAsset.IsValid())
    {
        Result->SetObjectField(TEXT("startAsset"), StartAsset.ToSharedRef());
    }

    TSharedRef<FJsonObject> EdgeCountsObject = MakeShared<FJsonObject>();
    for (const TPair<FString, int32>& Pair : EdgeKindCounts)
    {
        EdgeCountsObject->SetNumberField(Pair.Key, Pair.Value);
    }
    Result->SetObjectField(TEXT("edgeKindCounts"), EdgeCountsObject);

    TSharedRef<FJsonObject> DepthCountsObject = MakeShared<FJsonObject>();
    for (const TPair<int32, int32>& Pair : DepthCounts)
    {
        DepthCountsObject->SetNumberField(FString::FromInt(Pair.Key), Pair.Value);
    }
    Result->SetObjectField(TEXT("depthCounts"), DepthCountsObject);

    TSharedRef<FJsonObject> ScopeBreakdownObject = MakeShared<FJsonObject>();
    for (const TPair<FString, int32>& Pair : ScopeCounts)
    {
        ScopeBreakdownObject->SetNumberField(Pair.Key, Pair.Value);
    }
    Result->SetObjectField(TEXT("scopeBreakdown"), ScopeBreakdownObject);
    Result->SetArrayField(TEXT("nodes"), Nodes);
    Result->SetArrayField(TEXT("edges"), Edges);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FTraceFeatureFlowTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Optional asset object path, for example /Game/BP_MyActor.BP_MyActor."));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);

    TSharedRef<FJsonObject> PackageNameProperty = MakeShared<FJsonObject>();
    PackageNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    PackageNameProperty->SetStringField(TEXT("description"), TEXT("Optional package name when objectPath is not provided."));
    Properties->SetObjectField(TEXT("packageName"), PackageNameProperty);

    TSharedRef<FJsonObject> MaxDepthProperty = MakeShared<FJsonObject>();
    MaxDepthProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxDepthProperty->SetStringField(TEXT("description"), TEXT("Optional max traversal depth from 1 to 3. Defaults to 2."));
    Properties->SetObjectField(TEXT("maxDepth"), MaxDepthProperty);

    TSharedRef<FJsonObject> MaxNodesProperty = MakeShared<FJsonObject>();
    MaxNodesProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxNodesProperty->SetStringField(TEXT("description"), TEXT("Optional max nodes to emit. Defaults to 20."));
    Properties->SetObjectField(TEXT("maxNodes"), MaxNodesProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
