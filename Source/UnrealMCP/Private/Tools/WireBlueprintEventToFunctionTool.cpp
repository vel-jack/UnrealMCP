#include "Tools/WireBlueprintEventToFunctionTool.h"

#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    UEdGraphPin* FindPin(UEdGraphNode* Node, const FName Name, const EEdGraphPinDirection Direction)
    {
        return Node == nullptr ? nullptr : Node->FindPin(Name, Direction);
    }

    UEdGraphPin* FindValueOutput(UK2Node_VariableGet* Node)
    {
        if (Node == nullptr)
        {
            return nullptr;
        }
        UEdGraphPin** Match = Node->Pins.FindByPredicate([](const UEdGraphPin* Pin)
        {
            return Pin != nullptr && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec;
        });
        return Match != nullptr ? *Match : nullptr;
    }

    UK2Node_CustomEvent* FindCustomEvent(UEdGraph* Graph, const FString& EventName)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node);
            if (Event != nullptr && Event->CustomFunctionName.ToString().Equals(EventName, ESearchCase::IgnoreCase))
            {
                return Event;
            }
        }
        return nullptr;
    }

    UK2Node_CallFunction* FindConnectedCall(UEdGraphPin* EventThen, const UFunction* Function)
    {
        if (EventThen == nullptr || Function == nullptr)
        {
            return nullptr;
        }
        for (UEdGraphPin* LinkedPin : EventThen->LinkedTo)
        {
            UK2Node_CallFunction* Call = LinkedPin != nullptr ? Cast<UK2Node_CallFunction>(LinkedPin->GetOwningNode()) : nullptr;
            if (Call != nullptr && Call->GetTargetFunction() == Function)
            {
                return Call;
            }
        }
        return nullptr;
    }

    UK2Node_VariableGet* FindConnectedTargetGet(UEdGraphPin* TargetPin, const FString& VariableName)
    {
        if (TargetPin == nullptr)
        {
            return nullptr;
        }
        for (UEdGraphPin* LinkedPin : TargetPin->LinkedTo)
        {
            UK2Node_VariableGet* GetNode = LinkedPin != nullptr ? Cast<UK2Node_VariableGet>(LinkedPin->GetOwningNode()) : nullptr;
            if (GetNode != nullptr && GetNode->VariableReference.GetMemberName().ToString().Equals(VariableName, ESearchCase::IgnoreCase))
            {
                return GetNode;
            }
        }
        return nullptr;
    }

    TSharedRef<FJsonObject> SerializeNode(const UEdGraphNode* Node)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("nodeGuid"), UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node));
        Result->SetNumberField(TEXT("positionX"), Node != nullptr ? Node->NodePosX : 0);
        Result->SetNumberField(TEXT("positionY"), Node != nullptr ? Node->NodePosY : 0);
        Result->SetArrayField(TEXT("pins"), UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(Node));
        return Result;
    }
}

FWireBlueprintEventToFunctionTool::FWireBlueprintEventToFunctionTool()
    : FMCPToolBase(
        TEXT("WireBlueprintEventToFunction"),
        TEXT("Idempotently ensures a custom event calls one exact Blueprint-callable function, optionally through a Blueprint component/member target. Refuses to replace existing event flow."))
{
}

