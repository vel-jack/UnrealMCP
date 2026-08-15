#include "Tools/AddEnhancedInputActionNodeTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/UnrealType.h"

FAddEnhancedInputActionNodeTool::FAddEnhancedInputActionNodeTool()
    : FMCPToolBase(
        TEXT("AddEnhancedInputActionNode"),
        TEXT("Adds an Enhanced Input Action event node for an exact InputAction asset without requiring Enhanced Input as a hard plugin dependency."))
{
}

UnrealMCP::FMCPResponse FAddEnhancedInputActionNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

    FString ObjectPath;
    FString GraphName;
    FString GraphGuid;
    FString InputActionPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("inputActionPath"), InputActionPath))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("AddEnhancedInputActionNode requires objectPath, inputActionPath, and graph selector."));
    }
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("graphName or graphGuid is required."));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    bool bAlreadyExists = false;
    bool bIndexRefreshed = false;
    FString NodeGuid;
    FString NodeTitle;
    FString ResolvedActionPath;
    FString SavedFilename;
    FString IndexRefreshError;
    FString ExecutionError;
    BlueprintGraphEditToolUtils::FPlacement Placement;
    TArray<TSharedPtr<FJsonValue>> Pins;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError))
        {
            return false;
        }
        UEdGraph* Graph = nullptr;
        if (!BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError))
        {
            return false;
        }

        UClass* NodeClass = LoadObject<UClass>(nullptr, TEXT("/Script/InputBlueprintNodes.K2Node_EnhancedInputAction"));
        if (NodeClass == nullptr || !NodeClass->IsChildOf<UEdGraphNode>())
        {
            OutError = TEXT("Enhanced Input Blueprint nodes are unavailable. Enable the Enhanced Input plugin for this project.");
            return false;
        }
        FObjectPropertyBase* InputActionProperty = FindFProperty<FObjectPropertyBase>(NodeClass, TEXT("InputAction"));
        if (InputActionProperty == nullptr)
        {
            OutError = TEXT("Enhanced Input node class does not expose the expected InputAction property.");
            return false;
        }
        UObject* InputAction = LoadObject<UObject>(nullptr, *InputActionPath);
        if (InputAction == nullptr || !InputAction->IsA(InputActionProperty->PropertyClass))
        {
            OutError = FString::Printf(TEXT("Could not load InputAction asset '%s'."), *InputActionPath);
            return false;
        }
        ResolvedActionPath = InputAction->GetPathName();

        for (UEdGraphNode* ExistingNode : Graph->Nodes)
        {
            if (ExistingNode != nullptr && ExistingNode->IsA(NodeClass)
                && InputActionProperty->GetObjectPropertyValue_InContainer(ExistingNode) == InputAction)
            {
                bAlreadyExists = true;
                NodeGuid = BlueprintGraphEditToolUtils::GetNodeGuid(ExistingNode);
                NodeTitle = ExistingNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
                Pins = BlueprintGraphEditToolUtils::SerializePins(ExistingNode);
                return true;
            }
        }

        Placement = BlueprintGraphEditToolUtils::ResolvePlacement(Graph, Request.Params, OutError);
        if (!OutError.IsEmpty() || bDryRun)
        {
            return OutError.IsEmpty();
        }

        const FScopedTransaction Transaction(
            NSLOCTEXT("UnrealMCP", "AddEnhancedInputActionNode", "UnrealMCP Add Enhanced Input Action Node"));
        Blueprint->Modify();
        Graph->Modify();
        UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass);
        InputActionProperty->SetObjectPropertyValue_InContainer(Node, InputAction);
        BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, Placement);
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        NodeGuid = BlueprintGraphEditToolUtils::GetNodeGuid(Node);
        NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        Pins = BlueprintGraphEditToolUtils::SerializePins(Node);
        return BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
            Blueprint,
            ObjectPath,
            bSave,
            SavedFilename,
            bIndexRefreshed,
            IndexRefreshError,
            OutError);
    }, ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError);
    }

    FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("inputActionPath"), ResolvedActionPath);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetStringField(TEXT("nodeTitle"), NodeTitle);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("alreadyExists"), bAlreadyExists);
    Result->SetBoolField(TEXT("added"), !bDryRun && !bAlreadyExists);
    Result->SetNumberField(TEXT("positionX"), Placement.X);
    Result->SetNumberField(TEXT("positionY"), Placement.Y);
    Result->SetBoolField(TEXT("collisionAdjusted"), Placement.bCollisionAdjusted);
    Result->SetArrayField(TEXT("pins"), Pins);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun && !bAlreadyExists);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddEnhancedInputActionNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Stable event graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Exact event graph GUID.")));
    Properties->SetObjectField(TEXT("inputActionPath"), BuildStringProperty(TEXT("Exact UInputAction asset object path.")));
    Properties->SetObjectField(TEXT("relativeToNodeGuid"), BuildStringProperty(TEXT("Optional placement anchor node GUID.")));
    TSharedRef<FJsonObject> IntegerProperty = MakeShared<FJsonObject>();
    IntegerProperty->SetStringField(TEXT("type"), TEXT("integer"));
    Properties->SetObjectField(TEXT("positionX"), IntegerProperty);
    Properties->SetObjectField(TEXT("positionY"), IntegerProperty);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate asset, plugin availability, graph, and placement.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("inputActionPath"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
