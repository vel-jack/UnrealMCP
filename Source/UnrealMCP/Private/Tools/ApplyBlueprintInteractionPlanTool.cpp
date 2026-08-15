#include "Tools/ApplyBlueprintInteractionPlanTool.h"

#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    struct FPlanPinDefault
    {
        FString PinName, DefaultValue, DefaultObjectPath;
        UObject* ResolvedObject = nullptr;
        bool bApplied = false;
    };

    struct FPlanNode
    {
        FString Id, Type, NodeGuid, EventName, OwnerClassPath, FunctionName, VariableName;
        FString SpliceAfterNodeId, SpliceAfterPinName;
        int32 X = 0, Y = 0;
        bool bHasPosition = false, bCreated = false, bSpliceApplied = false, bSpliceAlreadyComplete = false;
        UFunction* Function = nullptr;
        FGuid VariableGuid;
        UEdGraphNode* Existing = nullptr;
        UEdGraphNode* Preview = nullptr;
        UEdGraphNode* Applied = nullptr;
        TArray<FPlanPinDefault> InputDefaults;
    };

    struct FPlanConnection
    {
        FString SourceId, SourcePin, TargetId, TargetPin;
        bool bAlreadyConnected = false, bConnected = false;
    };

    struct FPlanExecutionInsertion
    {
        FString SourceId, SourcePin, InsertedId, InsertedInputPin, InsertedOutputPin, TargetId, TargetPin;
        bool bAlreadyComplete = false, bApplied = false;
    };

    FString Marker(const FString& WorkflowId, const FString& NodeId)
    {
        return FString::Printf(TEXT("UnrealMCP.Workflow:%s:%s"), *WorkflowId, *NodeId);
    }

    bool ParseNode(const TSharedPtr<FJsonObject>& Object, FPlanNode& Out, FString& Error)
    {
        if (!Object.IsValid() || !Object->TryGetStringField(TEXT("id"), Out.Id)
            || !Object->TryGetStringField(TEXT("type"), Out.Type) || Out.Id.IsEmpty())
        {
            Error = TEXT("Each node requires non-empty id and type fields.");
            return false;
        }
        Object->TryGetStringField(TEXT("eventName"), Out.EventName);
        Object->TryGetStringField(TEXT("nodeGuid"), Out.NodeGuid);
        Object->TryGetStringField(TEXT("ownerClassPath"), Out.OwnerClassPath);
        Object->TryGetStringField(TEXT("functionName"), Out.FunctionName);
        Object->TryGetStringField(TEXT("variableName"), Out.VariableName);
        Object->TryGetStringField(TEXT("spliceAfterNodeId"), Out.SpliceAfterNodeId);
        Object->TryGetStringField(TEXT("spliceAfterPinName"), Out.SpliceAfterPinName);
        const TArray<TSharedPtr<FJsonValue>>* DefaultValues = nullptr;
        if (Object->TryGetArrayField(TEXT("inputDefaults"), DefaultValues) && DefaultValues)
        {
            for (const TSharedPtr<FJsonValue>& Value : *DefaultValues)
            {
                const TSharedPtr<FJsonObject> DefaultObject = Value.IsValid() ? Value->AsObject() : nullptr;
                FPlanPinDefault Default;
                if (!DefaultObject.IsValid() || !DefaultObject->TryGetStringField(TEXT("pinName"), Default.PinName)
                    || Default.PinName.IsEmpty())
                { Error = TEXT("Each inputDefaults entry requires pinName."); return false; }
                const bool HasLiteral = DefaultObject->TryGetStringField(TEXT("defaultValue"), Default.DefaultValue);
                const bool HasObject = DefaultObject->TryGetStringField(TEXT("defaultObjectPath"), Default.DefaultObjectPath);
                if (HasLiteral == HasObject)
                { Error = TEXT("Each inputDefaults entry requires exactly one of defaultValue or defaultObjectPath."); return false; }
                if (Out.InputDefaults.ContainsByPredicate([&](const FPlanPinDefault& Existing){ return Existing.PinName.Equals(Default.PinName, ESearchCase::IgnoreCase); }))
                { Error = FString::Printf(TEXT("Duplicate input default for pin '%s'."), *Default.PinName); return false; }
                Out.InputDefaults.Add(MoveTemp(Default));
            }
        }
        double X = 0, Y = 0;
        const bool HasX = Object->TryGetNumberField(TEXT("positionX"), X);
        const bool HasY = Object->TryGetNumberField(TEXT("positionY"), Y);
        if (HasX != HasY) { Error = TEXT("Node positions require both positionX and positionY."); return false; }
        Out.bHasPosition = HasX; Out.X = FMath::RoundToInt(X); Out.Y = FMath::RoundToInt(Y);
        if (Out.Type == TEXT("existingNode"))
        {
            FGuid ParsedGuid;
            if (!FGuid::Parse(Out.NodeGuid, ParsedGuid)) { Error = TEXT("existingNode requires a valid nodeGuid."); return false; }
        }
        else if (Out.Type == TEXT("customEvent"))
        {
            if (Out.EventName.IsEmpty()) { Error = TEXT("customEvent requires eventName."); return false; }
        }
        else if (Out.Type == TEXT("functionCall"))
        {
            if (Out.OwnerClassPath.IsEmpty() || Out.FunctionName.IsEmpty())
            { Error = TEXT("functionCall requires ownerClassPath and functionName."); return false; }
        }
        else if (Out.Type == TEXT("variableGet"))
        {
            if (Out.VariableName.IsEmpty()) { Error = TEXT("variableGet requires variableName."); return false; }
        }
        else if (Out.Type == TEXT("sequence"))
        {
            if (Out.SpliceAfterNodeId.IsEmpty() || Out.SpliceAfterPinName.IsEmpty())
            { Error = TEXT("sequence requires spliceAfterNodeId and spliceAfterPinName."); return false; }
        }
        else if (Out.Type != TEXT("branch"))
        {
            Error = FString::Printf(TEXT("Unsupported node type '%s'."), *Out.Type);
            return false;
        }
        return true;
    }

    bool ParseConnection(const TSharedPtr<FJsonObject>& Object, FPlanConnection& Out, FString& Error)
    {
        if (!Object.IsValid()
            || !Object->TryGetStringField(TEXT("sourceNodeId"), Out.SourceId)
            || !Object->TryGetStringField(TEXT("sourcePinName"), Out.SourcePin)
            || !Object->TryGetStringField(TEXT("targetNodeId"), Out.TargetId)
            || !Object->TryGetStringField(TEXT("targetPinName"), Out.TargetPin)
            || Out.SourceId.IsEmpty() || Out.SourcePin.IsEmpty() || Out.TargetId.IsEmpty() || Out.TargetPin.IsEmpty())
        {
            Error = TEXT("Connections require sourceNodeId/sourcePinName and targetNodeId/targetPinName.");
            return false;
        }
        return true;
    }

    bool ParseExecutionInsertion(const TSharedPtr<FJsonObject>& Object, FPlanExecutionInsertion& Out, FString& Error)
    {
        if (!Object.IsValid()
            || !Object->TryGetStringField(TEXT("sourceNodeId"), Out.SourceId)
            || !Object->TryGetStringField(TEXT("sourcePinName"), Out.SourcePin)
            || !Object->TryGetStringField(TEXT("insertedNodeId"), Out.InsertedId)
            || !Object->TryGetStringField(TEXT("insertedInputPinName"), Out.InsertedInputPin)
            || !Object->TryGetStringField(TEXT("insertedOutputPinName"), Out.InsertedOutputPin)
            || !Object->TryGetStringField(TEXT("targetNodeId"), Out.TargetId)
            || !Object->TryGetStringField(TEXT("targetPinName"), Out.TargetPin))
        { Error = TEXT("Execution insertions require exact source, inserted, and target node/pin fields."); return false; }
        return true;
    }

    UEdGraphNode* FindExisting(UEdGraph* Graph, const FString& WorkflowId, const FPlanNode& Spec)
    {
        if (Spec.Type == TEXT("existingNode"))
        {
            FGuid ParsedGuid;
            FGuid::Parse(Spec.NodeGuid, ParsedGuid);
            for (UEdGraphNode* Node : Graph->Nodes) if (Node && Node->NodeGuid == ParsedGuid) return Node;
            return nullptr;
        }
        const FString ExpectedMarker = Marker(WorkflowId, Spec.Id);
        for (UEdGraphNode* Node : Graph->Nodes) if (Node && Node->NodeComment == ExpectedMarker) return Node;
        if (Spec.Type == TEXT("customEvent"))
        {
            for (UEdGraphNode* Node : Graph->Nodes)
                if (const UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node);
                    Event && Event->CustomFunctionName.ToString().Equals(Spec.EventName, ESearchCase::IgnoreCase)) return Node;
        }
        return nullptr;
    }

    bool ExistingMatches(const FPlanNode& Spec)
    {
        if (Spec.Type == TEXT("existingNode")) return Spec.Existing != nullptr;
        if (!Spec.Existing) return true;
        if (Spec.Type == TEXT("customEvent"))
        {
            const UK2Node_CustomEvent* Node = Cast<UK2Node_CustomEvent>(Spec.Existing);
            return Node && Node->CustomFunctionName.ToString().Equals(Spec.EventName, ESearchCase::IgnoreCase);
        }
        if (Spec.Type == TEXT("functionCall"))
        {
            const UK2Node_CallFunction* Node = Cast<UK2Node_CallFunction>(Spec.Existing);
            return Node && Node->GetTargetFunction() == Spec.Function;
        }
        if (Spec.Type == TEXT("variableGet"))
        {
            const UK2Node_VariableGet* Node = Cast<UK2Node_VariableGet>(Spec.Existing);
            return Node && Node->VariableReference.GetMemberName().ToString().Equals(Spec.VariableName, ESearchCase::IgnoreCase);
        }
        if (Spec.Type == TEXT("branch")) return Spec.Existing->IsA<UK2Node_IfThenElse>();
        return Spec.Type == TEXT("sequence") && Spec.Existing->IsA<UK2Node_ExecutionSequence>();
    }

    UEdGraphNode* NewPlanNode(UEdGraph* Graph, const FPlanNode& Spec, bool Detached)
    {
        if (Spec.Type == TEXT("existingNode")) return nullptr;
        UEdGraphNode* Node = nullptr;
        if (Spec.Type == TEXT("customEvent"))
        {
            UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
            Event->CustomFunctionName = *Spec.EventName; Node = Event;
        }
        else if (Spec.Type == TEXT("functionCall"))
        {
            UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
            Call->SetFromFunction(Spec.Function); Node = Call;
        }
        else if (Spec.Type == TEXT("variableGet"))
        {
            UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(Graph);
            Get->VariableReference.SetSelfMember(*Spec.VariableName, Spec.VariableGuid); Node = Get;
        }
        else if (Spec.Type == TEXT("branch")) Node = NewObject<UK2Node_IfThenElse>(Graph);
        else if (Spec.Type == TEXT("sequence")) Node = NewObject<UK2Node_ExecutionSequence>(Graph);
        if (Node && Detached) Node->AllocateDefaultPins();
        return Node;
    }

    UEdGraphPin* NamedPin(UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Direction, FString& Error)
    {
        UEdGraphPin* Result = nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == Direction && Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                if (Result) { Error = FString::Printf(TEXT("Pin '%s' is ambiguous."), *Name); return nullptr; }
                Result = Pin;
            }
        }
        if (!Result) Error = FString::Printf(TEXT("Pin '%s' was not found on node '%s'."), *Name, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        return Result;
    }

    bool ApplyInputDefaults(FPlanNode& Spec, const UEdGraphSchema_K2* Schema, UEdGraphNode* Node, bool bExisting, FString& Error)
    {
        for (FPlanPinDefault& Default : Spec.InputDefaults)
        {
            UEdGraphPin* Pin = NamedPin(Node, Default.PinName, EGPD_Input, Error);
            if (!Pin) return false;
            if (!Pin->LinkedTo.IsEmpty())
            { Error = FString::Printf(TEXT("Cannot set a default on connected pin '%s'."), *Default.PinName); return false; }
            if (!Default.DefaultObjectPath.IsEmpty())
            {
                Default.ResolvedObject = LoadObject<UObject>(nullptr, *Default.DefaultObjectPath);
                if (!Default.ResolvedObject)
                { Error = FString::Printf(TEXT("Could not load default object/class '%s'."), *Default.DefaultObjectPath); return false; }
                const FString ValidationError = Schema->IsPinDefaultValid(Pin, FString(), Default.ResolvedObject, FText::GetEmpty());
                if (!ValidationError.IsEmpty())
                { Error = FString::Printf(TEXT("Default for %s.%s is invalid: %s"), *Spec.Id, *Default.PinName, *ValidationError); return false; }
                if (bExisting)
                {
                    if (Pin->DefaultObject != Default.ResolvedObject)
                    { Error = FString::Printf(TEXT("Existing workflow node '%s' has a conflicting object default on '%s'."), *Spec.Id, *Default.PinName); return false; }
                }
                else
                {
                    Schema->TrySetDefaultObject(*Pin, Default.ResolvedObject);
                    if (Pin->DefaultObject != Default.ResolvedObject)
                    { Error = FString::Printf(TEXT("Unreal rejected object default for %s.%s."), *Spec.Id, *Default.PinName); return false; }
                    Default.bApplied = true;
                }
            }
            else
            {
                const FString ValidationError = Schema->IsPinDefaultValid(Pin, Default.DefaultValue, nullptr, FText::GetEmpty());
                if (!ValidationError.IsEmpty())
                { Error = FString::Printf(TEXT("Default for %s.%s is invalid: %s"), *Spec.Id, *Default.PinName, *ValidationError); return false; }
                if (bExisting)
                {
                    if (Pin->DefaultValue != Default.DefaultValue)
                    { Error = FString::Printf(TEXT("Existing workflow node '%s' has a conflicting literal default on '%s'."), *Spec.Id, *Default.PinName); return false; }
                }
                else
                {
                    Schema->TrySetDefaultValue(*Pin, Default.DefaultValue);
                    if (Pin->DefaultValue != Default.DefaultValue)
                    { Error = FString::Printf(TEXT("Unreal rejected or normalized default for %s.%s to '%s'."), *Spec.Id, *Default.PinName, *Pin->DefaultValue); return false; }
                    Default.bApplied = true;
                }
            }
        }
        return true;
    }
}

