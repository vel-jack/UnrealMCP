#include "Tools/SetBlueprintSequenceOutputsTool.h"

#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_ExecutionSequence.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    TArray<UEdGraphPin*> GetSequenceOutputPins(UK2Node_ExecutionSequence* SequenceNode)
    {
        TArray<UEdGraphPin*> Result;
        for (UEdGraphPin* Pin : SequenceNode->Pins)
        {
            if (Pin != nullptr && Pin->Direction == EGPD_Output && UEdGraphSchema_K2::IsExecPin(*Pin))
            {
                Result.Add(Pin);
            }
        }
        return Result;
    }

    TSharedPtr<FJsonValue> SerializeOutputPin(const UEdGraphPin* Pin)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("pinId"), UnrealMCP::BlueprintGraphEditToolUtils::GetPinId(Pin));
        Item->SetStringField(TEXT("pinName"), Pin != nullptr ? Pin->PinName.ToString() : FString());
        Item->SetNumberField(TEXT("linkedPinCount"), Pin != nullptr ? Pin->LinkedTo.Num() : 0);
        return MakeShared<FJsonValueObject>(Item);
    }

    TArray<TSharedPtr<FJsonValue>> SerializeOutputPins(const TArray<UEdGraphPin*>& Pins)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        Result.Reserve(Pins.Num());
        for (const UEdGraphPin* Pin : Pins)
        {
            Result.Add(SerializeOutputPin(Pin));
        }
        return Result;
    }
}

FSetBlueprintSequenceOutputsTool::FSetBlueprintSequenceOutputsTool()
    : FMCPToolBase(
        TEXT("SetBlueprintSequenceOutputs"),
        TEXT("Sets a Sequence node to 2-32 execution outputs. Removing outputs requires explicit confirmation, with separate confirmation for linked pins."))
{
}

