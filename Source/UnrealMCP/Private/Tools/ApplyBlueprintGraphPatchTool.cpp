#include "Tools/ApplyBlueprintGraphPatchTool.h"

#include "MCP/MutationRequestTracker.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    struct FPatchDefault
    {
        FString PinName;
        FString LiteralValue;
        FString ObjectPath;
        UObject* Object = nullptr;
        bool bApplied = false;
    };

    struct FPatchNode
    {
        FString Id, Kind, NodeGuid, OwnerClassPath, FunctionName, VariableName, OperatorName, CanonicalOperator;
        FString Comment;
        bool bHasComment = false, bHasPosition = false, bHasTolerance = false;
        double Tolerance = 0.0;
        int32 X = 0, Y = 0;
        FGuid DeterministicGuid, VariableGuid;
        UFunction* Function = nullptr;
        UEdGraphNode* Preview = nullptr;
        UEdGraphNode* Live = nullptr;
        bool bCreated = false, bReused = false, bMoved = false, bCommentChanged = false;
        TArray<FPatchDefault> Defaults;
    };

    struct FPatchConnection
    {
        FString SourceId, SourcePin, TargetId, TargetPin;
        FString ExistingSourceId, ExistingSourcePin;
        bool bConfirmReplacement = false, bAlreadyConnected = false, bConnected = false, bReplaced = false;
        bool bSourceWildcardBefore = false, bTargetWildcardBefore = false;
        bool bSourceWildcardAfter = false, bTargetWildcardAfter = false, bWildcardResolved = false;
        UEdGraphPin* PreviewSource = nullptr;
        UEdGraphPin* PreviewTarget = nullptr;
    };

    struct FPatchDisconnection
    {
        FString SourceId, SourcePin, TargetId, TargetPin;
        bool bConfirm = false, bAlreadyDisconnected = false, bDisconnected = false;
    };

    bool ParseNode(const TSharedPtr<FJsonObject>& Object, FPatchNode& Out, FString& Error)
    {
        if (!Object.IsValid() || !Object->TryGetStringField(TEXT("id"), Out.Id)
            || !Object->TryGetStringField(TEXT("kind"), Out.Kind) || Out.Id.IsEmpty())
        { Error = TEXT("Each patch node requires non-empty id and kind fields."); return false; }
        Object->TryGetStringField(TEXT("nodeGuid"), Out.NodeGuid);
        Object->TryGetStringField(TEXT("ownerClassPath"), Out.OwnerClassPath);
        Object->TryGetStringField(TEXT("functionName"), Out.FunctionName);
        Object->TryGetStringField(TEXT("variableName"), Out.VariableName);
        Object->TryGetStringField(TEXT("operator"), Out.OperatorName);
        Out.bHasComment = Object->TryGetStringField(TEXT("comment"), Out.Comment);
        double X = 0.0, Y = 0.0;
        const bool bHasX = Object->TryGetNumberField(TEXT("positionX"), X);
        const bool bHasY = Object->TryGetNumberField(TEXT("positionY"), Y);
        if (bHasX != bHasY) { Error = TEXT("Node placement requires both positionX and positionY."); return false; }
        Out.bHasPosition = bHasX; Out.X = FMath::RoundToInt(X); Out.Y = FMath::RoundToInt(Y);
        Out.bHasTolerance = Object->TryGetNumberField(TEXT("tolerance"), Out.Tolerance);

        const TArray<TSharedPtr<FJsonValue>>* Defaults = nullptr;
        if (Object->TryGetArrayField(TEXT("inputDefaults"), Defaults) && Defaults != nullptr)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Defaults)
            {
                const TSharedPtr<FJsonObject> DefaultObject = Value.IsValid() ? Value->AsObject() : nullptr;
                FPatchDefault Default;
                if (!DefaultObject.IsValid() || !DefaultObject->TryGetStringField(TEXT("pinName"), Default.PinName)
                    || Default.PinName.IsEmpty())
                { Error = TEXT("Each input default requires pinName."); return false; }
                const bool bLiteral = DefaultObject->TryGetStringField(TEXT("defaultValue"), Default.LiteralValue);
                const bool bObject = DefaultObject->TryGetStringField(TEXT("defaultObjectPath"), Default.ObjectPath);
                if (bLiteral == bObject) { Error = TEXT("Each input default requires exactly one value source."); return false; }
                if (Out.Defaults.ContainsByPredicate([&](const FPatchDefault& Existing)
                    { return Existing.PinName.Equals(Default.PinName, ESearchCase::IgnoreCase); }))
                { Error = FString::Printf(TEXT("Duplicate default for pin '%s'."), *Default.PinName); return false; }
                Out.Defaults.Add(MoveTemp(Default));
            }
        }

        if (Out.Kind.Equals(TEXT("existingNode"), ESearchCase::IgnoreCase))
        {
            FGuid Guid;
            if (!FGuid::Parse(Out.NodeGuid, Guid)) { Error = TEXT("existingNode requires a valid nodeGuid."); return false; }
        }
        else if (Out.Kind.Equals(TEXT("functionCall"), ESearchCase::IgnoreCase))
        {
            if (Out.OwnerClassPath.IsEmpty() || Out.FunctionName.IsEmpty())
            { Error = TEXT("functionCall requires ownerClassPath and functionName."); return false; }
        }
        else if (Out.Kind.Equals(TEXT("variableGet"), ESearchCase::IgnoreCase)
            || Out.Kind.Equals(TEXT("variableSet"), ESearchCase::IgnoreCase))
        {
            if (Out.VariableName.IsEmpty()) { Error = TEXT("Variable nodes require variableName."); return false; }
        }
        else if (Out.Kind.Equals(TEXT("typedOperator"), ESearchCase::IgnoreCase))
        {
            if (Out.OperatorName.IsEmpty()) { Error = TEXT("typedOperator requires operator."); return false; }
        }
        else if (!Out.Kind.Equals(TEXT("branch"), ESearchCase::IgnoreCase)
            && !Out.Kind.Equals(TEXT("reroute"), ESearchCase::IgnoreCase))
        { Error = FString::Printf(TEXT("Unsupported patch node kind '%s'."), *Out.Kind); return false; }
        return true;
    }

    bool ParseConnection(const TSharedPtr<FJsonObject>& Object, FPatchConnection& Out, FString& Error)
    {
        if (!Object.IsValid()
            || !Object->TryGetStringField(TEXT("sourceNodeId"), Out.SourceId)
            || !Object->TryGetStringField(TEXT("sourcePinName"), Out.SourcePin)
            || !Object->TryGetStringField(TEXT("targetNodeId"), Out.TargetId)
            || !Object->TryGetStringField(TEXT("targetPinName"), Out.TargetPin))
        { Error = TEXT("Connections require exact source and target node/pin fields."); return false; }
        Object->TryGetStringField(TEXT("existingSourceNodeId"), Out.ExistingSourceId);
        Object->TryGetStringField(TEXT("existingSourcePinName"), Out.ExistingSourcePin);
        Out.bConfirmReplacement = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(
            Object, TEXT("confirmDataReplacement"), false);
        if (Out.ExistingSourceId.IsEmpty() != Out.ExistingSourcePin.IsEmpty())
        { Error = TEXT("Data replacement requires both existingSourceNodeId and existingSourcePinName."); return false; }
        return true;
    }

    bool ParseDisconnection(const TSharedPtr<FJsonObject>& Object, FPatchDisconnection& Out, FString& Error)
    {
        if (!Object.IsValid()
            || !Object->TryGetStringField(TEXT("sourceNodeId"), Out.SourceId)
            || !Object->TryGetStringField(TEXT("sourcePinName"), Out.SourcePin)
            || !Object->TryGetStringField(TEXT("targetNodeId"), Out.TargetId)
            || !Object->TryGetStringField(TEXT("targetPinName"), Out.TargetPin))
        { Error = TEXT("Disconnections require exact source and target node/pin fields."); return false; }
        Out.bConfirm = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Object, TEXT("confirm"), false);
        return true;
    }

    FGuid MakeDeterministicGuid(const FString& ObjectPath, const FGuid& GraphGuid, const FString& PatchId, const FString& NodeId)
    {
        const FString Hex = FMD5::HashAnsiString(*(ObjectPath + TEXT("|")
            + GraphGuid.ToString(EGuidFormats::Digits) + TEXT("|") + PatchId + TEXT("|") + NodeId));
        FGuid Result;
        FGuid::ParseExact(Hex, EGuidFormats::Digits, Result);
        return Result;
    }

    UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FGuid& Guid)
    {
        for (UEdGraphNode* Node : Graph->Nodes) if (Node != nullptr && Node->NodeGuid == Guid) return Node;
        return nullptr;
    }

    UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Direction, FString& Error)
    {
        UEdGraphPin* Found = nullptr;
        if (Node == nullptr) { Error = TEXT("Cannot resolve a pin on a null node."); return nullptr; }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin != nullptr && Pin->Direction == Direction
                && Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                if (Found != nullptr) { Error = FString::Printf(TEXT("Pin '%s' is ambiguous."), *Name); return nullptr; }
                Found = Pin;
            }
        }
        if (Found == nullptr) Error = FString::Printf(TEXT("Pin '%s' was not found."), *Name);
        return Found;
    }

    bool IsWildcardPin(const UEdGraphPin* Pin)
    {
        return Pin != nullptr
            && (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
                || (Pin->PinType.ContainerType == EPinContainerType::Map
                    && Pin->PinType.PinValueType.TerminalCategory == UEdGraphSchema_K2::PC_Wildcard));
    }

    bool NodeMatches(const FPatchNode& Spec, UEdGraphNode* Node)
    {
        if (Spec.Kind.Equals(TEXT("existingNode"), ESearchCase::IgnoreCase)) return Node != nullptr;
        if (Spec.Kind.Equals(TEXT("functionCall"), ESearchCase::IgnoreCase)
            || Spec.Kind.Equals(TEXT("typedOperator"), ESearchCase::IgnoreCase))
            return Cast<UK2Node_CallFunction>(Node) != nullptr
                && CastChecked<UK2Node_CallFunction>(Node)->GetTargetFunction() == Spec.Function;
        if (Spec.Kind.Equals(TEXT("variableGet"), ESearchCase::IgnoreCase))
            return Cast<UK2Node_VariableGet>(Node) != nullptr
                && CastChecked<UK2Node_VariableGet>(Node)->VariableReference.GetMemberGuid() == Spec.VariableGuid;
        if (Spec.Kind.Equals(TEXT("variableSet"), ESearchCase::IgnoreCase))
            return Cast<UK2Node_VariableSet>(Node) != nullptr
                && CastChecked<UK2Node_VariableSet>(Node)->VariableReference.GetMemberGuid() == Spec.VariableGuid;
        if (Spec.Kind.Equals(TEXT("branch"), ESearchCase::IgnoreCase)) return Node != nullptr && Node->IsA<UK2Node_IfThenElse>();
        return Node != nullptr && Node->IsA<UK2Node_Knot>();
    }

    UEdGraphNode* CreateNode(UEdGraph* Graph, const FPatchNode& Spec, bool bDetached)
    {
        using namespace UnrealMCP::BlueprintGraphEditToolUtils;
        if (Spec.Kind.Equals(TEXT("functionCall"), ESearchCase::IgnoreCase)
            || Spec.Kind.Equals(TEXT("typedOperator"), ESearchCase::IgnoreCase))
            return CreateFunctionCallNode(Graph, Spec.Function, bDetached);
        if (Spec.Kind.Equals(TEXT("variableGet"), ESearchCase::IgnoreCase))
            return CreateVariableGetNode(Graph, *Spec.VariableName, Spec.VariableGuid, bDetached);
        if (Spec.Kind.Equals(TEXT("variableSet"), ESearchCase::IgnoreCase))
            return CreateVariableSetNode(Graph, *Spec.VariableName, Spec.VariableGuid, bDetached);
        if (Spec.Kind.Equals(TEXT("branch"), ESearchCase::IgnoreCase)) return CreateBranchNode(Graph, bDetached);
        if (Spec.Kind.Equals(TEXT("reroute"), ESearchCase::IgnoreCase)) return CreateRerouteNode(Graph, bDetached);
        return nullptr;
    }

    bool ApplyDefaults(FPatchNode& Spec, UEdGraphNode* Node, const UEdGraphSchema_K2* Schema, bool bExisting, FString& Error)
    {
        if (Spec.bHasTolerance)
        {
            if (!Spec.Defaults.ContainsByPredicate([](const FPatchDefault& Default)
                { return Default.PinName.Equals(TEXT("ErrorTolerance"), ESearchCase::IgnoreCase); }))
            {
                FPatchDefault Tolerance;
                Tolerance.PinName = TEXT("ErrorTolerance");
                Tolerance.LiteralValue = FString::SanitizeFloat(Spec.Tolerance);
                Spec.Defaults.Add(Tolerance);
            }
        }
        for (FPatchDefault& Default : Spec.Defaults)
        {
            UEdGraphPin* Pin = FindPin(Node, Default.PinName, EGPD_Input, Error);
            if (Pin == nullptr) return false;
            if (!Pin->LinkedTo.IsEmpty()) { Error = FString::Printf(TEXT("Pin '%s.%s' is already connected."), *Spec.Id, *Default.PinName); return false; }
            if (!Default.ObjectPath.IsEmpty())
            {
                Default.Object = LoadObject<UObject>(nullptr, *Default.ObjectPath);
                if (Default.Object == nullptr) { Error = FString::Printf(TEXT("Could not load '%s'."), *Default.ObjectPath); return false; }
                const FString Validation = Schema->IsPinDefaultValid(Pin, FString(), Default.Object, FText::GetEmpty());
                if (!Validation.IsEmpty()) { Error = Validation; return false; }
                if (bExisting)
                {
                    if (Pin->DefaultObject != Default.Object) { Error = FString::Printf(TEXT("Existing default conflicts on '%s.%s'."), *Spec.Id, *Default.PinName); return false; }
                }
                else
                {
                    Schema->TrySetDefaultObject(*Pin, Default.Object);
                    if (Pin->DefaultObject != Default.Object) { Error = TEXT("Unreal rejected an object default."); return false; }
                    Default.bApplied = true;
                }
            }
            else
            {
                const FString Validation = Schema->IsPinDefaultValid(Pin, Default.LiteralValue, nullptr, FText::GetEmpty());
                if (!Validation.IsEmpty()) { Error = Validation; return false; }
                if (bExisting)
                {
                    if (Pin->DefaultValue != Default.LiteralValue) { Error = FString::Printf(TEXT("Existing default conflicts on '%s.%s'."), *Spec.Id, *Default.PinName); return false; }
                }
                else
                {
                    Schema->TrySetDefaultValue(*Pin, Default.LiteralValue);
                    if (Pin->DefaultValue != Default.LiteralValue) { Error = TEXT("Unreal rejected or normalized a literal default."); return false; }
                    Default.bApplied = true;
                }
            }
        }
        return true;
    }
}

