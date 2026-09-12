#include "Tools/SpliceBlueprintExecFlowTool.h"

#include "MCP/MutationRequestTracker.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    struct FSpliceNode
    {
        FString NodeGuid, InputPinName, OutputPinName;
        UEdGraphNode* Node = nullptr;
        UEdGraphPin* Input = nullptr;
        UEdGraphPin* Output = nullptr;
    };

    bool ParseSpliceNode(const TSharedPtr<FJsonObject>& Object, FSpliceNode& Out, FString& Error)
    {
        if (!Object.IsValid()
            || !Object->TryGetStringField(TEXT("nodeGuid"), Out.NodeGuid)
            || !Object->TryGetStringField(TEXT("inputPinName"), Out.InputPinName)
            || !Object->TryGetStringField(TEXT("outputPinName"), Out.OutputPinName))
        { Error = TEXT("Each inserted node requires nodeGuid, inputPinName, and outputPinName."); return false; }
        FGuid Guid;
        if (!FGuid::Parse(Out.NodeGuid, Guid)) { Error = TEXT("Inserted nodeGuid is invalid."); return false; }
        return true;
    }

    UEdGraphPin* ResolveNamedPin(
        UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Direction, FString& Error)
    {
        UEdGraphPin* Result = nullptr;
        if (Node == nullptr) { Error = TEXT("Cannot resolve a pin on a null node."); return nullptr; }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin != nullptr && Pin->Direction == Direction
                && Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                if (Result != nullptr) { Error = FString::Printf(TEXT("Pin '%s' is ambiguous."), *Name); return nullptr; }
                Result = Pin;
            }
        }
        if (Result == nullptr) Error = FString::Printf(TEXT("Pin '%s' was not found."), *Name);
        return Result;
    }

    bool HasOnlyLink(const UEdGraphPin* Pin, const UEdGraphPin* Expected)
    {
        return Pin != nullptr && Pin->LinkedTo.Num() == 1 && Pin->LinkedTo[0] == Expected;
    }

    TSharedPtr<FJsonValue> LinkJson(const UEdGraphPin* Source, const UEdGraphPin* Target)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("sourceNodeGuid"),
            UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Source != nullptr ? Source->GetOwningNode() : nullptr));
        Item->SetStringField(TEXT("sourcePinId"), UnrealMCP::BlueprintGraphEditToolUtils::GetPinId(Source));
        Item->SetStringField(TEXT("sourcePinName"), Source != nullptr ? Source->PinName.ToString() : FString());
        Item->SetStringField(TEXT("targetNodeGuid"),
            UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Target != nullptr ? Target->GetOwningNode() : nullptr));
        Item->SetStringField(TEXT("targetPinId"), UnrealMCP::BlueprintGraphEditToolUtils::GetPinId(Target));
        Item->SetStringField(TEXT("targetPinName"), Target != nullptr ? Target->PinName.ToString() : FString());
        return MakeShared<FJsonValueObject>(Item);
    }
}

FSpliceBlueprintExecFlowTool::FSpliceBlueprintExecFlowTool()
    : FMCPToolBase(TEXT("SpliceBlueprintExecFlow"),
        TEXT("Atomically inserts an ordered chain of existing nodes into one exact Blueprint execution link, with revision checks, idempotent retry, rollback, and compile-before-save.")) {}

