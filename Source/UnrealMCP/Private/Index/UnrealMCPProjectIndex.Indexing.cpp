#include "Index/UnrealMCPProjectIndex.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "Blueprint/BlueprintSupport.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Components/SceneComponent.h"
#include "Index/UnrealMCPProjectIndexInternal.h"
#include "Tools/BlueprintToolUtils.h"

bool FUnrealMCPProjectIndex::UpsertAsset(const FAssetData& AssetData, FString* OutError)
{
    const FString ObjectPath = AssetData.GetObjectPathString();
    const FString PackageName = AssetData.PackageName.ToString();

    if (!RemoveAsset(ObjectPath, PackageName, OutError))
    {
        return false;
    }

    const bool bIsBlueprint = IsBlueprintAsset(AssetData);
    const FString ContentScope = GetContentScope(AssetData);
    FString GeneratedClassPath = AssetData.GetTagValueRef<FString>(FBlueprintTags::GeneratedClassPath);
    FString ParentClassPath = AssetData.GetTagValueRef<FString>(FBlueprintTags::ParentClassPath);
    const FString NativeParentClassPath = AssetData.GetTagValueRef<FString>(FBlueprintTags::NativeParentClassPath);
    const FString BlueprintType = AssetData.GetTagValueRef<FString>(FBlueprintTags::BlueprintType);
    const FString IsDataOnly = AssetData.GetTagValueRef<FString>(FBlueprintTags::IsDataOnly);
    FString BlueprintStatus;
    int32 VariableCount = 0;
    int32 FunctionCount = 0;
    int32 ComponentCount = 0;
    int32 InterfaceCount = 0;

    TArray<FBPVariableDescription> BlueprintVariables;
    TArray<TTuple<FString, FString, FString>> BlueprintFunctions;
    TArray<FIndexedBlueprintComponentRow> BlueprintComponents;
    TArray<FIndexedBlueprintGraphRow> BlueprintGraphs;
    TArray<FIndexedBlueprintNodeRow> BlueprintNodes;
    TArray<FIndexedBlueprintPinRow> BlueprintPins;
    TArray<FIndexedBlueprintEdgeRow> BlueprintEdges;

    if (bIsBlueprint)
    {
        if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath))
        {
            BlueprintStatus = UnrealMCP::BlueprintToolUtils::GetBlueprintStatusString(Blueprint->Status);
            VariableCount = Blueprint->NewVariables.Num();
            InterfaceCount = Blueprint->ImplementedInterfaces.Num();
            GeneratedClassPath = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : GeneratedClassPath;
            ParentClassPath = Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : ParentClassPath;

            BlueprintVariables = Blueprint->NewVariables;

            TArray<UEdGraph*> FunctionGraphs = Blueprint->FunctionGraphs;
            FunctionGraphs.Sort([](const UEdGraph& Left, const UEdGraph& Right)
            {
                return Left.GetName() < Right.GetName();
            });

            for (const UEdGraph* Graph : FunctionGraphs)
            {
                BlueprintFunctions.Emplace(Graph ? Graph->GetName() : FString(), TEXT("functionGraph"), FString());
            }

            for (const FBPInterfaceDescription& InterfaceDescription : Blueprint->ImplementedInterfaces)
            {
                for (const UEdGraph* Graph : InterfaceDescription.Graphs)
                {
                    BlueprintFunctions.Emplace(
                        Graph ? Graph->GetName() : FString(),
                        TEXT("interfaceGraph"),
                        InterfaceDescription.Interface ? InterfaceDescription.Interface->GetPathName() : FString());
                }

                if (InterfaceDescription.Interface != nullptr)
                {
                    for (TFieldIterator<UFunction> FunctionIt(InterfaceDescription.Interface, EFieldIteratorFlags::ExcludeSuper); FunctionIt; ++FunctionIt)
                    {
                        BlueprintFunctions.Emplace(
                            FunctionIt->GetName(),
                            TEXT("interfaceDeclaration"),
                            InterfaceDescription.Interface->GetPathName());
                    }
                }
            }

            FunctionCount = BlueprintFunctions.Num();

            if (Blueprint->SimpleConstructionScript)
            {
                TArray<const USCS_Node*> Nodes;
                for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
                {
                    Nodes.Add(Node);
                }

                Nodes.Sort([](const USCS_Node& Left, const USCS_Node& Right)
                {
                    return Left.GetVariableName().LexicalLess(Right.GetVariableName());
                });

                for (const USCS_Node* Node : Nodes)
                {
                    if (Node == nullptr)
                    {
                        continue;
                    }

                    const USCS_Node* ParentNode = Blueprint->SimpleConstructionScript->FindParentNode(const_cast<USCS_Node*>(Node));
                    FIndexedBlueprintComponentRow ComponentRow;
                    ComponentRow.VariableName = Node->GetVariableName().ToString();
                    ComponentRow.ComponentClassPath = Node->ComponentClass ? Node->ComponentClass->GetPathName() : FString();
                    ComponentRow.TemplateName = Node->ComponentTemplate ? Node->ComponentTemplate->GetName() : FString();
                    ComponentRow.TemplatePath = Node->ComponentTemplate ? Node->ComponentTemplate->GetPathName() : FString();
                    ComponentRow.ParentVariableName = ParentNode
                        ? ParentNode->GetVariableName().ToString()
                        : Node->ParentComponentOrVariableName.ToString();
                    ComponentRow.AttachSocketName = Node->AttachToName.ToString();
                    ComponentRow.bIsDefaultSceneRoot = Blueprint->SimpleConstructionScript->GetDefaultSceneRootNode() == Node;
                    ComponentRow.ChildCount = Node->GetChildNodes().Num();

                    if (Node->ComponentClass != nullptr)
                    {
                        if (const UBlueprint* ComponentBlueprint = Cast<UBlueprint>(Node->ComponentClass->ClassGeneratedBy))
                        {
                            ComponentRow.ComponentBlueprintPath = ComponentBlueprint->GetPathName();
                        }
                    }

                    if (const USceneComponent* SceneTemplate = Cast<USceneComponent>(Node->ComponentTemplate))
                    {
                        ComponentRow.bIsSceneComponent = true;
                        ComponentRow.RelativeLocation = SceneTemplate->GetRelativeLocation().ToCompactString();
                        ComponentRow.RelativeRotation = SceneTemplate->GetRelativeRotation().ToCompactString();
                        ComponentRow.RelativeScale = SceneTemplate->GetRelativeScale3D().ToCompactString();
                        ComponentRow.Mobility = StaticEnum<EComponentMobility::Type>()->GetNameStringByValue(
                            static_cast<int64>(SceneTemplate->Mobility));
                    }

                    BlueprintComponents.Add(MoveTemp(ComponentRow));
                }
            }

            ComponentCount = BlueprintComponents.Num();
            ExtractBlueprintGraphRows(Blueprint, BlueprintGraphs, BlueprintNodes, BlueprintPins, BlueprintEdges);
        }
        else
        {
            BlueprintStatus = TEXT("load_failed");
        }
    }

    const FString IndexedAtUtc = ToUtcString(FDateTime::UtcNow());
    if (!ExecuteBoundStatement(Database,
        TEXT("INSERT INTO assets("
            "object_path, asset_name, class_path, package_name, package_path, content_scope, is_blueprint, generated_class_path, parent_class_path, native_parent_class_path, blueprint_type, is_data_only, blueprint_status, variable_count, function_count, component_count, interface_count, indexed_at_utc)"
            " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18);"),
        [&AssetData, &ObjectPath, &PackageName, &ContentScope, &GeneratedClassPath, &ParentClassPath, &NativeParentClassPath, &BlueprintType, &IsDataOnly, &BlueprintStatus, VariableCount, FunctionCount, ComponentCount, InterfaceCount, &IndexedAtUtc, bIsBlueprint](FSQLitePreparedStatement& Statement)
        {
            return Statement.SetBindingValueByIndex(1, ObjectPath)
                && Statement.SetBindingValueByIndex(2, AssetData.AssetName.ToString())
                && Statement.SetBindingValueByIndex(3, AssetData.AssetClassPath.ToString())
                && Statement.SetBindingValueByIndex(4, PackageName)
                && Statement.SetBindingValueByIndex(5, AssetData.PackagePath.ToString())
                && Statement.SetBindingValueByIndex(6, ContentScope)
                && Statement.SetBindingValueByIndex(7, bIsBlueprint ? 1 : 0)
                && Statement.SetBindingValueByIndex(8, GeneratedClassPath)
                && Statement.SetBindingValueByIndex(9, ParentClassPath)
                && Statement.SetBindingValueByIndex(10, NativeParentClassPath)
                && Statement.SetBindingValueByIndex(11, BlueprintType)
                && Statement.SetBindingValueByIndex(12, IsDataOnly)
                && Statement.SetBindingValueByIndex(13, BlueprintStatus)
                && Statement.SetBindingValueByIndex(14, VariableCount)
                && Statement.SetBindingValueByIndex(15, FunctionCount)
                && Statement.SetBindingValueByIndex(16, ComponentCount)
                && Statement.SetBindingValueByIndex(17, InterfaceCount)
                && Statement.SetBindingValueByIndex(18, IndexedAtUtc);
        },
        OutError))
    {
        return false;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FName> Dependencies;
    AssetRegistryModule.Get().GetDependencies(AssetData.PackageName, Dependencies);
    Dependencies.Sort(FNameLexicalLess());

    for (const FName& DependencyPackage : Dependencies)
    {
        if (!ExecuteBoundStatement(Database,
            TEXT("INSERT INTO asset_dependencies(source_package_name, target_package_name) VALUES(?1, ?2);"),
            [&PackageName, &DependencyPackage](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, PackageName)
                    && Statement.SetBindingValueByIndex(2, DependencyPackage.ToString());
            },
            OutError))
        {
            return false;
        }
    }

    for (const FBPVariableDescription& Variable : BlueprintVariables)
    {
        if (!ExecuteBoundStatement(Database,
            TEXT("INSERT INTO blueprint_variables(blueprint_object_path, name, type_category, type_subcategory, type_object_path, container_type, is_reference, is_const)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);"),
            [&ObjectPath, &Variable](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath)
                    && Statement.SetBindingValueByIndex(2, Variable.VarName.ToString())
                    && Statement.SetBindingValueByIndex(3, Variable.VarType.PinCategory.ToString())
                    && Statement.SetBindingValueByIndex(4, Variable.VarType.PinSubCategory.ToString())
                    && Statement.SetBindingValueByIndex(5, Variable.VarType.PinSubCategoryObject.IsValid() ? Variable.VarType.PinSubCategoryObject->GetPathName() : FString())
                    && Statement.SetBindingValueByIndex(6, StaticEnum<EPinContainerType>()->GetNameStringByValue(static_cast<int64>(Variable.VarType.ContainerType)))
                    && Statement.SetBindingValueByIndex(7, Variable.VarType.bIsReference ? 1 : 0)
                    && Statement.SetBindingValueByIndex(8, Variable.VarType.bIsConst ? 1 : 0);
            },
            OutError))
        {
            return false;
        }
    }

    for (const TTuple<FString, FString, FString>& FunctionRow : BlueprintFunctions)
    {
        if (!ExecuteBoundStatement(Database,
            TEXT("INSERT INTO blueprint_functions(blueprint_object_path, name, source, interface_path) VALUES(?1, ?2, ?3, ?4);"),
            [&ObjectPath, &FunctionRow](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath)
                    && Statement.SetBindingValueByIndex(2, FunctionRow.Get<0>())
                    && Statement.SetBindingValueByIndex(3, FunctionRow.Get<1>())
                    && Statement.SetBindingValueByIndex(4, FunctionRow.Get<2>());
            },
            OutError))
        {
            return false;
        }
    }

    for (const FIndexedBlueprintComponentRow& ComponentRow : BlueprintComponents)
    {
        if (!ExecuteBoundStatement(Database,
            TEXT("INSERT INTO blueprint_components(blueprint_object_path, variable_name, component_class_path, component_blueprint_path, template_name, template_path, parent_variable_name, attach_socket_name, creation_source, is_scene_component, is_default_scene_root, child_count, relative_location, relative_rotation, relative_scale, mobility)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16);"),
            [&ObjectPath, &ComponentRow](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath)
                    && Statement.SetBindingValueByIndex(2, ComponentRow.VariableName)
                    && Statement.SetBindingValueByIndex(3, ComponentRow.ComponentClassPath)
                    && Statement.SetBindingValueByIndex(4, ComponentRow.ComponentBlueprintPath)
                    && Statement.SetBindingValueByIndex(5, ComponentRow.TemplateName)
                    && Statement.SetBindingValueByIndex(6, ComponentRow.TemplatePath)
                    && Statement.SetBindingValueByIndex(7, ComponentRow.ParentVariableName)
                    && Statement.SetBindingValueByIndex(8, ComponentRow.AttachSocketName)
                    && Statement.SetBindingValueByIndex(9, ComponentRow.CreationSource)
                    && Statement.SetBindingValueByIndex(10, ComponentRow.bIsSceneComponent ? 1 : 0)
                    && Statement.SetBindingValueByIndex(11, ComponentRow.bIsDefaultSceneRoot ? 1 : 0)
                    && Statement.SetBindingValueByIndex(12, ComponentRow.ChildCount)
                    && Statement.SetBindingValueByIndex(13, ComponentRow.RelativeLocation)
                    && Statement.SetBindingValueByIndex(14, ComponentRow.RelativeRotation)
                    && Statement.SetBindingValueByIndex(15, ComponentRow.RelativeScale)
                    && Statement.SetBindingValueByIndex(16, ComponentRow.Mobility);
            },
            OutError))
        {
            return false;
        }
    }

    for (const FIndexedBlueprintGraphRow& GraphRow : BlueprintGraphs)
    {
        if (!ExecuteBoundStatement(Database,
            TEXT("INSERT INTO blueprint_graphs(blueprint_object_path, graph_name, graph_guid, graph_type, entry_node_guid, node_count)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6);"),
            [&ObjectPath, &GraphRow](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath)
                    && Statement.SetBindingValueByIndex(2, GraphRow.GraphName)
                    && Statement.SetBindingValueByIndex(3, GraphRow.GraphGuid)
                    && Statement.SetBindingValueByIndex(4, GraphRow.GraphType)
                    && Statement.SetBindingValueByIndex(5, GraphRow.EntryNodeGuid)
                    && Statement.SetBindingValueByIndex(6, GraphRow.NodeCount);
            },
            OutError))
        {
            return false;
        }
    }

    for (const FIndexedBlueprintNodeRow& NodeRow : BlueprintNodes)
    {
        if (!ExecuteBoundStatement(Database,
            TEXT("INSERT INTO blueprint_nodes(blueprint_object_path, graph_name, node_guid, node_name, node_class_path, node_title, node_type, member_name, member_parent_path, pos_x, pos_y, is_pure, input_pin_count, output_pin_count)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14);"),
            [&ObjectPath, &NodeRow](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath)
                    && Statement.SetBindingValueByIndex(2, NodeRow.GraphName)
                    && Statement.SetBindingValueByIndex(3, NodeRow.NodeGuid)
                    && Statement.SetBindingValueByIndex(4, NodeRow.NodeName)
                    && Statement.SetBindingValueByIndex(5, NodeRow.NodeClassPath)
                    && Statement.SetBindingValueByIndex(6, NodeRow.NodeTitle)
                    && Statement.SetBindingValueByIndex(7, NodeRow.NodeType)
                    && Statement.SetBindingValueByIndex(8, NodeRow.MemberName)
                    && Statement.SetBindingValueByIndex(9, NodeRow.MemberParentPath)
                    && Statement.SetBindingValueByIndex(10, NodeRow.NodePosX)
                    && Statement.SetBindingValueByIndex(11, NodeRow.NodePosY)
                    && Statement.SetBindingValueByIndex(12, NodeRow.bIsPure ? 1 : 0)
                    && Statement.SetBindingValueByIndex(13, NodeRow.InputPinCount)
                    && Statement.SetBindingValueByIndex(14, NodeRow.OutputPinCount);
            },
            OutError))
        {
            return false;
        }
    }

    for (const FIndexedBlueprintPinRow& PinRow : BlueprintPins)
    {
        if (!ExecuteBoundStatement(Database,
            TEXT("INSERT INTO blueprint_pins(blueprint_object_path, graph_name, node_guid, pin_id, pin_name, direction, category, subcategory, subcategory_object_path, container_type, is_reference, is_const, linked_pin_count, default_value)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14);"),
            [&ObjectPath, &PinRow](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath)
                    && Statement.SetBindingValueByIndex(2, PinRow.GraphName)
                    && Statement.SetBindingValueByIndex(3, PinRow.NodeGuid)
                    && Statement.SetBindingValueByIndex(4, PinRow.PinId)
                    && Statement.SetBindingValueByIndex(5, PinRow.PinName)
                    && Statement.SetBindingValueByIndex(6, PinRow.Direction)
                    && Statement.SetBindingValueByIndex(7, PinRow.Category)
                    && Statement.SetBindingValueByIndex(8, PinRow.Subcategory)
                    && Statement.SetBindingValueByIndex(9, PinRow.SubcategoryObjectPath)
                    && Statement.SetBindingValueByIndex(10, PinRow.ContainerType)
                    && Statement.SetBindingValueByIndex(11, PinRow.bIsReference ? 1 : 0)
                    && Statement.SetBindingValueByIndex(12, PinRow.bIsConst ? 1 : 0)
                    && Statement.SetBindingValueByIndex(13, PinRow.LinkedPinCount)
                    && Statement.SetBindingValueByIndex(14, PinRow.DefaultValue);
            },
            OutError))
        {
            return false;
        }
    }

    for (const FIndexedBlueprintEdgeRow& EdgeRow : BlueprintEdges)
    {
        if (!ExecuteBoundStatement(Database,
            TEXT("INSERT INTO blueprint_edges(blueprint_object_path, source_graph_name, source_node_guid, source_pin_id, target_graph_name, target_node_guid, target_pin_id, edge_kind)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);"),
            [&ObjectPath, &EdgeRow](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath)
                    && Statement.SetBindingValueByIndex(2, EdgeRow.SourceGraphName)
                    && Statement.SetBindingValueByIndex(3, EdgeRow.SourceNodeGuid)
                    && Statement.SetBindingValueByIndex(4, EdgeRow.SourcePinId)
                    && Statement.SetBindingValueByIndex(5, EdgeRow.TargetGraphName)
                    && Statement.SetBindingValueByIndex(6, EdgeRow.TargetNodeGuid)
                    && Statement.SetBindingValueByIndex(7, EdgeRow.TargetPinId)
                    && Statement.SetBindingValueByIndex(8, EdgeRow.EdgeKind);
            },
            OutError))
        {
            return false;
        }
    }

    LastUpdateUtc = IndexedAtUtc;
    SetMetadataValue(TEXT("last_update_utc"), LastUpdateUtc, nullptr);
    return true;
}

