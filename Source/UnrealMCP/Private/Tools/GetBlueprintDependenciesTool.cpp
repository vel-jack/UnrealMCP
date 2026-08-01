#include "Tools/GetBlueprintDependenciesTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/IndexedQueryToolUtils.h"

FGetBlueprintDependenciesTool::FGetBlueprintDependenciesTool()
    : FMCPToolBase(TEXT("GetBlueprintDependencies"), TEXT("Returns an indexed dependency summary for one Blueprint asset.")) 
{
}

UnrealMCP::FMCPResponse FGetBlueprintDependenciesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetBlueprintDependencies requires params.objectPath."));
    }

    FString ObjectPath;
    if (!Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetBlueprintDependencies requires a non-empty params.objectPath."));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    const FAssetData Asset = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
    if (!Asset.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetBlueprintDependencies could not resolve the Blueprint asset from params.objectPath."));
    }

    TArray<TSharedPtr<FJsonValue>> BlueprintDependencies;
    TArray<TSharedPtr<FJsonValue>> NonBlueprintDependencies;

    int32 TotalCount = 0;
    int32 BlueprintCount = 0;
    int32 NonBlueprintCount = 0;
    FString Error;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            FSQLitePreparedStatement Statement(
                Database,
                TEXT("SELECT d.target_package_name, a.object_path, a.asset_name, a.class_path, a.content_scope, a.is_blueprint "
                     "FROM asset_dependencies d "
                     "LEFT JOIN assets a ON a.package_name = d.target_package_name "
                     "WHERE d.source_package_name = ?1 "
                     "ORDER BY d.target_package_name ASC;"),
                ESQLitePreparedStatementFlags::None);
            if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, Asset.PackageName.ToString()))
            {
                OutExecError = TEXT("GetBlueprintDependencies could not prepare the project index query.");
                return false;
            }

            const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FString TargetPackageName;
                FString IndexedObjectPath;
                FString AssetName;
                FString ClassPath;
                FString ContentScope;
                int32 bIsBlueprint = 0;

                if (!Row.GetColumnValueByIndex(0, TargetPackageName)
                    || !Row.GetColumnValueByIndex(1, IndexedObjectPath)
                    || !Row.GetColumnValueByIndex(2, AssetName)
                    || !Row.GetColumnValueByIndex(3, ClassPath)
                    || !Row.GetColumnValueByIndex(4, ContentScope)
                    || !Row.GetColumnValueByIndex(5, bIsBlueprint))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }

                TSharedRef<FJsonObject> DependencyObject = MakeShared<FJsonObject>();
                DependencyObject->SetStringField(TEXT("packageName"), TargetPackageName);
                DependencyObject->SetStringField(TEXT("contentScope"), ContentScope.IsEmpty() ? UnrealMCP::IndexedQueryToolUtils::GetContentScopeFromPackageName(TargetPackageName) : ContentScope);
                DependencyObject->SetStringField(TEXT("objectPath"), IndexedObjectPath);
                DependencyObject->SetStringField(TEXT("assetName"), AssetName);
                DependencyObject->SetStringField(TEXT("classPath"), ClassPath);
                DependencyObject->SetBoolField(TEXT("isBlueprint"), bIsBlueprint != 0);

                ++TotalCount;
                if (bIsBlueprint != 0)
                {
                    ++BlueprintCount;
                    BlueprintDependencies.Add(MakeShared<FJsonValueObject>(DependencyObject));
                }
                else
                {
                    ++NonBlueprintCount;
                    NonBlueprintDependencies.Add(MakeShared<FJsonValueObject>(DependencyObject));
                }

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
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("GetBlueprintDependencies failed to query the project index: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
    Result->SetNumberField(TEXT("dependencyCount"), TotalCount);
    Result->SetNumberField(TEXT("blueprintDependencyCount"), BlueprintCount);
    Result->SetNumberField(TEXT("nonBlueprintDependencyCount"), NonBlueprintCount);
    Result->SetArrayField(TEXT("blueprintDependencies"), BlueprintDependencies);
    Result->SetArrayField(TEXT("nonBlueprintDependencies"), NonBlueprintDependencies);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetBlueprintDependenciesTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Blueprint asset object path, for example /Game/BP_MyActor.BP_MyActor"));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
