#include "Tools/SetBlueprintPinSplitTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FSetBlueprintPinSplitTool::FSetBlueprintPinSplitTool()
    : FMCPToolBase(
        TEXT("SetBlueprintPinSplit"),
        TEXT("Splits a Blueprint struct pin into member sub-pins, or explicitly recombines it, using stable graph/node/pin identifiers."))
{
}

UnrealMCP::FMCPResponse FSetBlueprintPinSplitTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

    FString ObjectPath;
    FString GraphName;
    FString GraphGuid;
    FString NodeGuid;
    FString PinId;
    FString PinName;
    FString Direction;
    bool bSplit = true;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("nodeGuid"), NodeGuid)
        || (!Request.Params->TryGetStringField(TEXT("pinId"), PinId)
            && !Request.Params->TryGetStringField(TEXT("pinName"), PinName)))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("SetBlueprintPinSplit requires objectPath, graph selector, nodeGuid, and pinId or pinName."));
    }
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("direction"), Direction);
    Request.Params->TryGetBoolField(TEXT("split"), bSplit);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("graphName or graphGuid is required."));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    const bool bConfirmRecombine = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirmRecombine"), false);
    bool bAlreadyInState = false;
    bool bIndexRefreshed = false;
    FString ResolvedPinId;
    FString ResolvedPinName;
    FString StructTypePath;
    FString SavedFilename;
    FString IndexRefreshError;
    FString ExecutionError;
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
        UEdGraphNode* Node = nullptr;
        if (!BlueprintGraphEditToolUtils::ResolveNode(Graph, NodeGuid, Node, OutError))
        {
            return false;
        }
        UEdGraphPin* Pin = nullptr;
        if (!BlueprintGraphEditToolUtils::ResolvePin(Node, PinId, PinName, Direction, Pin, OutError))
        {
            return false;
        }

        ResolvedPinId = BlueprintGraphEditToolUtils::GetPinId(Pin);
        ResolvedPinName = Pin->PinName.ToString();
        StructTypePath = Pin->PinType.PinSubCategoryObject.IsValid()
            ? Pin->PinType.PinSubCategoryObject->GetPathName()
            : FString();
        const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
        if (Schema == nullptr)
        {
            OutError = TEXT("The requested graph does not use the K2 Blueprint schema.");
            return false;
        }

        if (bSplit)
        {
            bAlreadyInState = Pin->SubPins.Num() > 0;
            if (!bAlreadyInState && !Schema->CanSplitStructPin(*Pin))
            {
                OutError = TEXT("The requested pin is not a splittable Blueprint struct pin.");
                return false;
            }
        }
        else
        {
            UEdGraphPin* ParentPin = Pin->ParentPin != nullptr ? Pin->ParentPin : Pin;
            Pin = ParentPin;
            ResolvedPinId = BlueprintGraphEditToolUtils::GetPinId(Pin);
            ResolvedPinName = Pin->PinName.ToString();
            bAlreadyInState = Pin->SubPins.IsEmpty();
            if (!bAlreadyInState && !bConfirmRecombine)
            {
                OutError = TEXT("Recombining can alter member-level wiring; set confirmRecombine=true after inspecting the sub-pins.");
                return false;
            }
        }

        if (bDryRun || bAlreadyInState)
        {
            Pins = BlueprintGraphEditToolUtils::SerializePins(Node);
            return true;
        }

        const FScopedTransaction Transaction(
            NSLOCTEXT("UnrealMCP", "SetBlueprintPinSplit", "UnrealMCP Set Blueprint Pin Split"));
        Blueprint->Modify();
        Graph->Modify();
        Node->Modify();
        if (bSplit)
        {
            Schema->SplitPin(Pin, true);
        }
        else
        {
            Schema->RecombinePin(Pin);
        }
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
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
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetStringField(TEXT("pinId"), ResolvedPinId);
    Result->SetStringField(TEXT("pinName"), ResolvedPinName);
    Result->SetStringField(TEXT("structTypePath"), StructTypePath);
    Result->SetBoolField(TEXT("split"), bSplit);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("alreadyInState"), bAlreadyInState);
    Result->SetBoolField(TEXT("changed"), !bDryRun && !bAlreadyInState);
    Result->SetArrayField(TEXT("nodePins"), Pins);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun && !bAlreadyInState);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FSetBlueprintPinSplitTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Stable graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Exact graph GUID.")));
    Properties->SetObjectField(TEXT("nodeGuid"), BuildStringProperty(TEXT("Exact node GUID.")));
    Properties->SetObjectField(TEXT("pinId"), BuildStringProperty(TEXT("Exact parent or member pin GUID.")));
    Properties->SetObjectField(TEXT("pinName"), BuildStringProperty(TEXT("Pin name fallback when unique.")));
    Properties->SetObjectField(TEXT("direction"), BuildStringProperty(TEXT("Optional input or output disambiguation.")));
    Properties->SetObjectField(TEXT("split"), BuildBoolProperty(TEXT("True to split; false to recombine. Defaults to true.")));
    Properties->SetObjectField(TEXT("confirmRecombine"), BuildBoolProperty(TEXT("Required when recombining existing sub-pins.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate and return the current pin model without mutation.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index after mutation.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("nodeGuid"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
