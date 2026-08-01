#include "Tools/ExplainBlueprintRoleTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Array.h"
#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    constexpr int32 PreviewLimit = 8;

    void AddUniqueRoleHint(TArray<FString>& RoleHints, const FString& Hint)
    {
        if (!Hint.IsEmpty())
        {
            RoleHints.AddUnique(Hint);
        }
    }

    void AddRoleHintsFromClassPath(const FString& ClassPath, TArray<FString>& RoleHints)
    {
        const FString LoweredClassPath = ClassPath.ToLower();
        if (LoweredClassPath.Contains(TEXT("character")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("character-like"));
        }

        if (LoweredClassPath.Contains(TEXT("pawn")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("pawn-like"));
        }

        if (LoweredClassPath.Contains(TEXT("controller")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("controller-like"));
        }

        if (LoweredClassPath.Contains(TEXT("gamemode")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("game-rule-owner"));
        }

        if (LoweredClassPath.Contains(TEXT("gamestate")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("shared-game-state"));
        }

        if (LoweredClassPath.Contains(TEXT("playerstate")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("player-state"));
        }

        if (LoweredClassPath.Contains(TEXT("actorcomponent")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("reusable-component"));
        }
    }

    void AddRoleHintsFromComponentClass(const FString& ComponentClassPath, TArray<FString>& RoleHints)
    {
        const FString LoweredComponentClassPath = ComponentClassPath.ToLower();
        if (LoweredComponentClassPath.Contains(TEXT("camera")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("camera-related"));
        }

        if (LoweredComponentClassPath.Contains(TEXT("springarm")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("camera-rig"));
        }

        if (LoweredComponentClassPath.Contains(TEXT("widget")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("ui-related"));
        }

        if (LoweredComponentClassPath.Contains(TEXT("audio")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("audio-related"));
        }

        if (LoweredComponentClassPath.Contains(TEXT("skeletalmesh")) || LoweredComponentClassPath.Contains(TEXT("staticmesh")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("visual-mesh-owner"));
        }

        if (LoweredComponentClassPath.Contains(TEXT("boxcomponent"))
            || LoweredComponentClassPath.Contains(TEXT("spherecomponent"))
            || LoweredComponentClassPath.Contains(TEXT("capsulecomponent")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("collision-related"));
        }

        if (LoweredComponentClassPath.Contains(TEXT("projectilemovement")))
        {
            AddUniqueRoleHint(RoleHints, TEXT("movement-or-projectile"));
        }
    }

    TSharedRef<FJsonObject> MakeLinkedAssetObject(
        const FString& PackageName,
        const FString& ObjectPath,
        const FString& AssetName,
        const FString& ClassPath,
        const FString& ContentScope,
        const bool bIsBlueprint)
    {
        TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
        AssetObject->SetStringField(TEXT("packageName"), PackageName);
        AssetObject->SetStringField(TEXT("objectPath"), ObjectPath);
        AssetObject->SetStringField(TEXT("assetName"), AssetName);
        AssetObject->SetStringField(TEXT("classPath"), ClassPath);
        AssetObject->SetStringField(TEXT("contentScope"), ContentScope.IsEmpty() ? UnrealMCP::IndexedQueryToolUtils::GetContentScopeFromPackageName(PackageName) : ContentScope);
        AssetObject->SetBoolField(TEXT("isBlueprint"), bIsBlueprint);
        return AssetObject;
    }
}

FExplainBlueprintRoleTool::FExplainBlueprintRoleTool()
    : FMCPToolBase(TEXT("ExplainBlueprintRole"), TEXT("Returns a structured indexed summary that helps explain a Blueprint's likely role, neighbors, and composition inside the project."))
{
}