FApplyBlueprintGraphPatchTool::FApplyBlueprintGraphPatchTool()
    : FMCPToolBase(TEXT("ApplyBlueprintGraphPatch"),
        TEXT("Atomically preflights and applies a bounded declarative Blueprint graph patch with deterministic node identities, exact data-link replacement, rollback, and compile-before-save.")) {}

UnrealMCP::FMCPResponse FApplyBlueprintGraphPatchTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, GraphName, GraphGuid, PatchId, ExpectedRevision, OperationId;
    const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* ConnectionValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* DisconnectionValues = nullptr;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("patchId"), PatchId)
        || !Request.Params->TryGetArrayField(TEXT("nodes"), NodeValues)
        || NodeValues == nullptr || NodeValues->IsEmpty() || PatchId.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams,
            TEXT("ApplyBlueprintGraphPatch requires objectPath, graph selector, patchId, and non-empty nodes."));
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("expectedGraphRevision"), ExpectedRevision);
    Request.Params->TryGetStringField(TEXT("operationId"), OperationId);
    Request.Params->TryGetArrayField(TEXT("connections"), ConnectionValues);
    Request.Params->TryGetArrayField(TEXT("disconnections"), DisconnectionValues);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("graphName or graphGuid is required."));

    TArray<FPatchNode> Nodes; TSet<FString> NodeIds;
    for (int32 Index = 0; Index < NodeValues->Num(); ++Index)
    {
        FPatchNode Node; FString Error;
        if (!ParseNode((*NodeValues)[Index]->AsObject(), Node, Error) || NodeIds.Contains(Node.Id))
            return BuildError(Request, EMCPErrorCode::InvalidParams,
                FString::Printf(TEXT("Invalid node %d: %s"), Index, NodeIds.Contains(Node.Id) ? TEXT("duplicate id") : *Error));
        NodeIds.Add(Node.Id); Nodes.Add(MoveTemp(Node));
    }
    TArray<FPatchConnection> Connections;
    if (ConnectionValues != nullptr) for (int32 Index = 0; Index < ConnectionValues->Num(); ++Index)
    {
        FPatchConnection Connection; FString Error;
        if (!ParseConnection((*ConnectionValues)[Index]->AsObject(), Connection, Error)
            || !NodeIds.Contains(Connection.SourceId) || !NodeIds.Contains(Connection.TargetId)
            || (!Connection.ExistingSourceId.IsEmpty() && !NodeIds.Contains(Connection.ExistingSourceId)))
            return BuildError(Request, EMCPErrorCode::InvalidParams,
                FString::Printf(TEXT("Invalid connection %d: %s"), Index, Error.IsEmpty() ? TEXT("unknown node id") : *Error));
        Connections.Add(MoveTemp(Connection));
    }
    TArray<FPatchDisconnection> Disconnections;
    if (DisconnectionValues != nullptr) for (int32 Index = 0; Index < DisconnectionValues->Num(); ++Index)
    {
        FPatchDisconnection Disconnection; FString Error;
        if (!ParseDisconnection((*DisconnectionValues)[Index]->AsObject(), Disconnection, Error)
            || !NodeIds.Contains(Disconnection.SourceId) || !NodeIds.Contains(Disconnection.TargetId))
            return BuildError(Request, EMCPErrorCode::InvalidParams,
                FString::Printf(TEXT("Invalid disconnection %d: %s"), Index, Error.IsEmpty() ? TEXT("unknown node id") : *Error));
        Disconnections.Add(MoveTemp(Disconnection));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), true);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    FString InitialRevision, FinalRevision, SavedFilename, IndexError, ExecutionError;
    bool bChanged = false, bCompiled = false, bCompileSucceeded = false, bSaved = false, bIndexRefreshed = false, bRolledBack = false;
    int32 CompileErrors = 0, CompileWarnings = 0;

    FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Preflighting,
        TEXT("Resolving the target graph and simulating the complete patch."));

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& Error)
    {
        UBlueprint* Blueprint = nullptr; UEdGraph* Graph = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, Error)
            || !BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, Error)) return false;
        const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
        if (Schema == nullptr) { Error = TEXT("Selected graph is not a K2 graph."); return false; }
        InitialRevision = BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
        if (!ExpectedRevision.IsEmpty() && !ExpectedRevision.Equals(InitialRevision, ESearchCase::IgnoreCase))
        { Error = FString::Printf(TEXT("Graph revision mismatch. expected=%s current=%s"), *ExpectedRevision, *InitialRevision); return false; }

        TMap<FString, FPatchNode*> ById;
        int32 AutoIndex = 0, MaxX = 0;
        for (const UEdGraphNode* Node : Graph->Nodes) if (Node != nullptr) MaxX = FMath::Max(MaxX, Node->NodePosX);
        for (FPatchNode& Spec : Nodes)
        {
            ById.Add(Spec.Id, &Spec);
            if (Spec.Kind.Equals(TEXT("existingNode"), ESearchCase::IgnoreCase))
            {
                FGuid Guid; FGuid::Parse(Spec.NodeGuid, Guid); Spec.Live = FindNodeByGuid(Graph, Guid);
                if (Spec.Live == nullptr) { Error = FString::Printf(TEXT("Existing node '%s' was not found."), *Spec.Id); return false; }
            }
            else
            {
                Spec.DeterministicGuid = MakeDeterministicGuid(ObjectPath, Graph->GraphGuid, PatchId, Spec.Id);
                if (Spec.Kind.Equals(TEXT("functionCall"), ESearchCase::IgnoreCase))
                {
                    UClass* Owner = nullptr;
                    if (!BlueprintEditToolUtils::ResolveClass(Spec.OwnerClassPath, Owner, Error)) return false;
                    Spec.Function = Owner->FindFunctionByName(*Spec.FunctionName, EIncludeSuperFlag::IncludeSuper);
                    if (Spec.Function == nullptr || !Spec.Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
                    { Error = FString::Printf(TEXT("Function '%s' is not Blueprint-callable."), *Spec.FunctionName); return false; }
                }
                else if (Spec.Kind.Equals(TEXT("typedOperator"), ESearchCase::IgnoreCase))
                {
                    bool bSupportsTolerance = false;
                    if (!BlueprintGraphEditToolUtils::ResolveTypedOperatorFunction(
                        Spec.OperatorName, Spec.CanonicalOperator, Spec.Function, bSupportsTolerance, Error)) return false;
                    if (Spec.bHasTolerance && (!bSupportsTolerance || Spec.Tolerance < 0.0))
                    { Error = bSupportsTolerance ? TEXT("tolerance must be non-negative.") : TEXT("tolerance is only valid for VectorNearlyEqual."); return false; }
                }
                else if (Spec.Kind.Equals(TEXT("variableGet"), ESearchCase::IgnoreCase)
                    || Spec.Kind.Equals(TEXT("variableSet"), ESearchCase::IgnoreCase))
                {
                    const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, *Spec.VariableName);
                    if (VariableIndex == INDEX_NONE) { Error = FString::Printf(TEXT("Variable '%s' was not found."), *Spec.VariableName); return false; }
                    Spec.VariableGuid = Blueprint->NewVariables[VariableIndex].VarGuid;
                }
                Spec.Live = FindNodeByGuid(Graph, Spec.DeterministicGuid);
                if (Spec.Live != nullptr && !NodeMatches(Spec, Spec.Live))
                { Error = FString::Printf(TEXT("Deterministic node identity collision for '%s'."), *Spec.Id); return false; }
                Spec.bReused = Spec.Live != nullptr;
            }
            if (!Spec.bHasPosition)
            {
                if (Spec.Live != nullptr) { Spec.X = Spec.Live->NodePosX; Spec.Y = Spec.Live->NodePosY; }
                else { Spec.X = MaxX + 380 + AutoIndex * 380; Spec.Y = (AutoIndex % 2) * 180; }
            }
            ++AutoIndex;
        }

        // Preflight on a transient duplicate so TryCreateConnection can reproduce K2 wildcard
        // specialization and connection-order effects without touching the live graph.
        const FName SimulationName = MakeUniqueObjectName(
            Blueprint, Graph->GetClass(), TEXT("UnrealMCP_PatchPreflight"));
        UEdGraph* SimulationGraph = DuplicateObject<UEdGraph>(Graph, Blueprint, SimulationName);
        if (SimulationGraph == nullptr) { Error = TEXT("Could not create a transient graph for patch preflight."); return false; }
        SimulationGraph->SetFlags(RF_Transient);
        for (FPatchNode& Spec : Nodes)
        {
            if (Spec.Live != nullptr)
            {
                Spec.Preview = FindNodeByGuid(SimulationGraph, Spec.Live->NodeGuid);
            }
            else
            {
                UEdGraphNode* Detached = CreateNode(Graph, Spec, true);
                Spec.Preview = Detached != nullptr
                    ? DuplicateObject<UEdGraphNode>(Detached, SimulationGraph)
                    : nullptr;
                if (Spec.Preview != nullptr) Spec.Preview->NodeGuid = Spec.DeterministicGuid;
            }
            if (Spec.Preview == nullptr) { Error = FString::Printf(TEXT("Could not preview node '%s'."), *Spec.Id); return false; }
            if (!ApplyDefaults(Spec, Spec.Preview, Schema, Spec.Live != nullptr, Error)) return false;
        }

        // Apply confirmed disconnections to the simulation first, matching real mutation order.
        for (FPatchDisconnection& Disconnection : Disconnections)
        {
            UEdGraphPin* Source = FindPin(ById.FindRef(Disconnection.SourceId)->Preview, Disconnection.SourcePin, EGPD_Output, Error);
            UEdGraphPin* Target = FindPin(ById.FindRef(Disconnection.TargetId)->Preview, Disconnection.TargetPin, EGPD_Input, Error);
            if (Source == nullptr || Target == nullptr) return false;
            Disconnection.bAlreadyDisconnected = !Source->LinkedTo.Contains(Target);
            if (!Disconnection.bAlreadyDisconnected && !Disconnection.bConfirm)
            { Error = TEXT("Exact link disconnection requires confirm=true."); return false; }
            if (!Disconnection.bAlreadyDisconnected) Schema->BreakSinglePinLink(Source, Target);
        }

        for (FPatchConnection& Connection : Connections)
        {
            FPatchNode* SourceNode = ById.FindRef(Connection.SourceId); FPatchNode* TargetNode = ById.FindRef(Connection.TargetId);
            UEdGraphPin* Source = FindPin(SourceNode->Preview, Connection.SourcePin, EGPD_Output, Error);
            UEdGraphPin* Target = FindPin(TargetNode->Preview, Connection.TargetPin, EGPD_Input, Error);
            if (Source == nullptr || Target == nullptr) return false;
            Connection.PreviewSource = Source; Connection.PreviewTarget = Target;
            Connection.bSourceWildcardBefore = IsWildcardPin(Source);
            Connection.bTargetWildcardBefore = IsWildcardPin(Target);
            Connection.bAlreadyConnected = Source->LinkedTo.Contains(Target);
            if (Connection.bAlreadyConnected) continue;
            if (!Target->LinkedTo.IsEmpty())
            {
                if (Target->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                { Error = FString::Printf(TEXT("Execution target '%s.%s' already has a route."), *Connection.TargetId, *Connection.TargetPin); return false; }
                if (!Connection.bConfirmReplacement || Connection.ExistingSourceId.IsEmpty())
                { Error = TEXT("Occupied data input replacement requires exact existing source fields and confirmDataReplacement=true."); return false; }
                FPatchNode* ExistingSourceNode = ById.FindRef(Connection.ExistingSourceId);
                UEdGraphPin* ExistingSource = FindPin(ExistingSourceNode->Preview, Connection.ExistingSourcePin, EGPD_Output, Error);
                if (ExistingSource == nullptr || Target->LinkedTo.Num() != 1 || !Target->LinkedTo.Contains(ExistingSource))
                { Error = TEXT("The confirmed existing data link does not exactly match the live graph."); return false; }
                Schema->BreakSinglePinLink(ExistingSource, Target);
            }
            const FPinConnectionResponse Compatibility = Schema->CanCreateConnection(Source, Target);
            if (Compatibility.Response != CONNECT_RESPONSE_MAKE && Compatibility.Response != CONNECT_RESPONSE_BREAK_OTHERS_A
                && Compatibility.Response != CONNECT_RESPONSE_BREAK_OTHERS_B && Compatibility.Response != CONNECT_RESPONSE_BREAK_OTHERS_AB)
            { Error = FString::Printf(TEXT("Pins '%s.%s' and '%s.%s' are not directly compatible: %s"),
                *Connection.SourceId, *Connection.SourcePin, *Connection.TargetId, *Connection.TargetPin, *Compatibility.Message.ToString()); return false; }
            if (!Schema->TryCreateConnection(Source, Target))
            { Error = TEXT("Unreal rejected a connection during transient wildcard-specialization preflight."); return false; }
        }
        for (FPatchConnection& Connection : Connections)
        {
            Connection.bSourceWildcardAfter = IsWildcardPin(Connection.PreviewSource);
            Connection.bTargetWildcardAfter = IsWildcardPin(Connection.PreviewTarget);
            Connection.bWildcardResolved = (Connection.bSourceWildcardBefore && !Connection.bSourceWildcardAfter)
                || (Connection.bTargetWildcardBefore && !Connection.bTargetWildcardAfter);
        }

        for (const FPatchNode& Spec : Nodes)
        {
            if (!Spec.Kind.Equals(TEXT("functionCall"), ESearchCase::IgnoreCase) || Spec.Function == nullptr
                || Spec.Function->HasAnyFunctionFlags(FUNC_Static)
                || (Blueprint->GeneratedClass != nullptr && Blueprint->GeneratedClass->IsChildOf(Spec.Function->GetOwnerClass()))) continue;
            UEdGraphPin* SelfPin = FindPin(Spec.Preview, UEdGraphSchema_K2::PN_Self.ToString(), EGPD_Input, Error);
            if (SelfPin == nullptr) return false;
            const bool bPlannedTarget = Connections.ContainsByPredicate([&](const FPatchConnection& Connection)
                { return Connection.TargetId == Spec.Id && Connection.TargetPin.Equals(SelfPin->PinName.ToString(), ESearchCase::IgnoreCase); });
            if (SelfPin->LinkedTo.IsEmpty() && !bPlannedTarget)
            { Error = FString::Printf(TEXT("External instance function node '%s' requires an explicit self/target connection."), *Spec.Id); return false; }
        }

        if (bDryRun) { FinalRevision = InitialRevision; return true; }

        FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Mutating,
            TEXT("Applying the preflighted graph patch transaction."));

        bool bApplyFailed = false;
        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "ApplyBlueprintGraphPatch", "UnrealMCP Apply Blueprint Graph Patch"));
            Blueprint->Modify(); Graph->Modify();
            for (FPatchNode& Spec : Nodes)
            {
                if (Spec.Live == nullptr)
                {
                    Spec.Live = CreateNode(Graph, Spec, false);
                    if (Spec.Live == nullptr) { Error = TEXT("Failed to create a preflighted node."); bApplyFailed = true; break; }
                    BlueprintGraphEditToolUtils::FPlacement Placement; Placement.X = Spec.X; Placement.Y = Spec.Y;
                    BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Spec.Live, Placement);
                    Spec.Live->NodeGuid = Spec.DeterministicGuid;
                    Spec.bCreated = true; bChanged = true;
                    if (!ApplyDefaults(Spec, Spec.Live, Schema, false, Error)) { bApplyFailed = true; break; }
                }
                else
                {
                    Spec.Live->Modify();
                    if (Spec.Live->NodePosX != Spec.X || Spec.Live->NodePosY != Spec.Y)
                    { Spec.Live->NodePosX = Spec.X; Spec.Live->NodePosY = Spec.Y; Spec.bMoved = true; bChanged = true; }
                }
                if (Spec.bHasComment && Spec.Live->NodeComment != Spec.Comment)
                { Spec.Live->NodeComment = Spec.Comment; Spec.bCommentChanged = true; bChanged = true; }
            }
            if (!bApplyFailed) for (FPatchDisconnection& Disconnection : Disconnections)
            {
                UEdGraphPin* Source = FindPin(ById.FindRef(Disconnection.SourceId)->Live, Disconnection.SourcePin, EGPD_Output, Error);
                UEdGraphPin* Target = FindPin(ById.FindRef(Disconnection.TargetId)->Live, Disconnection.TargetPin, EGPD_Input, Error);
                if (Source == nullptr || Target == nullptr) { bApplyFailed = true; break; }
                if (Source->LinkedTo.Contains(Target))
                { Schema->BreakSinglePinLink(Source, Target); Disconnection.bDisconnected = true; bChanged = true; }
            }
            if (!bApplyFailed) for (FPatchConnection& Connection : Connections)
            {
                UEdGraphPin* Source = FindPin(ById.FindRef(Connection.SourceId)->Live, Connection.SourcePin, EGPD_Output, Error);
                UEdGraphPin* Target = FindPin(ById.FindRef(Connection.TargetId)->Live, Connection.TargetPin, EGPD_Input, Error);
                if (Source == nullptr || Target == nullptr) { bApplyFailed = true; break; }
                if (Source->LinkedTo.Contains(Target)) continue;
                if (!Target->LinkedTo.IsEmpty())
                {
                    UEdGraphPin* ExistingSource = FindPin(ById.FindRef(Connection.ExistingSourceId)->Live, Connection.ExistingSourcePin, EGPD_Output, Error);
                    if (ExistingSource == nullptr || Target->LinkedTo.Num() != 1 || !Target->LinkedTo.Contains(ExistingSource))
                    { Error = TEXT("Confirmed replacement link changed after preflight."); bApplyFailed = true; break; }
                    Schema->BreakSinglePinLink(ExistingSource, Target); Connection.bReplaced = true;
                }
                if (!Schema->TryCreateConnection(Source, Target))
                { Error = TEXT("Unreal rejected a preflighted direct connection."); bApplyFailed = true; break; }
                Connection.bConnected = true; bChanged = true;
            }
            if (!bApplyFailed && bChanged)
            {
                Graph->NotifyGraphChanged();
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            }
        }
        if (bApplyFailed)
        {
            bRolledBack = GEditor != nullptr && GEditor->UndoTransaction();
            if (bRolledBack) FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::RolledBack,
                TEXT("Patch application failed and the editor transaction was rolled back."));
            if (!bRolledBack) Error += TEXT(" Immediate transaction rollback failed.");
            return false;
        }

        if (bCompile && bChanged)
        {
            FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Compiling,
                TEXT("Compiling the mutated Blueprint before any save."));
            FCompilerResultsLog Log; Log.bSilentMode = true;
            FKismetEditorUtilities::CompileBlueprint(
                Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Log);
            bCompiled = true; CompileErrors = Log.NumErrors; CompileWarnings = Log.NumWarnings;
            bCompileSucceeded = CompileErrors == 0
                && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
            if (!bCompileSucceeded)
            {
                bRolledBack = GEditor != nullptr && GEditor->UndoTransaction();
                if (bRolledBack) FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::RolledBack,
                    TEXT("Blueprint compilation failed and the graph patch was rolled back."));
                if (bRolledBack)
                {
                    FCompilerResultsLog RollbackLog;
                    RollbackLog.bSilentMode = true;
                    FKismetEditorUtilities::CompileBlueprint(
                        Blueprint,
                        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection,
                        &RollbackLog);
                }
                Error = FString::Printf(TEXT("Blueprint compilation failed with %d errors; patch rolled back=%s."),
                    CompileErrors, bRolledBack ? TEXT("true") : TEXT("false"));
                return false;
            }
        }
        else bCompileSucceeded = !bCompile || !bChanged;

        if (bSave && bChanged)
        {
            if (!BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, Error))
            {
                bRolledBack = GEditor != nullptr && GEditor->UndoTransaction();
                if (bRolledBack) FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::RolledBack,
                    TEXT("Asset save failed and the graph patch was rolled back."));
                return false;
            }
            bSaved = true;
            bIndexRefreshed = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexError);
        }
        FinalRevision = BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
        return true;
    }, ExecutionError);

    if (!bSucceeded)
        return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError);

    TArray<TSharedPtr<FJsonValue>> NodeResults;
    for (const FPatchNode& Node : Nodes)
    {
        const UEdGraphNode* ResultNode = bDryRun ? Node.Preview : Node.Live;
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("id"), Node.Id); Item->SetStringField(TEXT("kind"), Node.Kind);
        Item->SetStringField(TEXT("nodeGuid"), bDryRun && !Node.bReused
            ? Node.DeterministicGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
            : BlueprintGraphEditToolUtils::GetNodeGuid(ResultNode));
        Item->SetStringField(TEXT("nodeClass"), ResultNode != nullptr ? ResultNode->GetClass()->GetPathName() : FString());
        Item->SetStringField(TEXT("graphName"), ResultNode != nullptr && ResultNode->GetGraph() != nullptr ? ResultNode->GetGraph()->GetName() : FString());
        Item->SetBoolField(TEXT("created"), Node.bCreated); Item->SetBoolField(TEXT("reused"), Node.bReused);
        Item->SetBoolField(TEXT("wouldCreate"), bDryRun && !Node.bReused);
        Item->SetBoolField(TEXT("moved"), Node.bMoved); Item->SetBoolField(TEXT("commentChanged"), Node.bCommentChanged);
        Item->SetNumberField(TEXT("positionX"), Node.X); Item->SetNumberField(TEXT("positionY"), Node.Y);
        Item->SetArrayField(TEXT("pins"), BlueprintGraphEditToolUtils::SerializePins(ResultNode));
        NodeResults.Add(MakeShared<FJsonValueObject>(Item));
    }
    TArray<TSharedPtr<FJsonValue>> ConnectionResults;
    for (const FPatchConnection& Connection : Connections)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("sourceNodeId"), Connection.SourceId); Item->SetStringField(TEXT("sourcePinName"), Connection.SourcePin);
        Item->SetStringField(TEXT("targetNodeId"), Connection.TargetId); Item->SetStringField(TEXT("targetPinName"), Connection.TargetPin);
        Item->SetBoolField(TEXT("alreadyConnected"), Connection.bAlreadyConnected);
        Item->SetBoolField(TEXT("connected"), Connection.bConnected); Item->SetBoolField(TEXT("replacedExistingDataLink"), Connection.bReplaced);
        Item->SetBoolField(TEXT("sourceWildcardBefore"), Connection.bSourceWildcardBefore);
        Item->SetBoolField(TEXT("targetWildcardBefore"), Connection.bTargetWildcardBefore);
        Item->SetBoolField(TEXT("sourceWildcardAfter"), Connection.bSourceWildcardAfter);
        Item->SetBoolField(TEXT("targetWildcardAfter"), Connection.bTargetWildcardAfter);
        Item->SetBoolField(TEXT("wildcardResolved"), Connection.bWildcardResolved);
        ConnectionResults.Add(MakeShared<FJsonValueObject>(Item));
    }
    TArray<TSharedPtr<FJsonValue>> DisconnectionResults;
    for (const FPatchDisconnection& Disconnection : Disconnections)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("sourceNodeId"), Disconnection.SourceId); Item->SetStringField(TEXT("sourcePinName"), Disconnection.SourcePin);
        Item->SetStringField(TEXT("targetNodeId"), Disconnection.TargetId); Item->SetStringField(TEXT("targetPinName"), Disconnection.TargetPin);
        Item->SetBoolField(TEXT("alreadyDisconnected"), Disconnection.bAlreadyDisconnected);
        Item->SetBoolField(TEXT("disconnected"), Disconnection.bDisconnected);
        DisconnectionResults.Add(MakeShared<FJsonValueObject>(Item));
    }

    FMCPResponse Response; Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    int32 WildcardResolvedConnectionCount = 0;
    for (const FPatchConnection& Connection : Connections)
        if (Connection.bWildcardResolved) ++WildcardResolvedConnectionCount;
    Result->SetStringField(TEXT("objectPath"), ObjectPath); Result->SetStringField(TEXT("patchId"), PatchId);
    Result->SetStringField(TEXT("initialGraphRevision"), InitialRevision); Result->SetStringField(TEXT("finalGraphRevision"), FinalRevision);
    Result->SetBoolField(TEXT("dryRun"), bDryRun); Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetBoolField(TEXT("wildcardSpecializationSimulated"), true);
    Result->SetNumberField(TEXT("wildcardResolvedConnectionCount"), WildcardResolvedConnectionCount);
    Result->SetBoolField(TEXT("alreadyComplete"), !bDryRun && !bChanged); Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
    Result->SetArrayField(TEXT("nodes"), NodeResults); Result->SetArrayField(TEXT("connections"), ConnectionResults);
    Result->SetArrayField(TEXT("disconnections"), DisconnectionResults);
    Result->SetBoolField(TEXT("compiled"), bCompiled); Result->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
    Result->SetNumberField(TEXT("compileErrorCount"), CompileErrors); Result->SetNumberField(TEXT("compileWarningCount"), CompileWarnings);
    Result->SetBoolField(TEXT("saved"), bSaved); Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed); Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FApplyBlueprintGraphPatchTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("patchId"), TEXT("expectedGraphRevision"), TEXT("operationId")})
        Properties->SetObjectField(FieldName, BuildStringProperty(TEXT("Blueprint, graph, deterministic patch, or revision selector.")));
    TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>(); Nodes->SetStringField(TEXT("type"), TEXT("array"));
    Nodes->SetStringField(TEXT("description"), TEXT("Required node kinds: existingNode, functionCall, variableGet, variableSet, typedOperator, branch, and reroute. Nodes may include position, comment, tolerance, and inputDefaults."));
    TSharedRef<FJsonObject> NodeItem = MakeShared<FJsonObject>(); NodeItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> NodeProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("id"), TEXT("kind"), TEXT("nodeGuid"), TEXT("ownerClassPath"), TEXT("functionName"), TEXT("variableName"), TEXT("operator"), TEXT("comment")})
        NodeProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Plan-local identity or node-specific selector.")));
    TSharedRef<FJsonObject> Integer = MakeShared<FJsonObject>(); Integer->SetStringField(TEXT("type"), TEXT("integer"));
    TSharedRef<FJsonObject> Number = MakeShared<FJsonObject>(); Number->SetStringField(TEXT("type"), TEXT("number"));
    NodeProperties->SetObjectField(TEXT("positionX"), Integer); NodeProperties->SetObjectField(TEXT("positionY"), Integer);
    NodeProperties->SetObjectField(TEXT("tolerance"), Number);
    TSharedRef<FJsonObject> Defaults = MakeShared<FJsonObject>(); Defaults->SetStringField(TEXT("type"), TEXT("array"));
    TSharedRef<FJsonObject> DefaultItem = MakeShared<FJsonObject>(); DefaultItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> DefaultProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("pinName"), TEXT("defaultValue"), TEXT("defaultObjectPath")})
        DefaultProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Exact input pin and one default source.")));
    DefaultItem->SetObjectField(TEXT("properties"), DefaultProperties); DefaultItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("pinName"))});
    Defaults->SetObjectField(TEXT("items"), DefaultItem); NodeProperties->SetObjectField(TEXT("inputDefaults"), Defaults);
    NodeItem->SetObjectField(TEXT("properties"), NodeProperties);
    NodeItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("id")), MakeShared<FJsonValueString>(TEXT("kind"))});
    Nodes->SetObjectField(TEXT("items"), NodeItem); Properties->SetObjectField(TEXT("nodes"), Nodes);

    TSharedRef<FJsonObject> Connections = MakeShared<FJsonObject>(); Connections->SetStringField(TEXT("type"), TEXT("array"));
    Connections->SetStringField(TEXT("description"), TEXT("Exact output-to-input connections. Occupied data inputs require exact existing source fields plus confirmDataReplacement=true."));
    TSharedRef<FJsonObject> ConnectionItem = MakeShared<FJsonObject>(); ConnectionItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> ConnectionProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("sourceNodeId"), TEXT("sourcePinName"), TEXT("targetNodeId"), TEXT("targetPinName"), TEXT("existingSourceNodeId"), TEXT("existingSourcePinName")})
        ConnectionProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Plan node id and exact pin name.")));
    ConnectionProperties->SetObjectField(TEXT("confirmDataReplacement"), BuildBoolProperty(TEXT("Explicitly confirm replacement of the identified occupied data link.")));
    ConnectionItem->SetObjectField(TEXT("properties"), ConnectionProperties);
    ConnectionItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("sourceNodeId")), MakeShared<FJsonValueString>(TEXT("sourcePinName")), MakeShared<FJsonValueString>(TEXT("targetNodeId")), MakeShared<FJsonValueString>(TEXT("targetPinName"))});
    Connections->SetObjectField(TEXT("items"), ConnectionItem); Properties->SetObjectField(TEXT("connections"), Connections);

    TSharedRef<FJsonObject> Disconnections = MakeShared<FJsonObject>(); Disconnections->SetStringField(TEXT("type"), TEXT("array"));
    TSharedRef<FJsonObject> DisconnectionItem = MakeShared<FJsonObject>(); DisconnectionItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> DisconnectionProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("sourceNodeId"), TEXT("sourcePinName"), TEXT("targetNodeId"), TEXT("targetPinName")})
        DisconnectionProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Plan node id and exact linked pin name.")));
    DisconnectionProperties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Confirm this exact link disconnection.")));
    DisconnectionItem->SetObjectField(TEXT("properties"), DisconnectionProperties);
    DisconnectionItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("sourceNodeId")), MakeShared<FJsonValueString>(TEXT("sourcePinName")), MakeShared<FJsonValueString>(TEXT("targetNodeId")), MakeShared<FJsonValueString>(TEXT("targetPinName"))});
    Disconnections->SetObjectField(TEXT("items"), DisconnectionItem); Properties->SetObjectField(TEXT("disconnections"), Disconnections);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Run complete preflight without mutation.")));
    Properties->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile the edited Blueprint before any save. Defaults true.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh only after successful compilation. Defaults false.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("patchId")), MakeShared<FJsonValueString>(TEXT("nodes"))});
    return Schema;
}