FApplyBlueprintInteractionPlanTool::FApplyBlueprintInteractionPlanTool()
    : FMCPToolBase(TEXT("ApplyBlueprintInteractionPlan"),
        TEXT("Preflights and idempotently applies Blueprint interaction nodes, exact named-pin connections, and safe Sequence splices that preserve an existing execution route.")) {}

UnrealMCP::FMCPResponse FApplyBlueprintInteractionPlanTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, GraphName, GraphGuid, WorkflowId;
    const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* ConnectionValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* InsertionValues = nullptr;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("workflowId"), WorkflowId)
        || !Request.Params->TryGetArrayField(TEXT("nodes"), NodeValues) || !NodeValues || NodeValues->IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("ApplyBlueprintInteractionPlan requires objectPath, graph selector, workflowId, and nodes."));
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetArrayField(TEXT("connections"), ConnectionValues);
    Request.Params->TryGetArrayField(TEXT("executionInsertions"), InsertionValues);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty()) return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("graphName or graphGuid is required."));

    TArray<FPlanNode> Nodes; TSet<FString> Ids;
    for (int32 Index = 0; Index < NodeValues->Num(); ++Index)
    {
        FPlanNode Node; FString Error;
        if (!ParseNode((*NodeValues)[Index]->AsObject(), Node, Error) || Ids.Contains(Node.Id))
            return BuildError(Request, EMCPErrorCode::InvalidParams, FString::Printf(TEXT("Invalid node %d: %s"), Index, Ids.Contains(Node.Id) ? TEXT("duplicate id") : *Error));
        Ids.Add(Node.Id); Nodes.Add(MoveTemp(Node));
    }
    TArray<FPlanConnection> Connections;
    if (ConnectionValues) for (int32 Index = 0; Index < ConnectionValues->Num(); ++Index)
    {
        FPlanConnection Connection; FString Error;
        if (!ParseConnection((*ConnectionValues)[Index]->AsObject(), Connection, Error)
            || !Ids.Contains(Connection.SourceId) || !Ids.Contains(Connection.TargetId))
            return BuildError(Request, EMCPErrorCode::InvalidParams, FString::Printf(TEXT("Invalid connection %d: %s"), Index, Error.IsEmpty() ? TEXT("unknown node id") : *Error));
        Connections.Add(MoveTemp(Connection));
    }
    TArray<FPlanExecutionInsertion> Insertions;
    if (InsertionValues) for (int32 Index = 0; Index < InsertionValues->Num(); ++Index)
    {
        FPlanExecutionInsertion Insertion; FString Error;
        if (!ParseExecutionInsertion((*InsertionValues)[Index]->AsObject(), Insertion, Error)
            || !Ids.Contains(Insertion.SourceId) || !Ids.Contains(Insertion.InsertedId) || !Ids.Contains(Insertion.TargetId))
            return BuildError(Request, EMCPErrorCode::InvalidParams, FString::Printf(TEXT("Invalid execution insertion %d: %s"), Index, Error.IsEmpty() ? TEXT("unknown node id") : *Error));
        Insertions.Add(MoveTemp(Insertion));
    }

    const bool DryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool Compile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), true);
    const bool Save = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), true);
    bool CompileSucceeded = false, IndexRefreshed = false; int32 CompileErrors = 0, CompileWarnings = 0;
    FString SavedFilename, IndexError, ExecutionError;
    const bool Succeeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& Error)
    {
        UBlueprint* Blueprint = nullptr; UEdGraph* Graph = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, Error)
            || !BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, Error)) return false;
        const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
        if (!Schema) { Error = TEXT("Selected graph is not K2."); return false; }
        TMap<FString, FPlanNode*> ById; int32 AutoIndex = 0;
        for (FPlanNode& Spec : Nodes)
        {
            ById.Add(Spec.Id, &Spec);
            if (Spec.Type == TEXT("functionCall"))
            {
                UClass* Owner = nullptr;
                if (!BlueprintEditToolUtils::ResolveClass(Spec.OwnerClassPath, Owner, Error)) return false;
                Spec.Function = Owner->FindFunctionByName(*Spec.FunctionName, EIncludeSuperFlag::IncludeSuper);
                if (!Spec.Function || !Spec.Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
                { Error = FString::Printf(TEXT("Function '%s' is not Blueprint-callable."), *Spec.FunctionName); return false; }
            }
            else if (Spec.Type == TEXT("variableGet"))
            {
                Spec.VariableGuid = FBlueprintEditorUtils::FindMemberVariableGuidByName(Blueprint, *Spec.VariableName);
                if (!Spec.VariableGuid.IsValid() && (!Blueprint->GeneratedClass || !Blueprint->GeneratedClass->FindPropertyByName(*Spec.VariableName)))
                { Error = FString::Printf(TEXT("Member '%s' was not found."), *Spec.VariableName); return false; }
            }
            Spec.Existing = FindExisting(Graph, WorkflowId, Spec);
            if (Spec.Type == TEXT("existingNode") && !Spec.Existing)
            { Error = FString::Printf(TEXT("Existing node '%s' was not found in the selected graph."), *Spec.NodeGuid); return false; }
            if (!ExistingMatches(Spec)) { Error = FString::Printf(TEXT("Existing workflow node '%s' does not match the plan."), *Spec.Id); return false; }
            if (!Spec.bHasPosition) { Spec.X = AutoIndex * 380; Spec.Y = (AutoIndex % 2) * 180; } ++AutoIndex;
            Spec.Preview = Spec.Existing ? Spec.Existing : NewPlanNode(Graph, Spec, true);
            if (!Spec.Preview) { Error = FString::Printf(TEXT("Could not preview node '%s'."), *Spec.Id); return false; }
            if (!ApplyInputDefaults(Spec, Schema, Spec.Preview, Spec.Existing != nullptr, Error)) return false;
        }
        for (FPlanNode& Spec : Nodes)
        {
            if (Spec.Type != TEXT("sequence")) continue;
            FPlanNode* Source = ById.FindRef(Spec.SpliceAfterNodeId);
            if (!Source || Source == &Spec)
            { Error = FString::Printf(TEXT("Sequence '%s' has an invalid spliceAfterNodeId."), *Spec.Id); return false; }
            UEdGraphPin* SourceOut = NamedPin(Source->Preview, Spec.SpliceAfterPinName, EGPD_Output, Error);
            UEdGraphPin* SequenceIn = NamedPin(Spec.Preview, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input, Error);
            UEdGraphPin* PreservedOut = NamedPin(Spec.Preview, TEXT("Then_0"), EGPD_Output, Error);
            if (!SourceOut || !SequenceIn || !PreservedOut) return false;
            if (SourceOut->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            { Error = TEXT("Sequence splices require an execution output pin."); return false; }
            if (Spec.Existing)
            {
                Spec.bSpliceAlreadyComplete = SourceOut->LinkedTo.Contains(SequenceIn) && !PreservedOut->LinkedTo.IsEmpty();
                if (!Spec.bSpliceAlreadyComplete)
                { Error = FString::Printf(TEXT("Existing Sequence '%s' is not the expected completed splice."), *Spec.Id); return false; }
            }
            else if (SourceOut->LinkedTo.Num() != 1)
            {
                Error = FString::Printf(TEXT("Sequence '%s' can preserve exactly one existing execution route; found %d."), *Spec.Id, SourceOut->LinkedTo.Num());
                return false;
            }
        }
        for (FPlanConnection& Connection : Connections)
        {
            FPlanNode* Source = ById.FindRef(Connection.SourceId); FPlanNode* Target = ById.FindRef(Connection.TargetId);
            UEdGraphPin* Out = NamedPin(Source->Preview, Connection.SourcePin, EGPD_Output, Error); if (!Out) return false;
            UEdGraphPin* In = NamedPin(Target->Preview, Connection.TargetPin, EGPD_Input, Error); if (!In) return false;
            Connection.bAlreadyConnected = Out->LinkedTo.Contains(In);
            if (!Connection.bAlreadyConnected && !In->LinkedTo.IsEmpty()) { Error = TEXT("A target pin already has different wiring."); return false; }
            if (!Connection.bAlreadyConnected && Out->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && !Out->LinkedTo.IsEmpty())
            { Error = TEXT("An execution output already has a different route."); return false; }
            if (!Connection.bAlreadyConnected && Schema->CanCreateConnection(Out, In).Response == CONNECT_RESPONSE_DISALLOW)
            { Error = FString::Printf(TEXT("Pins %s.%s and %s.%s are incompatible."), *Connection.SourceId, *Connection.SourcePin, *Connection.TargetId, *Connection.TargetPin); return false; }
        }
        for (FPlanExecutionInsertion& Insertion : Insertions)
        {
            UEdGraphPin* Source = NamedPin(ById.FindRef(Insertion.SourceId)->Preview, Insertion.SourcePin, EGPD_Output, Error);
            UEdGraphPin* InsertedIn = NamedPin(ById.FindRef(Insertion.InsertedId)->Preview, Insertion.InsertedInputPin, EGPD_Input, Error);
            UEdGraphPin* InsertedOut = NamedPin(ById.FindRef(Insertion.InsertedId)->Preview, Insertion.InsertedOutputPin, EGPD_Output, Error);
            UEdGraphPin* Target = NamedPin(ById.FindRef(Insertion.TargetId)->Preview, Insertion.TargetPin, EGPD_Input, Error);
            if (!Source || !InsertedIn || !InsertedOut || !Target) return false;
            if (Source->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec || InsertedIn->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                || InsertedOut->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec || Target->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            { Error = TEXT("Execution insertions require exec pins."); return false; }
            Insertion.bAlreadyComplete = Source->LinkedTo.Contains(InsertedIn) && InsertedOut->LinkedTo.Contains(Target);
            if (!Insertion.bAlreadyComplete && (Source->LinkedTo.Num() != 1 || !Source->LinkedTo.Contains(Target)
                || !InsertedIn->LinkedTo.IsEmpty() || !InsertedOut->LinkedTo.IsEmpty()))
            { Error = TEXT("Execution insertion requires one exact source-to-target route and an unwired inserted node."); return false; }
        }
        if (DryRun) return true;

        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "ApplyInteractionPlan", "UnrealMCP Apply Blueprint Interaction Plan"));
        Blueprint->Modify(); Graph->Modify();
        for (FPlanNode& Spec : Nodes)
        {
            if (Spec.Existing) { Spec.Applied = Spec.Existing; continue; }
            Spec.Applied = NewPlanNode(Graph, Spec, false);
            BlueprintGraphEditToolUtils::FPlacement Placement; Placement.X = Spec.X; Placement.Y = Spec.Y;
            BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Spec.Applied, Placement);
            Spec.Applied->NodeComment = Marker(WorkflowId, Spec.Id); Spec.bCreated = true;
            if (!ApplyInputDefaults(Spec, Schema, Spec.Applied, false, Error)) return false;
        }
        for (FPlanNode& Spec : Nodes)
        {
            if (Spec.Type != TEXT("sequence") || Spec.bSpliceAlreadyComplete) continue;
            FPlanNode* Source = ById.FindRef(Spec.SpliceAfterNodeId);
            UEdGraphPin* SourceOut = NamedPin(Source->Applied, Spec.SpliceAfterPinName, EGPD_Output, Error);
            UEdGraphPin* SequenceIn = NamedPin(Spec.Applied, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input, Error);
            UEdGraphPin* PreservedOut = NamedPin(Spec.Applied, TEXT("Then_0"), EGPD_Output, Error);
            if (!SourceOut || !SequenceIn || !PreservedOut || SourceOut->LinkedTo.Num() != 1) return false;
            UEdGraphPin* PreviousTarget = SourceOut->LinkedTo[0];
            SourceOut->BreakLinkTo(PreviousTarget);
            if (!Schema->TryCreateConnection(SourceOut, SequenceIn) || !Schema->TryCreateConnection(PreservedOut, PreviousTarget))
            { Error = TEXT("Unreal rejected the preflighted Sequence splice."); return false; }
            Spec.bSpliceApplied = true;
        }
        for (FPlanExecutionInsertion& Insertion : Insertions)
        {
            if (Insertion.bAlreadyComplete) continue;
            UEdGraphPin* Source = NamedPin(ById.FindRef(Insertion.SourceId)->Applied, Insertion.SourcePin, EGPD_Output, Error);
            UEdGraphPin* InsertedIn = NamedPin(ById.FindRef(Insertion.InsertedId)->Applied, Insertion.InsertedInputPin, EGPD_Input, Error);
            UEdGraphPin* InsertedOut = NamedPin(ById.FindRef(Insertion.InsertedId)->Applied, Insertion.InsertedOutputPin, EGPD_Output, Error);
            UEdGraphPin* Target = NamedPin(ById.FindRef(Insertion.TargetId)->Applied, Insertion.TargetPin, EGPD_Input, Error);
            if (!Source || !InsertedIn || !InsertedOut || !Target || !Source->LinkedTo.Contains(Target)) return false;
            Source->BreakLinkTo(Target);
            if (!Schema->TryCreateConnection(Source, InsertedIn) || !Schema->TryCreateConnection(InsertedOut, Target))
            { Error = TEXT("Unreal rejected the preflighted execution insertion."); return false; }
            Insertion.bApplied = true;
        }
        for (FPlanConnection& Connection : Connections)
        {
            UEdGraphPin* Out = NamedPin(ById.FindRef(Connection.SourceId)->Applied, Connection.SourcePin, EGPD_Output, Error);
            UEdGraphPin* In = NamedPin(ById.FindRef(Connection.TargetId)->Applied, Connection.TargetPin, EGPD_Input, Error);
            if (!Out || !In) return false;
            if (!Out->LinkedTo.Contains(In)) { if (!Schema->TryCreateConnection(Out, In)) { Error = TEXT("Unreal rejected a preflighted connection."); return false; } Connection.bConnected = true; }
        }
        const bool Changed = Nodes.ContainsByPredicate([](const FPlanNode& N){ return N.bCreated || N.bSpliceApplied; })
            || Connections.ContainsByPredicate([](const FPlanConnection& C){ return C.bConnected; })
            || Insertions.ContainsByPredicate([](const FPlanExecutionInsertion& I){ return I.bApplied; });
        if (Changed) FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        if (Compile && Changed)
        {
            FCompilerResultsLog Log; Log.bSilentMode = true;
            FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Log);
            CompileErrors = Log.NumErrors; CompileWarnings = Log.NumWarnings;
            CompileSucceeded = CompileErrors == 0 && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
            if (!CompileSucceeded) { Error = TEXT("Plan applied, but Blueprint validation failed. Use ValidateBlueprint for diagnostics."); return false; }
        }
        if (Save && Changed && !BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, Error)) return false;
        if (Save && Changed) IndexRefreshed = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexError);
        return true;
    }, ExecutionError);
    if (!Succeeded) return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError);

    int32 CreatedCount = 0, ConnectedCount = 0, AppliedInsertionCount = 0;
    for (const FPlanNode& Node : Nodes) CreatedCount += Node.bCreated ? 1 : 0;
    for (const FPlanConnection& Connection : Connections) ConnectedCount += Connection.bConnected ? 1 : 0;
    for (const FPlanExecutionInsertion& Insertion : Insertions) AppliedInsertionCount += Insertion.bApplied ? 1 : 0;
    const bool Changed = CreatedCount > 0 || ConnectedCount > 0 || AppliedInsertionCount > 0;
    TArray<TSharedPtr<FJsonValue>> NodeResults;
    for (const FPlanNode& Node : Nodes)
    {
        const UEdGraphNode* Live = Node.Applied ? Node.Applied : Node.Existing;
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("id"), Node.Id); Item->SetStringField(TEXT("type"), Node.Type);
        Item->SetBoolField(TEXT("created"), Node.bCreated); Item->SetBoolField(TEXT("alreadyExisted"), Node.Existing != nullptr);
        Item->SetBoolField(TEXT("spliceApplied"), Node.bSpliceApplied); Item->SetBoolField(TEXT("spliceAlreadyComplete"), Node.bSpliceAlreadyComplete);
        int32 AppliedDefaultCount = 0; for (const FPlanPinDefault& Default : Node.InputDefaults) AppliedDefaultCount += Default.bApplied ? 1 : 0;
        Item->SetNumberField(TEXT("appliedInputDefaultCount"), AppliedDefaultCount);
        Item->SetStringField(TEXT("nodeGuid"), BlueprintGraphEditToolUtils::GetNodeGuid(Live)); NodeResults.Add(MakeShared<FJsonValueObject>(Item));
    }
    TArray<TSharedPtr<FJsonValue>> ConnectionResults;
    for (const FPlanConnection& Connection : Connections)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("sourceNodeId"), Connection.SourceId);
        Item->SetStringField(TEXT("sourcePinName"), Connection.SourcePin); Item->SetStringField(TEXT("targetNodeId"), Connection.TargetId);
        Item->SetStringField(TEXT("targetPinName"), Connection.TargetPin); Item->SetBoolField(TEXT("alreadyConnected"), Connection.bAlreadyConnected);
        Item->SetBoolField(TEXT("connected"), Connection.bConnected); ConnectionResults.Add(MakeShared<FJsonValueObject>(Item));
    }
    FMCPResponse Response; Response.Id = Request.Id; TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath); Result->SetStringField(TEXT("workflowId"), WorkflowId);
    Result->SetBoolField(TEXT("dryRun"), DryRun); Result->SetBoolField(TEXT("changed"), Changed); Result->SetBoolField(TEXT("alreadyComplete"), !DryRun && !Changed);
    Result->SetNumberField(TEXT("createdNodeCount"), CreatedCount); Result->SetNumberField(TEXT("connectedPinCount"), ConnectedCount);
    Result->SetNumberField(TEXT("appliedExecutionInsertionCount"), AppliedInsertionCount);
    Result->SetArrayField(TEXT("nodes"), NodeResults); Result->SetArrayField(TEXT("connections"), ConnectionResults);
    TArray<TSharedPtr<FJsonValue>> InsertionResults;
    for (const FPlanExecutionInsertion& Insertion : Insertions)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("sourceNodeId"), Insertion.SourceId);
        Item->SetStringField(TEXT("insertedNodeId"), Insertion.InsertedId); Item->SetStringField(TEXT("targetNodeId"), Insertion.TargetId);
        Item->SetBoolField(TEXT("alreadyComplete"), Insertion.bAlreadyComplete); Item->SetBoolField(TEXT("applied"), Insertion.bApplied);
        InsertionResults.Add(MakeShared<FJsonValueObject>(Item));
    }
    Result->SetArrayField(TEXT("executionInsertions"), InsertionResults);
    Result->SetBoolField(TEXT("compiled"), Compile && Changed && !DryRun); Result->SetBoolField(TEXT("compileSucceeded"), CompileSucceeded);
    Result->SetNumberField(TEXT("compileErrorCount"), CompileErrors); Result->SetNumberField(TEXT("compileWarningCount"), CompileWarnings);
    Result->SetBoolField(TEXT("saved"), Save && Changed && !DryRun); Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), IndexRefreshed); Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FApplyBlueprintInteractionPlanTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("workflowId")}) Properties->SetObjectField(FieldName, BuildStringProperty(TEXT("Blueprint, graph, or stable workflow selector.")));
    TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>(); Nodes->SetStringField(TEXT("type"), TEXT("array"));
    Nodes->SetStringField(TEXT("description"), TEXT("Nodes: existingNode, customEvent, functionCall, variableGet, branch, or sequence. A sequence safely preserves one existing exec route via spliceAfterNodeId/spliceAfterPinName."));
    TSharedRef<FJsonObject> NodeItem = MakeShared<FJsonObject>(); NodeItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> NodeProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("id"), TEXT("type"), TEXT("nodeGuid"), TEXT("eventName"), TEXT("ownerClassPath"), TEXT("functionName"), TEXT("variableName"), TEXT("spliceAfterNodeId"), TEXT("spliceAfterPinName")})
        NodeProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Stable node identity, kind, or type-specific reflected member selector.")));
    TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>(); Position->SetStringField(TEXT("type"), TEXT("integer"));
    NodeProperties->SetObjectField(TEXT("positionX"), Position); NodeProperties->SetObjectField(TEXT("positionY"), Position);
    NodeItem->SetObjectField(TEXT("properties"), NodeProperties);
    TSharedRef<FJsonObject> InputDefaults = MakeShared<FJsonObject>(); InputDefaults->SetStringField(TEXT("type"), TEXT("array"));
    InputDefaults->SetStringField(TEXT("description"), TEXT("Optional exact input-pin defaults applied before connection validation. Each entry uses defaultValue or defaultObjectPath."));
    TSharedRef<FJsonObject> DefaultItem = MakeShared<FJsonObject>(); DefaultItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> DefaultProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("pinName"), TEXT("defaultValue"), TEXT("defaultObjectPath")})
        DefaultProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Exact input pin and literal or UObject/UClass default.")));
    DefaultItem->SetObjectField(TEXT("properties"), DefaultProperties);
    DefaultItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("pinName"))});
    InputDefaults->SetObjectField(TEXT("items"), DefaultItem); NodeProperties->SetObjectField(TEXT("inputDefaults"), InputDefaults);
    NodeItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("id")), MakeShared<FJsonValueString>(TEXT("type"))});
    Nodes->SetObjectField(TEXT("items"), NodeItem); Properties->SetObjectField(TEXT("nodes"), Nodes);
    TSharedRef<FJsonObject> Connections = MakeShared<FJsonObject>(); Connections->SetStringField(TEXT("type"), TEXT("array"));
    Connections->SetStringField(TEXT("description"), TEXT("Exact named output-to-input pin connections."));
    TSharedRef<FJsonObject> ConnectionItem = MakeShared<FJsonObject>(); ConnectionItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> ConnectionProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("sourceNodeId"), TEXT("sourcePinName"), TEXT("targetNodeId"), TEXT("targetPinName")})
        ConnectionProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Plan node id or exact live pin name.")));
    ConnectionItem->SetObjectField(TEXT("properties"), ConnectionProperties);
    ConnectionItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("sourceNodeId")), MakeShared<FJsonValueString>(TEXT("sourcePinName")), MakeShared<FJsonValueString>(TEXT("targetNodeId")), MakeShared<FJsonValueString>(TEXT("targetPinName"))});
    Connections->SetObjectField(TEXT("items"), ConnectionItem); Properties->SetObjectField(TEXT("connections"), Connections);
    TSharedRef<FJsonObject> Insertions = MakeShared<FJsonObject>(); Insertions->SetStringField(TEXT("type"), TEXT("array"));
    Insertions->SetStringField(TEXT("description"), TEXT("Atomic idempotent insertion of one impure node into an exact existing exec link."));
    TSharedRef<FJsonObject> InsertionItem = MakeShared<FJsonObject>(); InsertionItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> InsertionProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("sourceNodeId"), TEXT("sourcePinName"), TEXT("insertedNodeId"), TEXT("insertedInputPinName"), TEXT("insertedOutputPinName"), TEXT("targetNodeId"), TEXT("targetPinName")})
        InsertionProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Exact plan node id and exec pin name.")));
    InsertionItem->SetObjectField(TEXT("properties"), InsertionProperties);
    InsertionItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("sourceNodeId")), MakeShared<FJsonValueString>(TEXT("sourcePinName")), MakeShared<FJsonValueString>(TEXT("insertedNodeId")), MakeShared<FJsonValueString>(TEXT("insertedInputPinName")), MakeShared<FJsonValueString>(TEXT("insertedOutputPinName")), MakeShared<FJsonValueString>(TEXT("targetNodeId")), MakeShared<FJsonValueString>(TEXT("targetPinName"))});
    Insertions->SetObjectField(TEXT("items"), InsertionItem); Properties->SetObjectField(TEXT("executionInsertions"), Insertions);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Preflight without mutation.")));
    Properties->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile once after changes. Defaults true.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh once. Defaults true.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("workflowId")), MakeShared<FJsonValueString>(TEXT("nodes"))});
    return Schema;
}