UnrealMCP::FMCPResponse FWireBlueprintEventToFunctionTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, GraphName, GraphGuid, EventName, OwnerClassPath, FunctionName, TargetVariableName;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("eventName"), EventName)
        || !Request.Params->TryGetStringField(TEXT("ownerClassPath"), OwnerClassPath)
        || !Request.Params->TryGetStringField(TEXT("functionName"), FunctionName))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams,
            TEXT("WireBlueprintEventToFunction requires objectPath, graphName/graphGuid, eventName, ownerClassPath, and functionName."));
    }
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("targetVariableName"), TargetVariableName);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("graphName or graphGuid is required."));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), true);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), true);
    bool bEventAdded = false, bCallAdded = false, bTargetGetAdded = false, bExecConnected = false, bTargetConnected = false;
    bool bAlreadyComplete = false, bCompileSucceeded = false, bIndexRefreshed = false;
    int32 CompileErrors = 0, CompileWarnings = 0;
    FString SavedFilename, IndexError, ExecutionError;
    TSharedPtr<FJsonObject> EventJson, CallJson, TargetJson;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        UEdGraph* Graph = nullptr;
        UClass* OwnerClass = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)
            || !BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError)
            || !BlueprintEditToolUtils::ResolveClass(OwnerClassPath, OwnerClass, OutError))
        {
            return false;
        }
        UFunction* Function = OwnerClass->FindFunctionByName(*FunctionName, EIncludeSuperFlag::IncludeSuper);
        if (Function == nullptr || !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable) || Function->HasAnyFunctionFlags(FUNC_BlueprintPure))
        {
            OutError = TEXT("The requested function must resolve to an impure BlueprintCallable UFunction.");
            return false;
        }
        const bool bSelfCall = Blueprint->GeneratedClass != nullptr && Blueprint->GeneratedClass->IsChildOf(Function->GetOwnerClass());
        if (!bSelfCall && TargetVariableName.IsEmpty())
        {
            OutError = TEXT("targetVariableName is required when the function owner is not a base class of the target Blueprint.");
            return false;
        }
        if (!TargetVariableName.IsEmpty()
            && FBlueprintEditorUtils::FindMemberVariableGuidByName(Blueprint, *TargetVariableName).IsValid() == false
            && (Blueprint->GeneratedClass == nullptr || Blueprint->GeneratedClass->FindPropertyByName(*TargetVariableName) == nullptr))
        {
            OutError = FString::Printf(TEXT("Target member '%s' was not found on the Blueprint."), *TargetVariableName);
            return false;
        }

        UK2Node_CustomEvent* Event = FindCustomEvent(Graph, EventName);
        UEdGraphPin* EventThen = Event != nullptr ? FindPin(Event, UEdGraphSchema_K2::PN_Then, EGPD_Output) : nullptr;
        UK2Node_CallFunction* Call = FindConnectedCall(EventThen, Function);
        if (EventThen != nullptr && Call == nullptr && !EventThen->LinkedTo.IsEmpty())
        {
            OutError = TEXT("The existing custom event already has execution flow. This workflow will not replace or branch existing wiring.");
            return false;
        }
        UEdGraphPin* CallTarget = Call != nullptr ? FindPin(Call, UEdGraphSchema_K2::PN_Self, EGPD_Input) : nullptr;
        UK2Node_VariableGet* TargetGet = TargetVariableName.IsEmpty() ? nullptr : FindConnectedTargetGet(CallTarget, TargetVariableName);
        if (!TargetVariableName.IsEmpty() && CallTarget != nullptr && TargetGet == nullptr && !CallTarget->LinkedTo.IsEmpty())
        {
            OutError = TEXT("The existing function call already has a different target connection. This workflow will not replace it.");
            return false;
        }
        bAlreadyComplete = Event != nullptr && Call != nullptr && (TargetVariableName.IsEmpty() || TargetGet != nullptr);
        if (bDryRun || bAlreadyComplete)
        {
            if (Event != nullptr) EventJson = SerializeNode(Event);
            if (Call != nullptr) CallJson = SerializeNode(Call);
            if (TargetGet != nullptr) TargetJson = SerializeNode(TargetGet);
            return true;
        }

        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "WireEventToFunction", "UnrealMCP Wire Event To Function"));
        Blueprint->Modify();
        Graph->Modify();
        if (Event == nullptr)
        {
            Event = NewObject<UK2Node_CustomEvent>(Graph);
            Event->CustomFunctionName = *EventName;
            BlueprintGraphEditToolUtils::FPlacement Placement = BlueprintGraphEditToolUtils::ResolvePlacement(Graph, Request.Params, OutError);
            if (!OutError.IsEmpty()) return false;
            BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Event, Placement);
            bEventAdded = true;
            EventThen = FindPin(Event, UEdGraphSchema_K2::PN_Then, EGPD_Output);
        }
        if (Call == nullptr)
        {
            Call = NewObject<UK2Node_CallFunction>(Graph);
            Call->SetFromFunction(Function);
            BlueprintGraphEditToolUtils::FPlacement Placement;
            Placement.X = Event->NodePosX + 380;
            Placement.Y = Event->NodePosY;
            BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Call, Placement);
            bCallAdded = true;
        }

        const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
        UEdGraphPin* CallExec = FindPin(Call, UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (Schema == nullptr || EventThen == nullptr || CallExec == nullptr)
        {
            OutError = TEXT("Could not resolve K2 execution pins for the event-to-function connection.");
            return false;
        }
        if (!EventThen->LinkedTo.Contains(CallExec))
        {
            if (!Schema->TryCreateConnection(EventThen, CallExec))
            {
                OutError = TEXT("Unreal rejected the event-to-function execution connection.");
                return false;
            }
            bExecConnected = true;
        }

        if (!TargetVariableName.IsEmpty())
        {
            CallTarget = FindPin(Call, UEdGraphSchema_K2::PN_Self, EGPD_Input);
            if (CallTarget == nullptr)
            {
                OutError = TEXT("The requested member function call has no target pin.");
                return false;
            }
            TargetGet = FindConnectedTargetGet(CallTarget, TargetVariableName);
            if (TargetGet == nullptr)
            {
                TargetGet = NewObject<UK2Node_VariableGet>(Graph);
                TargetGet->VariableReference.SetSelfMember(
                    *TargetVariableName,
                    FBlueprintEditorUtils::FindMemberVariableGuidByName(Blueprint, *TargetVariableName));
                BlueprintGraphEditToolUtils::FPlacement Placement;
                Placement.X = Call->NodePosX - 20;
                Placement.Y = Call->NodePosY - 180;
                BlueprintGraphEditToolUtils::PlaceNewNode(Graph, TargetGet, Placement);
                UEdGraphPin* ValuePin = FindValueOutput(TargetGet);
                if (ValuePin == nullptr || !Schema->TryCreateConnection(ValuePin, CallTarget))
                {
                    OutError = TEXT("Unreal rejected the component/member target connection.");
                    return false;
                }
                bTargetGetAdded = true;
                bTargetConnected = true;
            }
        }

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        if (bCompile)
        {
            FCompilerResultsLog CompilerLog;
            CompilerLog.bSilentMode = true;
            FKismetEditorUtilities::CompileBlueprint(
                Blueprint,
                EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection,
                &CompilerLog);
            CompileErrors = CompilerLog.NumErrors;
            CompileWarnings = CompilerLog.NumWarnings;
            bCompileSucceeded = CompileErrors == 0
                && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
            if (!bCompileSucceeded)
            {
                OutError = TEXT("The workflow changed the graph, but Blueprint validation failed. Use ValidateBlueprint for node diagnostics.");
                return false;
            }
        }
        if (bSave && !BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, OutError))
        {
            return false;
        }
        if (bSave)
        {
            bIndexRefreshed = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexError);
        }
        EventJson = SerializeNode(Event);
        CallJson = SerializeNode(Call);
        if (TargetGet != nullptr) TargetJson = SerializeNode(TargetGet);
        return true;
    }, ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError);
    }

    const bool bChanged = bEventAdded || bCallAdded || bTargetGetAdded || bExecConnected || bTargetConnected;
    FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("eventName"), EventName);
    Result->SetStringField(TEXT("functionName"), FunctionName);
    Result->SetStringField(TEXT("ownerClassPath"), OwnerClassPath);
    Result->SetStringField(TEXT("targetVariableName"), TargetVariableName);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("alreadyComplete"), bAlreadyComplete);
    Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetBoolField(TEXT("eventAdded"), bEventAdded);
    Result->SetBoolField(TEXT("functionCallAdded"), bCallAdded);
    Result->SetBoolField(TEXT("targetGetAdded"), bTargetGetAdded);
    Result->SetBoolField(TEXT("executionConnected"), bExecConnected);
    Result->SetBoolField(TEXT("targetConnected"), bTargetConnected);
    if (EventJson.IsValid()) Result->SetObjectField(TEXT("eventNode"), EventJson.ToSharedRef());
    if (CallJson.IsValid()) Result->SetObjectField(TEXT("functionCallNode"), CallJson.ToSharedRef());
    if (TargetJson.IsValid()) Result->SetObjectField(TEXT("targetGetNode"), TargetJson.ToSharedRef());
    Result->SetBoolField(TEXT("compiled"), bCompile && bChanged && !bDryRun);
    Result->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
    Result->SetNumberField(TEXT("compileErrorCount"), CompileErrors);
    Result->SetNumberField(TEXT("compileWarningCount"), CompileWarnings);
    Result->SetBoolField(TEXT("saved"), bSave && bChanged && !bDryRun);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FWireBlueprintEventToFunctionTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Exact graph name; use graphGuid when ambiguous.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Stable graph GUID.")));
    Properties->SetObjectField(TEXT("eventName"), BuildStringProperty(TEXT("Custom event to create or reuse.")));
    Properties->SetObjectField(TEXT("ownerClassPath"), BuildStringProperty(TEXT("Exact class that owns the BlueprintCallable function.")));
    Properties->SetObjectField(TEXT("functionName"), BuildStringProperty(TEXT("Exact reflected UFunction name.")));
    Properties->SetObjectField(TEXT("targetVariableName"), BuildStringProperty(TEXT("Optional Blueprint component/member variable used as the call target.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Preflight the workflow without mutation.")));
    Properties->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile the touched Blueprint once. Defaults to true.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the touched asset once. Defaults to true.")));
    TSharedRef<FJsonObject> Integer = MakeShared<FJsonObject>();
    Integer->SetStringField(TEXT("type"), TEXT("integer"));
    Properties->SetObjectField(TEXT("positionX"), Integer);
    Properties->SetObjectField(TEXT("positionY"), Integer);
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("eventName")),
        MakeShared<FJsonValueString>(TEXT("ownerClassPath")),
        MakeShared<FJsonValueString>(TEXT("functionName"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