bool FUnrealMCPProjectIndex::RemoveAsset(const FString& ObjectPath, const FString& PackageName, FString* OutError)
{
    return ExecuteBoundStatement(Database,
            TEXT("DELETE FROM blueprint_edges WHERE blueprint_object_path = ?1;"),
            [&ObjectPath](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath);
            },
            OutError)
        && ExecuteBoundStatement(Database,
            TEXT("DELETE FROM blueprint_pins WHERE blueprint_object_path = ?1;"),
            [&ObjectPath](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath);
            },
            OutError)
        && ExecuteBoundStatement(Database,
            TEXT("DELETE FROM blueprint_nodes WHERE blueprint_object_path = ?1;"),
            [&ObjectPath](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath);
            },
            OutError)
        && ExecuteBoundStatement(Database,
            TEXT("DELETE FROM blueprint_graphs WHERE blueprint_object_path = ?1;"),
            [&ObjectPath](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath);
            },
            OutError)
        && ExecuteBoundStatement(Database,
            TEXT("DELETE FROM blueprint_variables WHERE blueprint_object_path = ?1;"),
            [&ObjectPath](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath);
            },
            OutError)
        && ExecuteBoundStatement(Database,
            TEXT("DELETE FROM blueprint_functions WHERE blueprint_object_path = ?1;"),
            [&ObjectPath](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath);
            },
            OutError)
        && ExecuteBoundStatement(Database,
            TEXT("DELETE FROM blueprint_components WHERE blueprint_object_path = ?1;"),
            [&ObjectPath](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath);
            },
            OutError)
        && ExecuteBoundStatement(Database,
            TEXT("DELETE FROM asset_dependencies WHERE source_package_name = ?1;"),
            [&PackageName](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, PackageName);
            },
            OutError)
        && ExecuteBoundStatement(Database,
            TEXT("DELETE FROM assets WHERE object_path = ?1;"),
            [&ObjectPath](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath);
            },
            OutError);
}
