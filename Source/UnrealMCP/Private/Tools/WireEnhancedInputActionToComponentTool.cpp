#include "Tools/WireEnhancedInputActionToComponentTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintComponentEditUtils.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/UnrealType.h"

namespace
{
    bool IsSupportedPhase(const FString& Phase)
    {
        return Phase.Equals(TEXT("Started"), ESearchCase::IgnoreCase) || Phase.Equals(TEXT("Triggered"), ESearchCase::IgnoreCase)
            || Phase.Equals(TEXT("Ongoing"), ESearchCase::IgnoreCase) || Phase.Equals(TEXT("Canceled"), ESearchCase::IgnoreCase)
            || Phase.Equals(TEXT("Completed"), ESearchCase::IgnoreCase);
    }

    FString CanonicalPhase(const FString& Phase)
    {
        for (const TCHAR* Candidate : {TEXT("Started"), TEXT("Triggered"), TEXT("Ongoing"), TEXT("Canceled"), TEXT("Completed")})
            if (Phase.Equals(Candidate, ESearchCase::IgnoreCase)) return Candidate;
        return Phase;
    }

    UEdGraphPin* FindValueOutput(UK2Node_VariableGet* Node)
    {
        if (Node == nullptr) return nullptr;
        for (UEdGraphPin* Pin : Node->Pins) if (Pin != nullptr && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) return Pin;
        return nullptr;
    }

    bool HasExpectedTarget(UK2Node_CallFunction* Call, const FString& TargetVariableName)
    {
        UEdGraphPin* SelfPin = Call != nullptr ? Call->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input) : nullptr;
        if (SelfPin == nullptr) return false;
        for (UEdGraphPin* Linked : SelfPin->LinkedTo)
        {
            const UK2Node_VariableGet* Get = Linked != nullptr ? Cast<UK2Node_VariableGet>(Linked->GetOwningNode()) : nullptr;
            if (Get != nullptr && Get->VariableReference.GetMemberName().ToString().Equals(TargetVariableName, ESearchCase::IgnoreCase)) return true;
        }
        return false;
    }
}

FWireEnhancedInputActionToComponentTool::FWireEnhancedInputActionToComponentTool()
    : FMCPToolBase(TEXT("WireEnhancedInputActionToComponent"), TEXT("Idempotently inserts one component function call into an exact Enhanced Input action phase while preserving its existing execution continuation."))
{
}

