#include "Tools/Blueprint/Graph/PruneUnreachableNodesTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintGraphEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"

FPruneUnreachableNodesTool::FPruneUnreachableNodesTool()
    : FMCPToolBase(TEXT("PruneUnreachableNodes"),
        TEXT("Previews or removes unreachable Blueprint nodes while preserving entry points, reachable execution and required data. Apply requires the preview revision and confirm=true."))
{
}

UnrealMCP::FMCPResponse FPruneUnreachableNodesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString Path, GraphName, Guid, Expected, Error;
    if (Request.Params)
    {
        Request.Params->TryGetStringField(TEXT("objectPath"), Path);
        Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
        Request.Params->TryGetStringField(TEXT("graphGuid"), Guid);
        Request.Params->TryGetStringField(TEXT("expectedRevision"), Expected);
    }
    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), true);
    if (Path.IsEmpty() || (GraphName.IsEmpty() && Guid.IsEmpty())
        || (!bDryRun && (Expected.IsEmpty() || !BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirm"), false))))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams,
            TEXT("Provide objectPath and graphName or graphGuid. Apply requires confirm=true and expectedRevision from a preview."));
    }

    auto Result = BuildBooleanResult(true);
    const bool bSuccess = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        UEdGraph* Graph = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(Path, Blueprint, OutError)
            || !BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, Guid, Graph, OutError))
            return false;
        if (!Cast<UEdGraphSchema_K2>(Graph->GetSchema()) || Graph->Nodes.Num() > 2048)
        {
            OutError = TEXT("Pruning requires a K2 graph with at most 2048 nodes.");
            return false;
        }
        const FString Revision = BlueprintGraphEditToolUtils::ComputeGraphRevision(Graph);
        if (!Expected.IsEmpty() && Expected != Revision)
        {
            OutError = TEXT("Graph changed since preview.");
            return false;
        }

        TSet<UEdGraphNode*> Kept;
        TArray<UEdGraphNode*> Work;
        auto Keep = [&](UEdGraphNode* Node)
        {
            if (Node && Node->GetGraph() == Graph && !Kept.Contains(Node))
            {
                Kept.Add(Node);
                Work.Add(Node);
            }
        };
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && (Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_FunctionEntry>()
                || (Node->GetClass() == UK2Node_Tunnel::StaticClass() && CastChecked<UK2Node_Tunnel>(Node)->bCanHaveOutputs)))
                Keep(Node);
        }
        if (Work.IsEmpty())
        {
            OutError = TEXT("No supported entry point found. No nodes were pruned.");
            return false;
        }
        for (int32 Index = 0; Index < Work.Num(); ++Index)
        {
            for (const UEdGraphPin* Pin : Work[Index]->Pins)
                if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    for (const UEdGraphPin* Linked : Pin->LinkedTo)
                        if (Linked) Keep(Linked->GetOwningNode());
        }

        // Preserve structural endpoints and walk data dependencies without following their exec outputs.
        for (UEdGraphNode* Node : Graph->Nodes)
            if (Node && (!Node->CanUserDeleteNode() || Node->IsA<UEdGraphNode_Comment>()
                || Node->IsA<UK2Node_FunctionResult>() || Node->GetClass() == UK2Node_Tunnel::StaticClass()))
                Keep(Node);
        for (int32 Index = 0; Index < Work.Num(); ++Index)
            for (const UEdGraphPin* Pin : Work[Index]->Pins)
                if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                    for (const UEdGraphPin* Linked : Pin->LinkedTo)
                        if (Linked) Keep(Linked->GetOwningNode());

        TArray<UEdGraphNode*> Remove;
        TArray<TSharedPtr<FJsonValue>> RemovedGuids;
        for (UEdGraphNode* Node : Graph->Nodes)
            if (Node && !Kept.Contains(Node))
            {
                Remove.Add(Node);
                RemovedGuids.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
            }
        if (!bDryRun && !Remove.IsEmpty())
        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "PruneUnreachableNodes", "Prune Unreachable Blueprint Nodes"));
            Blueprint->Modify();
            Graph->Modify();
            for (UEdGraphNode* Node : Remove)
            {
                Node->Modify();
                FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
            }
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        }
        Result->SetStringField(TEXT("revision"), Revision);
        Result->SetArrayField(TEXT("unreachableNodeGuids"), RemovedGuids);
        Result->SetBoolField(TEXT("dryRun"), bDryRun);
        Result->SetNumberField(TEXT("removedCount"), bDryRun ? 0 : Remove.Num());
        return true;
    }, Error);

    if (!bSuccess) return BuildError(Request, EMCPErrorCode::InvalidParams, Error);
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FPruneUnreachableNodesTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto Schema = MakeShared<FJsonObject>();
    auto Properties = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Exact graph GUID.")));
    Properties->SetObjectField(TEXT("expectedRevision"), BuildStringProperty(TEXT("Preview revision required for apply.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Preview only. Defaults to true.")));
    Properties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Confirm removal when dryRun=false.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath"))});
    return Schema;
}