UnrealMCP::FMCPResponse FSetBlueprintSequenceOutputsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FString GraphName;
    FString GraphGuid;
    FString NodeGuid;
    double RequestedCountValue = 0;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("nodeGuid"), NodeGuid)
        || !Request.Params->TryGetNumberField(TEXT("desiredOutputCount"), RequestedCountValue))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("SetBlueprintSequenceOutputs requires objectPath, graphName or graphGuid, nodeGuid, and desiredOutputCount."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Graph selector is required."));
    }

    const int32 RequestedCount = FMath::RoundToInt(RequestedCountValue);
    const int32 DesiredCount = FMath::Clamp(RequestedCount, 2, 32);
    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    const bool bConfirmRemove = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirmRemove"), false);
    const bool bConfirmRemoveLinkedPins = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirmRemoveLinkedPins"), false);

    int32 PreviousCount = 0;
    int32 AddedCount = 0;
    int32 RemovedCount = 0;
    int32 RemovedLinkCount = 0;
    bool bChanged = false;
    bool bIndexRefreshed = false;
    FString Filename;
    FString IndexError;
    FString ExecutionError;
    TArray<TSharedPtr<FJsonValue>> OutputPins;
    TArray<TSharedPtr<FJsonValue>> PinsSelectedForRemoval;

    const bool bExecuted = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& Error)
        {
            UBlueprint* Blueprint = nullptr;
            if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, Error))
            {
                return false;
            }

            UEdGraph* Graph = nullptr;
            if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, Error))
            {
                return false;
            }

            UEdGraphNode* ResolvedNode = nullptr;
            if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveNode(Graph, NodeGuid, ResolvedNode, Error))
            {
                return false;
            }

            UK2Node_ExecutionSequence* SequenceNode = Cast<UK2Node_ExecutionSequence>(ResolvedNode);
            if (SequenceNode == nullptr)
            {
                Error = TEXT("nodeGuid does not identify a UK2Node_ExecutionSequence node.");
                return false;
            }

            TArray<UEdGraphPin*> CurrentPins = GetSequenceOutputPins(SequenceNode);
            PreviousCount = CurrentPins.Num();
            AddedCount = FMath::Max(0, DesiredCount - PreviousCount);
            RemovedCount = FMath::Max(0, PreviousCount - DesiredCount);

            if (RemovedCount > 0)
            {
                for (int32 Index = CurrentPins.Num() - RemovedCount; Index < CurrentPins.Num(); ++Index)
                {
                    UEdGraphPin* Pin = CurrentPins[Index];
                    PinsSelectedForRemoval.Add(SerializeOutputPin(Pin));
                    RemovedLinkCount += Pin->LinkedTo.Num();
                }

                if (!bDryRun && !bConfirmRemove)
                {
                    Error = TEXT("Reducing Sequence outputs requires confirmRemove=true.");
                    return false;
                }
                if (!bDryRun && RemovedLinkCount > 0 && !bConfirmRemoveLinkedPins)
                {
                    Error = TEXT("One or more Sequence outputs selected for removal are linked. Set confirmRemoveLinkedPins=true to remove them and break their links.");
                    return false;
                }
            }

            if (bDryRun || (AddedCount == 0 && RemovedCount == 0))
            {
                OutputPins = SerializeOutputPins(CurrentPins);
                return true;
            }

            const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
            if (Schema == nullptr)
            {
                Error = TEXT("Graph does not use the K2 schema.");
                return false;
            }

            const FScopedTransaction Transaction(NSLOCTEXT(
                "UnrealMCP", "SetBlueprintSequenceOutputs", "UnrealMCP Set Blueprint Sequence Outputs"));
            Blueprint->Modify();
            Graph->Modify();
            SequenceNode->Modify();

            while (GetSequenceOutputPins(SequenceNode).Num() < DesiredCount)
            {
                SequenceNode->AddInputPin();
            }

            while (GetSequenceOutputPins(SequenceNode).Num() > DesiredCount)
            {
                TArray<UEdGraphPin*> MutablePins = GetSequenceOutputPins(SequenceNode);
                UEdGraphPin* PinToRemove = MutablePins.Last();
                if (PinToRemove->LinkedTo.Num() > 0)
                {
                    Schema->BreakPinLinks(*PinToRemove, true);
                }
                SequenceNode->RemovePinFromExecutionNode(PinToRemove);
            }

            bChanged = true;
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            OutputPins = SerializeOutputPins(GetSequenceOutputPins(SequenceNode));
            return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
                Blueprint, ObjectPath, bSave, Filename, bIndexRefreshed, IndexError, Error);
        },
        ExecutionError);

    if (!bExecuted)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedPtr<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetNumberField(TEXT("requestedOutputCount"), RequestedCount);
    Result->SetNumberField(TEXT("desiredOutputCount"), DesiredCount);
    Result->SetBoolField(TEXT("countClamped"), RequestedCount != DesiredCount);
    Result->SetNumberField(TEXT("previousOutputCount"), PreviousCount);
    Result->SetNumberField(TEXT("addedOutputCount"), AddedCount);
    Result->SetNumberField(TEXT("removedOutputCount"), RemovedCount);
    Result->SetNumberField(TEXT("removedLinkCount"), RemovedLinkCount);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetArrayField(TEXT("outputPins"), OutputPins);
    Result->SetArrayField(TEXT("pinsSelectedForRemoval"), PinsSelectedForRemoval);
    Result->SetBoolField(TEXT("saved"), bSave && bChanged);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FSetBlueprintSequenceOutputsTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;

    TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Stable or unique graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Exact graph GUID.")));
    Properties->SetObjectField(TEXT("nodeGuid"), BuildStringProperty(TEXT("Exact Sequence node GUID.")));

    TSharedPtr<FJsonObject> CountProperty = MakeShared<FJsonObject>();
    CountProperty->SetStringField(TEXT("type"), TEXT("integer"));
    CountProperty->SetStringField(TEXT("description"), TEXT("Desired output count, clamped to the supported range 2-32."));
    Properties->SetObjectField(TEXT("desiredOutputCount"), CountProperty);
    Properties->SetObjectField(TEXT("confirmRemove"), BuildBoolProperty(TEXT("Required to remove any Sequence output pins.")));
    Properties->SetObjectField(TEXT("confirmRemoveLinkedPins"), BuildBoolProperty(TEXT("Additionally required when removed outputs have links.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Preview count and affected trailing pins without mutation.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index after mutation.")));
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Required.Add(MakeShared<FJsonValueString>(TEXT("nodeGuid")));
    Required.Add(MakeShared<FJsonValueString>(TEXT("desiredOutputCount")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