UnrealMCP::FMCPResponse FWireEnhancedInputActionToComponentTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, GraphName, GraphGuid, InputActionPath, NodeGuid, Phase, OwnerClassPath, FunctionName, TargetVariableName;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("phase"), Phase) || !Request.Params->TryGetStringField(TEXT("ownerClassPath"), OwnerClassPath)
        || !Request.Params->TryGetStringField(TEXT("functionName"), FunctionName) || !Request.Params->TryGetStringField(TEXT("targetVariableName"), TargetVariableName))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("WireEnhancedInputActionToComponent requires objectPath, graph selector, action/node selector, phase, ownerClassPath, functionName, and targetVariableName."));
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName); Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("inputActionPath"), InputActionPath); Request.Params->TryGetStringField(TEXT("nodeGuid"), NodeGuid);
    if ((GraphName.IsEmpty() && GraphGuid.IsEmpty()) || (InputActionPath.IsEmpty() && NodeGuid.IsEmpty()) || !IsSupportedPhase(Phase))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide graphName/graphGuid, inputActionPath/nodeGuid, and a supported phase: Started, Triggered, Ongoing, Canceled, or Completed."));
    Phase = CanonicalPhase(Phase);
    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), true);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), true);
    bool bAlreadyComplete = false, bChanged = false, bPreservedContinuation = false, bCompileSucceeded = false, bIndexRefreshed = false;
    int32 CompileErrors = 0, CompileWarnings = 0;
    FString ActionNodeGuid, CallNodeGuid, TargetNodeGuid, SavedFilename, IndexError, Error;
    TArray<FString> UnconnectedInputs;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr; UEdGraph* Graph = nullptr; UClass* OwnerClass = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)
            || !BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError)
            || !BlueprintEditToolUtils::ResolveClass(OwnerClassPath, OwnerClass, OutError)) return false;
        UFunction* Function = OwnerClass->FindFunctionByName(*FunctionName, EIncludeSuperFlag::IncludeSuper);
        if (Function == nullptr || !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable) || Function->HasAnyFunctionFlags(FUNC_BlueprintPure))
        { OutError = TEXT("The requested component function must be an impure BlueprintCallable UFunction."); return false; }
        if (BlueprintComponentEditUtils::FindComponentNode(Blueprint, TargetVariableName) == nullptr
            && (Blueprint->GeneratedClass == nullptr || Blueprint->GeneratedClass->FindPropertyByName(*TargetVariableName) == nullptr))
        { OutError = FString::Printf(TEXT("Target component/member '%s' was not found on the Blueprint."), *TargetVariableName); return false; }

        UClass* ActionNodeClass = LoadObject<UClass>(nullptr, TEXT("/Script/InputBlueprintNodes.K2Node_EnhancedInputAction"));
        FObjectPropertyBase* ActionProperty = ActionNodeClass != nullptr ? FindFProperty<FObjectPropertyBase>(ActionNodeClass, TEXT("InputAction")) : nullptr;
        if (ActionNodeClass == nullptr || ActionProperty == nullptr) { OutError = TEXT("Enhanced Input Blueprint nodes are unavailable."); return false; }
        UEdGraphNode* ActionNode = nullptr;
        for (UEdGraphNode* Candidate : Graph->Nodes)
        {
            if (Candidate == nullptr || !Candidate->IsA(ActionNodeClass)) continue;
            UObject* Action = ActionProperty->GetObjectPropertyValue_InContainer(Candidate);
            const bool bGuidMatches = NodeGuid.IsEmpty() || BlueprintGraphEditToolUtils::GetNodeGuid(Candidate).Equals(NodeGuid, ESearchCase::IgnoreCase);
            const bool bActionMatches = InputActionPath.IsEmpty() || (Action != nullptr && Action->GetPathName().Equals(InputActionPath, ESearchCase::IgnoreCase));
            if (!bGuidMatches || !bActionMatches) continue;
            if (ActionNode != nullptr) { OutError = TEXT("Enhanced Input action selector is ambiguous; provide nodeGuid."); return false; }
            ActionNode = Candidate;
        }
        if (ActionNode == nullptr) { OutError = TEXT("Could not find the requested Enhanced Input Action node in the graph."); return false; }
        ActionNodeGuid = BlueprintGraphEditToolUtils::GetNodeGuid(ActionNode);
        UEdGraphPin* PhasePin = ActionNode->FindPin(*Phase, EGPD_Output);
        if (PhasePin == nullptr) { OutError = FString::Printf(TEXT("Enhanced Input node does not expose phase pin '%s'."), *Phase); return false; }
        if (PhasePin->LinkedTo.Num() > 1) { OutError = TEXT("The phase has multiple execution routes. Refusing ambiguous insertion; normalize through a Sequence node first."); return false; }
        UK2Node_CallFunction* ExistingCall = PhasePin->LinkedTo.Num() == 1 ? Cast<UK2Node_CallFunction>(PhasePin->LinkedTo[0]->GetOwningNode()) : nullptr;
        if (ExistingCall != nullptr && ExistingCall->GetTargetFunction() == Function)
        {
            if (!HasExpectedTarget(ExistingCall, TargetVariableName)) { OutError = TEXT("The phase already calls the requested function through a different target."); return false; }
            bAlreadyComplete = true; CallNodeGuid = BlueprintGraphEditToolUtils::GetNodeGuid(ExistingCall); return true;
        }
        if (bDryRun) return true;

        const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
        if (Schema == nullptr) { OutError = TEXT("The selected graph does not use the K2 schema."); return false; }
        UEdGraphPin* PreviousTarget = PhasePin->LinkedTo.Num() == 1 ? PhasePin->LinkedTo[0] : nullptr;
        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "WireEnhancedInputToComponent", "UnrealMCP Wire Enhanced Input To Component"));
        Blueprint->Modify(); Graph->Modify(); ActionNode->Modify();
        UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph); Call->SetFromFunction(Function);
        BlueprintGraphEditToolUtils::FPlacement CallPlacement; CallPlacement.X = ActionNode->NodePosX + 420; CallPlacement.Y = ActionNode->NodePosY;
        BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Call, CallPlacement); Call->NodeComment = FString::Printf(TEXT("UnrealMCP EnhancedInput %s -> %s.%s"), *Phase, *TargetVariableName, *FunctionName);
        UK2Node_VariableGet* TargetGet = NewObject<UK2Node_VariableGet>(Graph);
        TargetGet->VariableReference.SetSelfMember(*TargetVariableName, FBlueprintEditorUtils::FindMemberVariableGuidByName(Blueprint, *TargetVariableName));
        BlueprintGraphEditToolUtils::FPlacement GetPlacement; GetPlacement.X = Call->NodePosX; GetPlacement.Y = Call->NodePosY - 180; BlueprintGraphEditToolUtils::PlaceNewNode(Graph, TargetGet, GetPlacement);
        UEdGraphPin* CallExec = Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input); UEdGraphPin* CallThen = Call->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
        UEdGraphPin* CallSelf = Call->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input); UEdGraphPin* TargetValue = FindValueOutput(TargetGet);
        if (CallExec == nullptr || CallThen == nullptr || CallSelf == nullptr || TargetValue == nullptr) { OutError = TEXT("Could not resolve generated function-call or target pins."); return false; }
        if (PreviousTarget != nullptr) { PhasePin->BreakLinkTo(PreviousTarget); bPreservedContinuation = true; }
        if (!Schema->TryCreateConnection(PhasePin, CallExec) || !Schema->TryCreateConnection(TargetValue, CallSelf)
            || (PreviousTarget != nullptr && !Schema->TryCreateConnection(CallThen, PreviousTarget)))
        { OutError = TEXT("Unreal rejected one of the Enhanced Input route connections."); return false; }
        for (UEdGraphPin* Pin : Call->Pins)
        {
            if (Pin != nullptr && Pin->Direction == EGPD_Input && Pin != CallExec && Pin != CallSelf && Pin->LinkedTo.IsEmpty()
                && Pin->DefaultValue.IsEmpty() && Pin->DefaultObject == nullptr && Pin->DefaultTextValue.IsEmpty()) UnconnectedInputs.Add(Pin->PinName.ToString());
        }
        CallNodeGuid = BlueprintGraphEditToolUtils::GetNodeGuid(Call); TargetNodeGuid = BlueprintGraphEditToolUtils::GetNodeGuid(TargetGet); bChanged = true;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        if (bCompile)
        {
            FCompilerResultsLog Log; Log.bSilentMode = true; FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Log);
            CompileErrors = Log.NumErrors; CompileWarnings = Log.NumWarnings; bCompileSucceeded = CompileErrors == 0 && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
            if (!bCompileSucceeded) { OutError = TEXT("Enhanced Input wiring was created, but Blueprint compilation failed. Use ValidateBlueprint for diagnostics or undo the transaction."); return false; }
        }
        if (bSave && !BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, OutError)) return false;
        if (bSave) bIndexRefreshed = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexError); return true;
    }, Error);
    if (!bSucceeded) return BuildError(Request, EMCPErrorCode::InternalError, Error);

    TArray<TSharedPtr<FJsonValue>> Inputs; for (const FString& PinName : UnconnectedInputs) Inputs.Add(MakeShared<FJsonValueString>(PinName));
    FMCPResponse Response; Response.Id = Request.Id; TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath); Result->SetStringField(TEXT("phase"), Phase); Result->SetStringField(TEXT("inputActionPath"), InputActionPath);
    Result->SetStringField(TEXT("actionNodeGuid"), ActionNodeGuid); Result->SetStringField(TEXT("functionName"), FunctionName); Result->SetStringField(TEXT("targetVariableName"), TargetVariableName);
    Result->SetStringField(TEXT("functionCallNodeGuid"), CallNodeGuid); Result->SetStringField(TEXT("targetGetNodeGuid"), TargetNodeGuid); Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("alreadyComplete"), bAlreadyComplete); Result->SetBoolField(TEXT("changed"), bChanged); Result->SetBoolField(TEXT("preservedExistingContinuation"), bPreservedContinuation); Result->SetArrayField(TEXT("unconnectedInputPins"), Inputs);
    Result->SetBoolField(TEXT("compiled"), bCompile && bChanged); Result->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded); Result->SetNumberField(TEXT("compileErrorCount"), CompileErrors); Result->SetNumberField(TEXT("compileWarningCount"), CompileWarnings);
    Result->SetBoolField(TEXT("saved"), bSave && bChanged); Result->SetStringField(TEXT("savedFilename"), SavedFilename); Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed); Result->SetStringField(TEXT("indexRefreshError"), IndexError); Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FWireEnhancedInputActionToComponentTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object")); TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path."))); P->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Exact graph name."))); P->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Exact graph GUID.")));
    P->SetObjectField(TEXT("inputActionPath"), BuildStringProperty(TEXT("Exact InputAction object path; nodeGuid is preferred when duplicate action nodes exist."))); P->SetObjectField(TEXT("nodeGuid"), BuildStringProperty(TEXT("Exact Enhanced Input Action node GUID.")));
    P->SetObjectField(TEXT("phase"), BuildStringProperty(TEXT("Started, Triggered, Ongoing, Canceled, or Completed."))); P->SetObjectField(TEXT("ownerClassPath"), BuildStringProperty(TEXT("Exact class owning the impure BlueprintCallable function.")));
    P->SetObjectField(TEXT("functionName"), BuildStringProperty(TEXT("Exact component function name."))); P->SetObjectField(TEXT("targetVariableName"), BuildStringProperty(TEXT("Existing Blueprint component/member variable used as call target.")));
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate the route without mutation."))); P->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile once after mutation; defaults true."))); P->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh once after mutation; defaults true.")));
    Schema->SetObjectField(TEXT("properties"), P); Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("phase")), MakeShared<FJsonValueString>(TEXT("ownerClassPath")), MakeShared<FJsonValueString>(TEXT("functionName")), MakeShared<FJsonValueString>(TEXT("targetVariableName"))}); return Schema;
}