UnrealMCP::FMCPResponse FSpliceBlueprintExecFlowTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, GraphName, GraphGuid, SpliceId, ExpectedRevision, OperationId;
    FString SourceNodeGuid, SourcePinName, TargetNodeGuid, TargetPinName;
    const TArray<TSharedPtr<FJsonValue>>* InsertedValues = nullptr;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("spliceId"), SpliceId)
        || !Request.Params->TryGetStringField(TEXT("sourceNodeGuid"), SourceNodeGuid)
        || !Request.Params->TryGetStringField(TEXT("sourcePinName"), SourcePinName)
        || !Request.Params->TryGetStringField(TEXT("targetNodeGuid"), TargetNodeGuid)
        || !Request.Params->TryGetStringField(TEXT("targetPinName"), TargetPinName)
        || !Request.Params->TryGetArrayField(TEXT("insertedNodes"), InsertedValues)
        || InsertedValues == nullptr || InsertedValues->IsEmpty() || SpliceId.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams,
            TEXT("SpliceBlueprintExecFlow requires objectPath, graph selector, spliceId, exact source/target pins, and non-empty insertedNodes."));
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("expectedGraphRevision"), ExpectedRevision);
    Request.Params->TryGetStringField(TEXT("operationId"), OperationId);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("graphName or graphGuid is required."));

    TArray<FSpliceNode> InsertedNodes; TSet<FString> InsertedGuids;
    for (int32 Index = 0; Index < InsertedValues->Num(); ++Index)
    {
        FSpliceNode Node; FString Error;
        if (!ParseSpliceNode((*InsertedValues)[Index]->AsObject(), Node, Error) || InsertedGuids.Contains(Node.NodeGuid))
            return BuildError(Request, EMCPErrorCode::InvalidParams,
                FString::Printf(TEXT("Invalid inserted node %d: %s"), Index,
                    InsertedGuids.Contains(Node.NodeGuid) ? TEXT("duplicate nodeGuid") : *Error));
        if (Node.NodeGuid.Equals(SourceNodeGuid, ESearchCase::IgnoreCase)
            || Node.NodeGuid.Equals(TargetNodeGuid, ESearchCase::IgnoreCase))
            return BuildError(Request, EMCPErrorCode::InvalidParams,
                TEXT("Source and target nodes cannot also be inserted nodes."));
        InsertedGuids.Add(Node.NodeGuid); InsertedNodes.Add(MoveTemp(Node));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), true);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    // Splicing breaks an existing user execution link before rewiring it, so it is as
    // destructive as DisconnectBlueprintPins and carries the same confirmation gate. A
    // non-destructive-sounding tool name is not a reason to skip it.
    const bool bConfirm = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirm"), false);
    if (!bDryRun && !bConfirm)
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams,
            TEXT("SpliceBlueprintExecFlow breaks an existing execution link; inspect it with dryRun=true, then supply confirm=true."));
    }
    FString InitialRevision, FinalRevision, SavedFilename, IndexError, ExecutionError;
    bool bAlreadyComplete = false, bChanged = false, bCompiled = false, bCompileSucceeded = false;
    bool bSaved = false, bIndexRefreshed = false, bRolledBack = false;
    int32 CompileErrors = 0, CompileWarnings = 0;
    UEdGraphPin* SourcePin = nullptr; UEdGraphPin* TargetPin = nullptr;
    TArray<TSharedPtr<FJsonValue>> RemovedLinks, AddedLinks;

    FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Preflighting,
        TEXT("Resolving and validating the exact execution route."));

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

        UEdGraphNode* SourceNode = nullptr; UEdGraphNode* TargetNode = nullptr;
        if (!BlueprintGraphEditToolUtils::ResolveNode(Graph, SourceNodeGuid, SourceNode, Error)
            || !BlueprintGraphEditToolUtils::ResolveNode(Graph, TargetNodeGuid, TargetNode, Error)) return false;
        SourcePin = ResolveNamedPin(SourceNode, SourcePinName, EGPD_Output, Error);
        TargetPin = ResolveNamedPin(TargetNode, TargetPinName, EGPD_Input, Error);
        if (SourcePin == nullptr || TargetPin == nullptr) return false;
        if (SourcePin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
            || TargetPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
        { Error = TEXT("Source and target pins must be execution pins."); return false; }

        for (FSpliceNode& Inserted : InsertedNodes)
        {
            if (!BlueprintGraphEditToolUtils::ResolveNode(Graph, Inserted.NodeGuid, Inserted.Node, Error)) return false;
            Inserted.Input = ResolveNamedPin(Inserted.Node, Inserted.InputPinName, EGPD_Input, Error);
            Inserted.Output = ResolveNamedPin(Inserted.Node, Inserted.OutputPinName, EGPD_Output, Error);
            if (Inserted.Input == nullptr || Inserted.Output == nullptr) return false;
            if (Inserted.Input->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                || Inserted.Output->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            { Error = FString::Printf(TEXT("Inserted node '%s' pins must both be execution pins."), *Inserted.NodeGuid); return false; }
        }

        bool bComplete = HasOnlyLink(SourcePin, InsertedNodes[0].Input);
        for (int32 Index = 0; bComplete && Index < InsertedNodes.Num() - 1; ++Index)
            bComplete = HasOnlyLink(InsertedNodes[Index].Output, InsertedNodes[Index + 1].Input);
        bComplete = bComplete && HasOnlyLink(InsertedNodes.Last().Output, TargetPin);
        bAlreadyComplete = bComplete;
        if (bAlreadyComplete) { FinalRevision = InitialRevision; return true; }

        if (!HasOnlyLink(SourcePin, TargetPin) || !HasOnlyLink(TargetPin, SourcePin))
        { Error = TEXT("The exact source-to-target execution link is not present or is not the source's only route."); return false; }
        for (const FSpliceNode& Inserted : InsertedNodes)
        {
            if (!Inserted.Input->LinkedTo.IsEmpty() || !Inserted.Output->LinkedTo.IsEmpty())
            { Error = FString::Printf(TEXT("Inserted node '%s' already has execution wiring."), *Inserted.NodeGuid); return false; }
        }
        UEdGraphPin* PreviousOutput = SourcePin;
        for (int32 Index = 0; Index < InsertedNodes.Num(); ++Index)
        {
            const FSpliceNode& Inserted = InsertedNodes[Index];
            const FPinConnectionResponse Compatibility = Schema->CanCreateConnection(PreviousOutput, Inserted.Input);
            const bool bExpectedSourceBreak = Index == 0 && Compatibility.Response == CONNECT_RESPONSE_BREAK_OTHERS_A;
            if (Compatibility.Response != CONNECT_RESPONSE_MAKE && !bExpectedSourceBreak)
            { Error = FString::Printf(TEXT("An inserted execution input is incompatible: %s"), *Compatibility.Message.ToString()); return false; }
            PreviousOutput = Inserted.Output;
        }
        const FPinConnectionResponse FinalCompatibility = Schema->CanCreateConnection(PreviousOutput, TargetPin);
        if (FinalCompatibility.Response != CONNECT_RESPONSE_MAKE
            && FinalCompatibility.Response != CONNECT_RESPONSE_BREAK_OTHERS_B)
        { Error = FString::Printf(TEXT("Final inserted execution output is incompatible: %s"), *FinalCompatibility.Message.ToString()); return false; }
        if (bDryRun) { FinalRevision = InitialRevision; return true; }

        FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Mutating,
            TEXT("Applying the preflighted execution splice transaction."));

        bool bApplyFailed = false;
        {
            const FScopedTransaction Transaction(NSLOCTEXT(
                "UnrealMCP", "SpliceBlueprintExecFlow", "UnrealMCP Splice Blueprint Exec Flow"));
            Blueprint->Modify(); Graph->Modify(); SourceNode->Modify(); TargetNode->Modify();
            for (FSpliceNode& Inserted : InsertedNodes) Inserted.Node->Modify();
            RemovedLinks.Add(LinkJson(SourcePin, TargetPin));
            Schema->BreakSinglePinLink(SourcePin, TargetPin);
            PreviousOutput = SourcePin;
            for (FSpliceNode& Inserted : InsertedNodes)
            {
                if (!Schema->TryCreateConnection(PreviousOutput, Inserted.Input))
                { Error = TEXT("Unreal rejected a preflighted execution-chain connection."); bApplyFailed = true; break; }
                AddedLinks.Add(LinkJson(PreviousOutput, Inserted.Input));
                PreviousOutput = Inserted.Output;
            }
            if (!bApplyFailed && !Schema->TryCreateConnection(PreviousOutput, TargetPin))
            { Error = TEXT("Unreal rejected the final preflighted execution-chain connection."); bApplyFailed = true; }
            else if (!bApplyFailed) AddedLinks.Add(LinkJson(PreviousOutput, TargetPin));
            if (!bApplyFailed)
            {
                Graph->NotifyGraphChanged();
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
                bChanged = true;
            }
        }
        if (bApplyFailed)
        {
            bRolledBack = GEditor != nullptr && GEditor->UndoTransaction();
            if (bRolledBack) FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::RolledBack,
                TEXT("Execution splice application failed and the transaction was rolled back."));
            if (!bRolledBack) Error += TEXT(" Immediate transaction rollback failed.");
            return false;
        }

        if (bCompile)
        {
            FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Compiling,
                TEXT("Compiling the spliced Blueprint before any save."));
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
                    TEXT("Blueprint compilation failed and the execution splice was rolled back."));
                if (bRolledBack)
                {
                    FCompilerResultsLog RollbackLog;
                    RollbackLog.bSilentMode = true;
                    FKismetEditorUtilities::CompileBlueprint(
                        Blueprint,
                        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection,
                        &RollbackLog);
                }
                Error = FString::Printf(TEXT("Blueprint compilation failed with %d errors; splice rolled back=%s."),
                    CompileErrors, bRolledBack ? TEXT("true") : TEXT("false"));
                return false;
            }
        }
        else bCompileSucceeded = true;

        if (bSave)
        {
            if (!BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, Error))
            {
                bRolledBack = GEditor != nullptr && GEditor->UndoTransaction();
                if (bRolledBack) FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::RolledBack,
                    TEXT("Asset save failed and the execution splice was rolled back."));
                return false;
            }
            bSaved = true;
            bIndexRefreshed = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexError);
        }
        FinalRevision = BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
        return true;
    }, ExecutionError);

    if (!bSucceeded) return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError);

    TArray<TSharedPtr<FJsonValue>> NodeResults;
    for (const FSpliceNode& Node : InsertedNodes)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("nodeGuid"), BlueprintGraphEditToolUtils::GetNodeGuid(Node.Node));
        Item->SetStringField(TEXT("nodeClass"), Node.Node != nullptr ? Node.Node->GetClass()->GetPathName() : FString());
        Item->SetStringField(TEXT("inputPinId"), BlueprintGraphEditToolUtils::GetPinId(Node.Input));
        Item->SetStringField(TEXT("inputPinName"), Node.InputPinName);
        Item->SetStringField(TEXT("outputPinId"), BlueprintGraphEditToolUtils::GetPinId(Node.Output));
        Item->SetStringField(TEXT("outputPinName"), Node.OutputPinName);
        Item->SetArrayField(TEXT("pins"), BlueprintGraphEditToolUtils::SerializePins(Node.Node));
        NodeResults.Add(MakeShared<FJsonValueObject>(Item));
    }
    FMCPResponse Response; Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath); Result->SetStringField(TEXT("spliceId"), SpliceId);
    Result->SetStringField(TEXT("initialGraphRevision"), InitialRevision); Result->SetStringField(TEXT("finalGraphRevision"), FinalRevision);
    Result->SetBoolField(TEXT("dryRun"), bDryRun); Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetBoolField(TEXT("alreadyComplete"), bAlreadyComplete); Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
    Result->SetArrayField(TEXT("insertedNodes"), NodeResults);
    Result->SetArrayField(TEXT("removedLinks"), RemovedLinks); Result->SetArrayField(TEXT("addedLinks"), AddedLinks);
    Result->SetBoolField(TEXT("compiled"), bCompiled); Result->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
    Result->SetNumberField(TEXT("compileErrorCount"), CompileErrors); Result->SetNumberField(TEXT("compileWarningCount"), CompileWarnings);
    Result->SetBoolField(TEXT("saved"), bSaved); Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed); Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FSpliceBlueprintExecFlowTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("spliceId"),
        TEXT("expectedGraphRevision"), TEXT("sourceNodeGuid"), TEXT("sourcePinName"), TEXT("targetNodeGuid"), TEXT("targetPinName"), TEXT("operationId")})
        Properties->SetObjectField(FieldName, BuildStringProperty(TEXT("Blueprint, graph, splice, revision, node, or exact execution-pin selector.")));
    TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>(); Nodes->SetStringField(TEXT("type"), TEXT("array"));
    Nodes->SetStringField(TEXT("description"), TEXT("Ordered existing nodes to insert. Each node must have currently unwired exact execution input/output pins."));
    TSharedRef<FJsonObject> NodeItem = MakeShared<FJsonObject>(); NodeItem->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> NodeProperties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("nodeGuid"), TEXT("inputPinName"), TEXT("outputPinName")})
        NodeProperties->SetObjectField(FieldName, BuildStringProperty(TEXT("Exact inserted node and execution-pin selector.")));
    NodeItem->SetObjectField(TEXT("properties"), NodeProperties);
    NodeItem->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("nodeGuid")), MakeShared<FJsonValueString>(TEXT("inputPinName")), MakeShared<FJsonValueString>(TEXT("outputPinName"))});
    Nodes->SetObjectField(TEXT("items"), NodeItem); Properties->SetObjectField(TEXT("insertedNodes"), Nodes);
    Properties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required true for a real splice, because the existing execution link is broken and rewired.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate the exact route and ordered chain without mutation.")));
    Properties->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile before any save. Defaults true.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh after successful compilation. Defaults false.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {
        MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("spliceId")),
        MakeShared<FJsonValueString>(TEXT("sourceNodeGuid")), MakeShared<FJsonValueString>(TEXT("sourcePinName")),
        MakeShared<FJsonValueString>(TEXT("targetNodeGuid")), MakeShared<FJsonValueString>(TEXT("targetPinName")),
        MakeShared<FJsonValueString>(TEXT("insertedNodes"))});
    return Schema;
}
