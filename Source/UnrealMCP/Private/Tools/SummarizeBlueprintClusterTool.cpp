#include "Tools/SummarizeBlueprintClusterTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    bool ResolveStartAsset(const TSharedPtr<FJsonObject>& Params, FString& OutObjectPath, FName& OutPackageName)
    {
        return UnrealMCP::IndexedQueryToolUtils::ResolvePackageName(Params, OutObjectPath, OutPackageName);
    }

    void IncrementScopeCount(TMap<FString, int32>& ScopeCounts, const FString& Scope)
    {
        const FString Key = Scope.IsEmpty() ? TEXT("unknown") : Scope.ToLower();
        int32& Count = ScopeCounts.FindOrAdd(Key);
        ++Count;
    }
}

FSummarizeBlueprintClusterTool::FSummarizeBlueprintClusterTool()
    : FMCPToolBase(TEXT("SummarizeBlueprintCluster"), TEXT("Summarizes the nearby indexed asset cluster around a starting Blueprint or asset.")) 
{
}

UnrealMCP::FMCPResponse FSummarizeBlueprintClusterTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FName PackageName;
    if (!ResolveStartAsset(Request.Params, ObjectPath, PackageName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("SummarizeBlueprintCluster requires params.objectPath or params.packageName."));
    }

    const int32 Limit = UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params, 25);

    TSet<FString> NeighborPackages;
    TArray<FString> OrderedNeighbors;

    auto AddNeighbor = [&NeighborPackages, &OrderedNeighbors, Limit](const FString& Package)
    {
        if (Package.IsEmpty() || NeighborPackages.Num() >= Limit)
        {
            return;
        }

        if (!NeighborPackages.Contains(Package))
        {
            NeighborPackages.Add(Package);
            OrderedNeighbors.Add(Package);
        }
    };

    TSharedPtr<FJsonObject> StartAsset;
    TArray<TSharedPtr<FJsonValue>> NeighborAssets;
    TArray<TSharedPtr<FJsonValue>> DependencyPreview;
    TArray<TSharedPtr<FJsonValue>> ReferencerPreview;
    int32 BlueprintNeighborCount = 0;
    int32 NonBlueprintNeighborCount = 0;
    int32 DirectDependencyCount = 0;
    int32 DirectReferencerCount = 0;
    TMap<FString, int32> ScopeCounts;
    FString Error;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            if (!UnrealMCP::IndexedQueryToolUtils::QueryAssetByPackageName(Database, PackageName.ToString(), StartAsset, OutExecError) || !StartAsset.IsValid())
            {
                return false;
            }

            IncrementScopeCount(ScopeCounts, StartAsset->GetStringField(TEXT("contentScope")));

            auto CollectPackages = [&Database, &PackageName, &AddNeighbor](const TCHAR* Sql) -> bool
            {
                FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, PackageName.ToString()))
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

                    AddNeighbor(RelatedPackage);
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                }) != INDEX_NONE;
            };

            auto CollectPreview = [&Database, &PackageName, Limit](const TCHAR* Sql, TArray<TSharedPtr<FJsonValue>>& OutPreview, int32& OutCount) -> bool
            {
                FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, PackageName.ToString()))
                {
                    return false;
                }

                return Statement.Execute([&](const FSQLitePreparedStatement& Row)
                {
                    FString RelatedPackageName;
                    FString RelatedObjectPath;
                    FString RelatedAssetName;
                    FString RelatedClassPath;
                    FString RelatedContentScope;
                    int32 bRelatedIsBlueprint = 0;

                    if (!Row.GetColumnValueByIndex(0, RelatedPackageName)
                        || !Row.GetColumnValueByIndex(1, RelatedObjectPath)
                        || !Row.GetColumnValueByIndex(2, RelatedAssetName)
                        || !Row.GetColumnValueByIndex(3, RelatedClassPath)
                        || !Row.GetColumnValueByIndex(4, RelatedContentScope)
                        || !Row.GetColumnValueByIndex(5, bRelatedIsBlueprint))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }

                    ++OutCount;
                    if (OutPreview.Num() < Limit)
                    {
                        TSharedRef<FJsonObject> ItemObject = MakeShared<FJsonObject>();
                        ItemObject->SetStringField(TEXT("packageName"), RelatedPackageName);
                        ItemObject->SetStringField(TEXT("objectPath"), RelatedObjectPath);
                        ItemObject->SetStringField(TEXT("assetName"), RelatedAssetName);
                        ItemObject->SetStringField(TEXT("classPath"), RelatedClassPath);
                        ItemObject->SetStringField(TEXT("contentScope"), RelatedContentScope);
                        ItemObject->SetBoolField(TEXT("isBlueprint"), bRelatedIsBlueprint != 0);
                        OutPreview.Add(MakeShared<FJsonValueObject>(ItemObject));
                    }

                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                }) != INDEX_NONE;
            };

            if (!CollectPackages(TEXT("SELECT target_package_name FROM asset_dependencies WHERE source_package_name = ?1 ORDER BY target_package_name ASC;"))
                || !CollectPackages(TEXT("SELECT source_package_name FROM asset_dependencies WHERE target_package_name = ?1 ORDER BY source_package_name ASC;"))
                || !CollectPreview(
                    TEXT("SELECT d.target_package_name, COALESCE(a.object_path, ''), COALESCE(a.asset_name, ''), COALESCE(a.class_path, ''), COALESCE(a.content_scope, ''), COALESCE(a.is_blueprint, 0) "
                         "FROM asset_dependencies d "
                         "LEFT JOIN assets a ON a.package_name = d.target_package_name "
                         "WHERE d.source_package_name = ?1 "
                         "ORDER BY d.target_package_name ASC;"),
                    DependencyPreview,
                    DirectDependencyCount)
                || !CollectPreview(
                    TEXT("SELECT d.source_package_name, COALESCE(a.object_path, ''), COALESCE(a.asset_name, ''), COALESCE(a.class_path, ''), COALESCE(a.content_scope, ''), COALESCE(a.is_blueprint, 0) "
                         "FROM asset_dependencies d "
                         "LEFT JOIN assets a ON a.package_name = d.source_package_name "
                         "WHERE d.target_package_name = ?1 "
                         "ORDER BY d.source_package_name ASC;"),
                    ReferencerPreview,
                    DirectReferencerCount))
            {
                OutExecError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
                return false;
            }

            for (const FString& NeighborPackage : OrderedNeighbors)
            {
                TSharedPtr<FJsonObject> NeighborAsset;
                FString NeighborError;
                if (!UnrealMCP::IndexedQueryToolUtils::QueryAssetByPackageName(Database, NeighborPackage, NeighborAsset, NeighborError) || !NeighborAsset.IsValid())
                {
                    continue;
                }

                const bool bIsBlueprint = NeighborAsset->GetBoolField(TEXT("isBlueprint"));
                BlueprintNeighborCount += bIsBlueprint ? 1 : 0;
                NonBlueprintNeighborCount += bIsBlueprint ? 0 : 1;
                IncrementScopeCount(ScopeCounts, NeighborAsset->GetStringField(TEXT("contentScope")));
                NeighborAssets.Add(MakeShared<FJsonValueObject>(NeighborAsset.ToSharedRef()));
            }

            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("SummarizeBlueprintCluster failed to query the project index: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetObjectField(TEXT("startAsset"), StartAsset.ToSharedRef());
    Result->SetStringField(TEXT("packageName"), PackageName.ToString());
    Result->SetNumberField(TEXT("neighborCount"), NeighborAssets.Num());
    Result->SetNumberField(TEXT("blueprintNeighborCount"), BlueprintNeighborCount);
    Result->SetNumberField(TEXT("nonBlueprintNeighborCount"), NonBlueprintNeighborCount);
    Result->SetNumberField(TEXT("directDependencyCount"), DirectDependencyCount);
    Result->SetNumberField(TEXT("directReferencerCount"), DirectReferencerCount);
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetArrayField(TEXT("dependencyPreview"), DependencyPreview);
    Result->SetArrayField(TEXT("referencerPreview"), ReferencerPreview);

    TSharedRef<FJsonObject> ScopeBreakdown = MakeShared<FJsonObject>();
    for (const TPair<FString, int32>& Pair : ScopeCounts)
    {
        ScopeBreakdown->SetNumberField(Pair.Key, Pair.Value);
    }
    Result->SetObjectField(TEXT("scopeBreakdown"), ScopeBreakdown);
    Result->SetArrayField(TEXT("neighbors"), NeighborAssets);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FSummarizeBlueprintClusterTool::BuildInputSchema() const
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

    TSharedRef<FJsonObject> LimitProperty = MakeShared<FJsonObject>();
    LimitProperty->SetStringField(TEXT("type"), TEXT("integer"));
    LimitProperty->SetStringField(TEXT("description"), TEXT("Optional max number of neighbor assets. Defaults to 25."));
    Properties->SetObjectField(TEXT("limit"), LimitProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
