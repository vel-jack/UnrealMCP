#include "Tools/GetBlueprintComponentHierarchyTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    struct FIndexedComponent
    {
        FString VariableName;
        FString ComponentClassPath;
        FString ComponentBlueprintPath;
        FString TemplateName;
        FString TemplatePath;
        FString ParentVariableName;
        FString AttachSocketName;
        FString CreationSource;
        FString RelativeLocation;
        FString RelativeRotation;
        FString RelativeScale;
        FString Mobility;
        bool bIsSceneComponent = false;
        bool bIsDefaultSceneRoot = false;
        int32 ChildCount = 0;
    };
}

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

    TArray<FIndexedComponent> IndexedComponents;
    TMap<FString, TArray<TSharedPtr<FJsonValue>>> UsageByComponent;
    FString Error;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            FSQLitePreparedStatement Statement(
                Database,
                TEXT("SELECT variable_name, component_class_path, component_blueprint_path, template_name, template_path, "
                     "parent_variable_name, attach_socket_name, creation_source, is_scene_component, is_default_scene_root, child_count, "
                     "relative_location, relative_rotation, relative_scale, mobility "
                     "FROM blueprint_components WHERE blueprint_object_path = ?1 ORDER BY variable_name ASC;"),
                ESQLitePreparedStatementFlags::None);
            if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ObjectPath))
            {
                OutExecError = TEXT("GetBlueprintComponentHierarchy could not prepare the project index query.");
                return false;
            }

            const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FIndexedComponent Component;
                int32 bIsSceneComponent = 0;
                int32 bIsDefaultSceneRoot = 0;
                if (!Row.GetColumnValueByIndex(0, Component.VariableName)
                    || !Row.GetColumnValueByIndex(1, Component.ComponentClassPath)
                    || !Row.GetColumnValueByIndex(2, Component.ComponentBlueprintPath)
                    || !Row.GetColumnValueByIndex(3, Component.TemplateName)
                    || !Row.GetColumnValueByIndex(4, Component.TemplatePath)
                    || !Row.GetColumnValueByIndex(5, Component.ParentVariableName)
                    || !Row.GetColumnValueByIndex(6, Component.AttachSocketName)
                    || !Row.GetColumnValueByIndex(7, Component.CreationSource)
                    || !Row.GetColumnValueByIndex(8, bIsSceneComponent)
                    || !Row.GetColumnValueByIndex(9, bIsDefaultSceneRoot)
                    || !Row.GetColumnValueByIndex(10, Component.ChildCount)
                    || !Row.GetColumnValueByIndex(11, Component.RelativeLocation)
                    || !Row.GetColumnValueByIndex(12, Component.RelativeRotation)
                    || !Row.GetColumnValueByIndex(13, Component.RelativeScale)
                    || !Row.GetColumnValueByIndex(14, Component.Mobility))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }
                Component.bIsSceneComponent = bIsSceneComponent != 0;
                Component.bIsDefaultSceneRoot = bIsDefaultSceneRoot != 0;
                IndexedComponents.Add(MoveTemp(Component));
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });

            if (QueryResult == INDEX_NONE)
            {
                OutExecError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
                return false;
            }

            FSQLitePreparedStatement UsageStatement(
                Database,
                TEXT("SELECT member_name, graph_name, node_guid, node_title, node_type "
                     "FROM blueprint_nodes WHERE blueprint_object_path = ?1 AND member_name IS NOT NULL AND member_name != '' "
                     "ORDER BY member_name ASC, graph_name ASC, node_title ASC;"),
                ESQLitePreparedStatementFlags::None);
            if (!UsageStatement.IsValid() || !UsageStatement.SetBindingValueByIndex(1, ObjectPath))
            {
                OutExecError = TEXT("GetBlueprintComponentHierarchy could not prepare the component usage query.");
                return false;
            }

            const TSet<FString> ComponentNames = [&IndexedComponents]()
            {
                TSet<FString> Names;
                for (const FIndexedComponent& Component : IndexedComponents)
                {
                    Names.Add(Component.VariableName);
                }
                return Names;
            }();

            const int64 UsageResult = UsageStatement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FString MemberName;
                FString GraphName;
                FString NodeGuid;
                FString NodeTitle;
                FString NodeType;
                if (!Row.GetColumnValueByIndex(0, MemberName)
                    || !Row.GetColumnValueByIndex(1, GraphName)
                    || !Row.GetColumnValueByIndex(2, NodeGuid)
                    || !Row.GetColumnValueByIndex(3, NodeTitle)
                    || !Row.GetColumnValueByIndex(4, NodeType))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }
                if (ComponentNames.Contains(MemberName))
                {
                    TSharedRef<FJsonObject> Usage = MakeShared<FJsonObject>();
                    Usage->SetStringField(TEXT("graphName"), GraphName);
                    Usage->SetStringField(TEXT("nodeGuid"), NodeGuid);
                    Usage->SetStringField(TEXT("nodeTitle"), NodeTitle);
                    Usage->SetStringField(TEXT("nodeType"), NodeType);
                    UsageByComponent.FindOrAdd(MemberName).Add(MakeShared<FJsonValueObject>(Usage));
                }
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });
            if (UsageResult == INDEX_NONE)
            {
                OutExecError = Database.GetLastError().IsEmpty() ? TEXT("Component usage query failed.") : Database.GetLastError();
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
    TArray<TSharedPtr<FJsonValue>> Components;
    int32 ReferencedComponentCount = 0;
    for (const FIndexedComponent& Component : IndexedComponents)
    {
        const TArray<TSharedPtr<FJsonValue>>& Usage = UsageByComponent.FindOrAdd(Component.VariableName);
        ReferencedComponentCount += Usage.IsEmpty() ? 0 : 1;

        TSharedRef<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
        ComponentObject->SetStringField(TEXT("variableName"), Component.VariableName);
        ComponentObject->SetStringField(TEXT("componentClassPath"), Component.ComponentClassPath);
        ComponentObject->SetStringField(TEXT("componentBlueprintPath"), Component.ComponentBlueprintPath);
        ComponentObject->SetStringField(TEXT("templateName"), Component.TemplateName);
        ComponentObject->SetStringField(TEXT("templatePath"), Component.TemplatePath);
        ComponentObject->SetStringField(TEXT("parentVariableName"), Component.ParentVariableName);
        ComponentObject->SetStringField(TEXT("attachSocketName"), Component.AttachSocketName);
        ComponentObject->SetStringField(TEXT("creationSource"), Component.CreationSource);
        ComponentObject->SetBoolField(TEXT("isSceneComponent"), Component.bIsSceneComponent);
        ComponentObject->SetBoolField(TEXT("isDefaultSceneRoot"), Component.bIsDefaultSceneRoot);
        ComponentObject->SetNumberField(TEXT("childCount"), Component.ChildCount);
        ComponentObject->SetStringField(TEXT("relativeLocation"), Component.RelativeLocation);
        ComponentObject->SetStringField(TEXT("relativeRotation"), Component.RelativeRotation);
        ComponentObject->SetStringField(TEXT("relativeScale"), Component.RelativeScale);
        ComponentObject->SetStringField(TEXT("mobility"), Component.Mobility);
        ComponentObject->SetNumberField(TEXT("usageCount"), Usage.Num());
        ComponentObject->SetArrayField(TEXT("graphUsages"), Usage);

        TSharedRef<FJsonObject> TraceParams = MakeShared<FJsonObject>();
        TraceParams->SetStringField(TEXT("objectPath"), ObjectPath);
        TraceParams->SetStringField(TEXT("startNodeQuery"), Component.VariableName);
        TraceParams->SetStringField(TEXT("direction"), TEXT("both"));
        TraceParams->SetStringField(TEXT("outputMode"), TEXT("summary"));
        ComponentObject->SetObjectField(TEXT("suggestedTraceParams"), TraceParams);
        Components.Add(MakeShared<FJsonValueObject>(ComponentObject));
    }

    Result->SetNumberField(TEXT("count"), IndexedComponents.Num());
    Result->SetNumberField(TEXT("referencedComponentCount"), ReferencedComponentCount);
    Result->SetArrayField(TEXT("components"), Components);
    Result->SetStringField(TEXT("nextStepHint"), TEXT("Use a component's suggestedTraceParams to follow its get/call flow. If componentBlueprintPath is present, inspect that Actor Component Blueprint as a separate execution host."));
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
