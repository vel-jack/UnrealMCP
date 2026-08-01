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
    int32 BlueprintNeighborCount = 0;
    int32 NonBlueprintNeighborCount = 0;
    FString Error;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            if (!UnrealMCP::IndexedQueryToolUtils::QueryAssetByPackageName(Database, PackageName.ToString(), StartAsset, OutExecError) || !StartAsset.IsValid())
            {
                return false;
            }

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

            if (!CollectPackages(TEXT("SELECT target_package_name FROM asset_dependencies WHERE source_package_name = ?1 ORDER BY target_package_name ASC;"))
                || !CollectPackages(TEXT("SELECT source_package_name FROM asset_dependencies WHERE target_package_name = ?1 ORDER BY source_package_name ASC;")))
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
    Result->SetNumberField(TEXT("limit"), Limit);
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
