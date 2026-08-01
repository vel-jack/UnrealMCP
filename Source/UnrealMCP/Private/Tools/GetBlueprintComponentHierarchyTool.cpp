#include "Tools/GetBlueprintComponentHierarchyTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/IndexedQueryToolUtils.h"

FGetBlueprintComponentHierarchyTool::FGetBlueprintComponentHierarchyTool()
    : FMCPToolBase(TEXT("GetBlueprintComponentHierarchy"), TEXT("Returns an indexed component hierarchy summary for one Blueprint asset.")) 
{
}

UnrealMCP::FMCPResponse FGetBlueprintComponentHierarchyTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetBlueprintComponentHierarchy requires params.objectPath."));
    }

    FString ObjectPath;
    if (!Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetBlueprintComponentHierarchy requires a non-empty params.objectPath."));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    const FAssetData Asset = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
    if (!Asset.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetBlueprintComponentHierarchy could not resolve the Blueprint asset from params.objectPath."));
    }

    TArray<TSharedPtr<FJsonValue>> Components;
    int32 Count = 0;
    FString Error;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            FSQLitePreparedStatement Statement(
                Database,
                TEXT("SELECT variable_name, component_class_path, parent_variable_name, is_default_scene_root, child_count "
                     "FROM blueprint_components WHERE blueprint_object_path = ?1 ORDER BY variable_name ASC;"),
                ESQLitePreparedStatementFlags::None);
            if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ObjectPath))
            {
                OutExecError = TEXT("GetBlueprintComponentHierarchy could not prepare the project index query.");
                return false;
            }

            const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FString VariableName;
                FString ComponentClassPath;
                FString ParentVariableName;
                int32 bIsDefaultSceneRoot = 0;
                int32 ChildCount = 0;

                if (!Row.GetColumnValueByIndex(0, VariableName)
                    || !Row.GetColumnValueByIndex(1, ComponentClassPath)
                    || !Row.GetColumnValueByIndex(2, ParentVariableName)
                    || !Row.GetColumnValueByIndex(3, bIsDefaultSceneRoot)
                    || !Row.GetColumnValueByIndex(4, ChildCount))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }

                TSharedRef<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
                ComponentObject->SetStringField(TEXT("variableName"), VariableName);
                ComponentObject->SetStringField(TEXT("componentClassPath"), ComponentClassPath);
                ComponentObject->SetStringField(TEXT("parentVariableName"), ParentVariableName);
                ComponentObject->SetBoolField(TEXT("isDefaultSceneRoot"), bIsDefaultSceneRoot != 0);
                ComponentObject->SetNumberField(TEXT("childCount"), ChildCount);
                Components.Add(MakeShared<FJsonValueObject>(ComponentObject));
                ++Count;
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
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("GetBlueprintComponentHierarchy failed to query the project index: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
    Result->SetNumberField(TEXT("count"), Count);
    Result->SetArrayField(TEXT("components"), Components);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetBlueprintComponentHierarchyTool::BuildInputSchema() const
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