UnrealMCP::FMCPResponse FExplainBlueprintRoleTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    if (!Request.Params.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ExplainBlueprintRole requires params.objectPath."));
    }

    FString ObjectPath;
    if (!Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ExplainBlueprintRole requires a non-empty params.objectPath."));
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    const FAssetData Asset = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
    if (!Asset.IsValid())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ExplainBlueprintRole could not resolve the asset from params.objectPath."));
    }

    TSharedPtr<FJsonObject> BlueprintAsset;
    TSharedPtr<FJsonObject> BlueprintProfile;
    TArray<TSharedPtr<FJsonValue>> ComponentPreview;
    TArray<TSharedPtr<FJsonValue>> VariablePreview;
    TArray<TSharedPtr<FJsonValue>> FunctionPreview;
    TArray<TSharedPtr<FJsonValue>> DependencyPreview;
    TArray<TSharedPtr<FJsonValue>> ReferencerPreview;
    TArray<FString> RoleHints;
    int32 DependencyCount = 0;
    int32 ReferencerCount = 0;
    FString Error;

    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            if (!UnrealMCP::IndexedQueryToolUtils::QueryAssetByObjectPath(Database, ObjectPath, BlueprintAsset, OutExecError) || !BlueprintAsset.IsValid())
            {
                if (OutExecError.IsEmpty())
                {
                    OutExecError = TEXT("The Blueprint asset is not present in the current project index. Rebuild the index and try again.");
                }
                return false;
            }

            {
                FSQLitePreparedStatement Statement(
                    Database,
                    TEXT("SELECT generated_class_path, parent_class_path, native_parent_class_path, blueprint_type, is_data_only, blueprint_status, variable_count, function_count, component_count, interface_count "
                         "FROM assets WHERE object_path = ?1;"),
                    ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ObjectPath))
                {
                    OutExecError = TEXT("ExplainBlueprintRole could not prepare the Blueprint profile query.");
                    return false;
                }

                const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
                {
                    FString GeneratedClassPath;
                    FString ParentClassPath;
                    FString NativeParentClassPath;
                    FString BlueprintType;
                    int32 bIsDataOnly = 0;
                    FString BlueprintStatus;
                    int32 VariableCount = 0;
                    int32 FunctionCount = 0;
                    int32 ComponentCount = 0;
                    int32 InterfaceCount = 0;

                    if (!Row.GetColumnValueByIndex(0, GeneratedClassPath)
                        || !Row.GetColumnValueByIndex(1, ParentClassPath)
                        || !Row.GetColumnValueByIndex(2, NativeParentClassPath)
                        || !Row.GetColumnValueByIndex(3, BlueprintType)
                        || !Row.GetColumnValueByIndex(4, bIsDataOnly)
                        || !Row.GetColumnValueByIndex(5, BlueprintStatus)
                        || !Row.GetColumnValueByIndex(6, VariableCount)
                        || !Row.GetColumnValueByIndex(7, FunctionCount)
                        || !Row.GetColumnValueByIndex(8, ComponentCount)
                        || !Row.GetColumnValueByIndex(9, InterfaceCount))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }

                    TSharedRef<FJsonObject> ProfileObject = MakeShared<FJsonObject>();
                    ProfileObject->SetStringField(TEXT("generatedClassPath"), GeneratedClassPath);
                    ProfileObject->SetStringField(TEXT("parentClassPath"), ParentClassPath);
                    ProfileObject->SetStringField(TEXT("nativeParentClassPath"), NativeParentClassPath);
                    ProfileObject->SetStringField(TEXT("blueprintType"), BlueprintType);
                    ProfileObject->SetBoolField(TEXT("isDataOnly"), bIsDataOnly != 0);
                    ProfileObject->SetStringField(TEXT("blueprintStatus"), BlueprintStatus);
                    ProfileObject->SetNumberField(TEXT("variableCount"), VariableCount);
                    ProfileObject->SetNumberField(TEXT("functionCount"), FunctionCount);
                    ProfileObject->SetNumberField(TEXT("componentCount"), ComponentCount);
                    ProfileObject->SetNumberField(TEXT("interfaceCount"), InterfaceCount);
                    BlueprintProfile = ProfileObject;
                    AddRoleHintsFromClassPath(ParentClassPath, RoleHints);
                    return ESQLitePreparedStatementExecuteRowResult::Stop;
                });

                if (QueryResult == INDEX_NONE)
                {
                    OutExecError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
                    return false;
                }
            }

            auto QueryLinkedAssets =
                [&](const TCHAR* Sql, const FString& PackageName, TArray<TSharedPtr<FJsonValue>>& OutRows, int32& OutCount) -> bool
            {
                FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, PackageName))
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
                    if (OutRows.Num() < PreviewLimit)
                    {
                        OutRows.Add(MakeShared<FJsonValueObject>(
                            MakeLinkedAssetObject(
                                RelatedPackageName,
                                RelatedObjectPath,
                                RelatedAssetName,
                                RelatedClassPath,
                                RelatedContentScope,
                                bRelatedIsBlueprint != 0)));
                    }

                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                }) != INDEX_NONE;
            };

            const FString PackageName = BlueprintAsset->GetStringField(TEXT("packageName"));
            if (!QueryLinkedAssets(
                    TEXT("SELECT d.target_package_name, COALESCE(a.object_path, ''), COALESCE(a.asset_name, ''), COALESCE(a.class_path, ''), COALESCE(a.content_scope, ''), COALESCE(a.is_blueprint, 0) "
                         "FROM asset_dependencies d "
                         "LEFT JOIN assets a ON a.package_name = d.target_package_name "
                         "WHERE d.source_package_name = ?1 "
                         "ORDER BY d.target_package_name ASC;"),
                    PackageName,
                    DependencyPreview,
                    DependencyCount)
                || !QueryLinkedAssets(
                    TEXT("SELECT d.source_package_name, COALESCE(a.object_path, ''), COALESCE(a.asset_name, ''), COALESCE(a.class_path, ''), COALESCE(a.content_scope, ''), COALESCE(a.is_blueprint, 0) "
                         "FROM asset_dependencies d "
                         "LEFT JOIN assets a ON a.package_name = d.source_package_name "
                         "WHERE d.target_package_name = ?1 "
                         "ORDER BY d.source_package_name ASC;"),
                    PackageName,
                    ReferencerPreview,
                    ReferencerCount))
            {
                OutExecError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
                return false;
            }

            {
                FSQLitePreparedStatement Statement(
                    Database,
                    TEXT("SELECT variable_name, component_class_path, parent_variable_name, is_default_scene_root, child_count "
                         "FROM blueprint_components WHERE blueprint_object_path = ?1 ORDER BY variable_name ASC;"),
                    ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ObjectPath))
                {
                    OutExecError = TEXT("ExplainBlueprintRole could not prepare the component query.");
                    return false;
                }

                if (Statement.Execute([&](const FSQLitePreparedStatement& Row)
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

                        AddRoleHintsFromComponentClass(ComponentClassPath, RoleHints);

                        if (ComponentPreview.Num() < PreviewLimit)
                        {
                            TSharedRef<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
                            ComponentObject->SetStringField(TEXT("variableName"), VariableName);
                            ComponentObject->SetStringField(TEXT("componentClassPath"), ComponentClassPath);
                            ComponentObject->SetStringField(TEXT("parentVariableName"), ParentVariableName);
                            ComponentObject->SetBoolField(TEXT("isDefaultSceneRoot"), bIsDefaultSceneRoot != 0);
                            ComponentObject->SetNumberField(TEXT("childCount"), ChildCount);
                            ComponentPreview.Add(MakeShared<FJsonValueObject>(ComponentObject));
                        }

                        return ESQLitePreparedStatementExecuteRowResult::Continue;
                    }) == INDEX_NONE)
                {
                    OutExecError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
                    return false;
                }
            }

            {
                FSQLitePreparedStatement Statement(
                    Database,
                    TEXT("SELECT name, type_category, type_subcategory, type_object_path, container_type, is_reference, is_const "
                         "FROM blueprint_variables WHERE blueprint_object_path = ?1 ORDER BY name ASC;"),
                    ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ObjectPath))
                {
                    OutExecError = TEXT("ExplainBlueprintRole could not prepare the variable query.");
                    return false;
                }

                if (Statement.Execute([&](const FSQLitePreparedStatement& Row)
                    {
                        if (VariablePreview.Num() >= PreviewLimit)
                        {
                            return ESQLitePreparedStatementExecuteRowResult::Continue;
                        }

                        FString Name;
                        FString TypeCategory;
                        FString TypeSubcategory;
                        FString TypeObjectPath;
                        FString ContainerType;
                        int32 bIsReference = 0;
                        int32 bIsConst = 0;

                        if (!Row.GetColumnValueByIndex(0, Name)
                            || !Row.GetColumnValueByIndex(1, TypeCategory)
                            || !Row.GetColumnValueByIndex(2, TypeSubcategory)
                            || !Row.GetColumnValueByIndex(3, TypeObjectPath)
                            || !Row.GetColumnValueByIndex(4, ContainerType)
                            || !Row.GetColumnValueByIndex(5, bIsReference)
                            || !Row.GetColumnValueByIndex(6, bIsConst))
                        {
                            return ESQLitePreparedStatementExecuteRowResult::Error;
                        }

                        TSharedRef<FJsonObject> VariableObject = MakeShared<FJsonObject>();
                        VariableObject->SetStringField(TEXT("name"), Name);
                        VariableObject->SetStringField(TEXT("typeCategory"), TypeCategory);
                        VariableObject->SetStringField(TEXT("typeSubcategory"), TypeSubcategory);
                        VariableObject->SetStringField(TEXT("typeObjectPath"), TypeObjectPath);
                        VariableObject->SetStringField(TEXT("containerType"), ContainerType);
                        VariableObject->SetBoolField(TEXT("isReference"), bIsReference != 0);
                        VariableObject->SetBoolField(TEXT("isConst"), bIsConst != 0);
                        VariablePreview.Add(MakeShared<FJsonValueObject>(VariableObject));
                        return ESQLitePreparedStatementExecuteRowResult::Continue;
                    }) == INDEX_NONE)
                {
                    OutExecError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
                    return false;
                }
            }

            {
                FSQLitePreparedStatement Statement(
                    Database,
                    TEXT("SELECT name, source, interface_path FROM blueprint_functions WHERE blueprint_object_path = ?1 ORDER BY name ASC;"),
                    ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ObjectPath))
                {
                    OutExecError = TEXT("ExplainBlueprintRole could not prepare the function query.");
                    return false;
                }

                if (Statement.Execute([&](const FSQLitePreparedStatement& Row)
                    {
                        if (FunctionPreview.Num() >= PreviewLimit)
                        {
                            return ESQLitePreparedStatementExecuteRowResult::Continue;
                        }

                        FString Name;
                        FString Source;
                        FString InterfacePath;
                        if (!Row.GetColumnValueByIndex(0, Name)
                            || !Row.GetColumnValueByIndex(1, Source)
                            || !Row.GetColumnValueByIndex(2, InterfacePath))
                        {
                            return ESQLitePreparedStatementExecuteRowResult::Error;
                        }

                        TSharedRef<FJsonObject> FunctionObject = MakeShared<FJsonObject>();
                        FunctionObject->SetStringField(TEXT("name"), Name);
                        FunctionObject->SetStringField(TEXT("source"), Source);
                        FunctionObject->SetStringField(TEXT("interfacePath"), InterfacePath);
                        FunctionPreview.Add(MakeShared<FJsonValueObject>(FunctionObject));
                        return ESQLitePreparedStatementExecuteRowResult::Continue;
                    }) == INDEX_NONE)
                {
                    OutExecError = Database.GetLastError().IsEmpty() ? TEXT("SQLite query execution failed.") : Database.GetLastError();
                    return false;
                }
            }

            if (ReferencerCount == 0)
            {
                AddUniqueRoleHint(RoleHints, TEXT("leaf-or-manually-placed"));
            }
            else
            {
                AddUniqueRoleHint(RoleHints, TEXT("used-by-other-assets"));
            }

            if (DependencyCount > 0)
            {
                AddUniqueRoleHint(RoleHints, TEXT("depends-on-other-assets"));
            }

            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("ExplainBlueprintRole failed to query the project index: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
    Result->SetObjectField(TEXT("asset"), BlueprintAsset.ToSharedRef());
    if (BlueprintProfile.IsValid())
    {
        Result->SetObjectField(TEXT("blueprintProfile"), BlueprintProfile.ToSharedRef());
    }

    TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetStringField(TEXT("folderPath"), Asset.PackagePath.ToString());
    Summary->SetNumberField(TEXT("dependencyCount"), DependencyCount);
    Summary->SetNumberField(TEXT("referencerCount"), ReferencerCount);
    Summary->SetNumberField(TEXT("previewLimit"), PreviewLimit);
    Result->SetObjectField(TEXT("summary"), Summary);

    TArray<TSharedPtr<FJsonValue>> RoleHintValues;
    for (const FString& RoleHint : RoleHints)
    {
        RoleHintValues.Add(MakeShared<FJsonValueString>(RoleHint));
    }
    Result->SetArrayField(TEXT("roleHints"), RoleHintValues);
    Result->SetArrayField(TEXT("componentPreview"), ComponentPreview);
    Result->SetArrayField(TEXT("variablePreview"), VariablePreview);
    Result->SetArrayField(TEXT("functionPreview"), FunctionPreview);
    Result->SetArrayField(TEXT("dependencyPreview"), DependencyPreview);
    Result->SetArrayField(TEXT("referencerPreview"), ReferencerPreview);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FExplainBlueprintRoleTool::BuildInputSchema() const
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
