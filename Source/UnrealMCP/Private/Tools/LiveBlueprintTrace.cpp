#include "Tools/LiveBlueprintToolUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Message.h"
#include "K2Node_Tunnel.h"
#include "Tools/BlueprintGraphEditToolUtils.h"

namespace UnrealMCP::LiveBlueprintToolUtils
{
    namespace
    {
        struct FVisit
        {
            UEdGraphNode* Node = nullptr;
            int32 Depth = 0;
            int32 CallDepth = 0;
            // An input producer contributes data, but its exec successors are not thereby reachable.
            bool bDependency = false;
        };

        struct FBody
        {
            TArray<UEdGraphNode*> Entries;
            TArray<UEdGraphNode*> Exits;
            FString Reason;
            FString Confidence = TEXT("exact");
            bool bBoundary = false;
        };

        FBody FindBody(UEdGraphNode* Node)
        {
            FBody Body;
            UEdGraph* Graph = nullptr;
            if (auto* Composite = Cast<UK2Node_Composite>(Node)) Graph = Composite->BoundGraph;
            else if (auto* Macro = Cast<UK2Node_MacroInstance>(Node)) Graph = Macro->GetMacroGraph();
            else if (auto* Call = Cast<UK2Node_CallFunction>(Node))
            {
                const UFunction* Function = Call->GetTargetFunction();
                if (Cast<UK2Node_Message>(Call))
                {
                    Body.Reason = TEXT("dynamic_interface_dispatch");
                    Body.bBoundary = true;
                    return Body;
                }
                TArray<UBlueprint*> CandidateBlueprints;
                if (Call->FunctionReference.IsSelfContext())
                {
                    if (UBlueprint* Self = Node->GetTypedOuter<UBlueprint>()) CandidateBlueprints.Add(Self);
                }
                if (Function && Function->GetOwnerClass())
                {
                    if (auto* DeclaredOwner = Cast<UBlueprint>(Function->GetOwnerClass()->ClassGeneratedBy)) CandidateBlueprints.AddUnique(DeclaredOwner);
                }
                for (UBlueprint* Blueprint : CandidateBlueprints)
                {
                    TArray<UEdGraph*> Graphs;
                    Blueprint->GetAllGraphs(Graphs);
                    for (UEdGraph* Candidate : Graphs)
                    {
                        if (!Candidate) continue;
                        for (UEdGraphNode* Entry : Candidate->Nodes)
                        {
                            if ((Cast<UK2Node_FunctionEntry>(Entry) && Candidate->GetFName() == Call->GetFunctionName())
                                || (Cast<UK2Node_CustomEvent>(Entry) && MemberName(Entry) == Call->GetFunctionName().ToString()))
                            {
                                if (Graph && Graph != Candidate)
                                {
                                    Body.Reason = TEXT("ambiguous_function_body");
                                    Body.bBoundary = true;
                                    return Body;
                                }
                                Graph = Candidate;
                                Body.Entries.AddUnique(Entry);
                            }
                        }
                    }
                    // External instance dispatch can select an override at runtime.
                    if (!Call->FunctionReference.IsSelfContext() && Function && !Function->HasAnyFunctionFlags(FUNC_Static)) Body.Confidence = TEXT("inferred_declared_target");
                    if (Graph) break;
                }
                if (!Graph)
                {
                    Body.bBoundary = true;
                    Body.Reason = Function && Function->HasAnyFunctionFlags(FUNC_Native) ? TEXT("native_function_body") : TEXT("unresolved_or_nonresident_function_body");
                    return Body;
                }
            }
            else return Body;

            if (!Graph)
            {
                Body.bBoundary = true;
                Body.Reason = TEXT("unavailable_macro_or_composite_graph");
                return Body;
            }
            for (UEdGraphNode* Inner : Graph->Nodes)
            {
                if (!Inner) continue;
                if (Cast<UK2Node_FunctionResult>(Inner)) Body.Exits.Add(Inner);
                // Macro/composite instances also derive from Tunnel. Only the actual
                // boundary tunnel nodes are roots/exits; nested calls must be reached by links.
                if (Inner->GetClass() == UK2Node_Tunnel::StaticClass())
                {
                    const auto* Tunnel = CastChecked<UK2Node_Tunnel>(Inner);
                    if (Tunnel->bCanHaveOutputs) Body.Entries.AddUnique(Inner);
                    if (Tunnel->bCanHaveInputs) Body.Exits.AddUnique(Inner);
                }
            }
            Body.Reason = TEXT("live_graph_body");
            if (Body.Entries.IsEmpty())
            {
                Body.bBoundary = true;
                Body.Reason = TEXT("unresolved_graph_entry");
            }
            return Body;
        }

        TArray<TSharedPtr<FJsonValue>> Bindings(UEdGraphNode* Caller, const FBody& Body)
        {
            TArray<TSharedPtr<FJsonValue>> Result;
            auto Bind = [&](UEdGraphPin* OuterPin, UEdGraphNode* Inner, EEdGraphPinDirection InnerDirection, const TCHAR* Kind)
            {
                if (!OuterPin || !Inner) return;
                UEdGraphPin* Match = nullptr;
                for (UEdGraphPin* Pin : Inner->Pins)
                {
                    if (!Pin || Pin->Direction != InnerDirection) continue;
                    const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && OuterPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
                    // Function exec names differ (execute/then); tunnel pins retain names.
                    const bool bFunctionExec = bExec && (Cast<UK2Node_FunctionEntry>(Inner) || Cast<UK2Node_FunctionResult>(Inner) || Cast<UK2Node_CustomEvent>(Inner));
                    if (Pin->PinName == OuterPin->PinName || bFunctionExec)
                    {
                        if (Match) return; // Ambiguous mapping is not fabricated.
                        Match = Pin;
                    }
                }
                if (!Match) return;
                auto Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("kind"), Kind);
                Item->SetObjectField(TEXT("caller"), Endpoint(OuterPin));
                Item->SetObjectField(TEXT("body"), Endpoint(Match));
                Item->SetStringField(TEXT("confidence"), Body.Confidence);
                Result.Add(MakeShared<FJsonValueObject>(Item));
            };
            for (UEdGraphPin* Pin : Caller->Pins)
            {
                if (!Pin) continue;
                if (Pin->Direction == EGPD_Input) for (UEdGraphNode* Entry : Body.Entries) Bind(Pin, Entry, EGPD_Output, TEXT("argument_or_entry"));
                else for (UEdGraphNode* Exit : Body.Exits) Bind(Pin, Exit, EGPD_Input, TEXT("return_or_exit"));
            }
            return Result;
        }
    }

    bool Trace(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& Result, FString& Error)
    {
        FTarget Target;
        if (!Resolve(Params, Target, Error)) return false;
        const FString Guid = String(Params, TEXT("startNodeGuid"));
        FString Event = String(Params, TEXT("startEvent"));
        if (!Guid.IsEmpty() && !Event.IsEmpty())
        {
            Error = TEXT("conflicting_start: use startNodeGuid or startEvent.");
            return false;
        }
        if (Event.IsEmpty()) Event = TEXT("BeginPlay");
        FGuid ParsedGuid;
        if (!Guid.IsEmpty() && !FGuid::Parse(Guid, ParsedGuid))
        {
            Error = TEXT("invalid_node_guid: startNodeGuid must be a GUID.");
            return false;
        }
        UEdGraphNode* Start = nullptr;
        for (UEdGraph* Graph : Target.Graphs)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                const FString Member = MemberName(Node);
                const bool bMatches = !Guid.IsEmpty() ? Node->NodeGuid == ParsedGuid
                    : Cast<UK2Node_Event>(Node) && (Member.Equals(Event, ESearchCase::IgnoreCase)
                        || (Event.Equals(TEXT("BeginPlay"), ESearchCase::IgnoreCase) && Member == TEXT("ReceiveBeginPlay")));
                if (!bMatches) continue;
                if (Start)
                {
                    Error = TEXT("ambiguous_start: more than one start node; use graphPath and startNodeGuid from InspectLiveBlueprint.");
                    return false;
                }
                Start = Node;
            }
        }
        if (!Start)
        {
            Error = TEXT("start_not_found: inspect live nodes and select a startNodeGuid or exact event member name.");
            return false;
        }

        const int32 MaxNodes = Bound(Params, TEXT("maxNodes"), 100, 1, 1000);
        const int32 MaxDepth = Bound(Params, TEXT("maxDepth"), 128, 0, 512);
        const int32 MaxCallDepth = Bound(Params, TEXT("maxCallDepth"), 4, 0, 16);
        bool bIncludePins = true;
        if (Params) Params->TryGetBoolField(TEXT("includePins"), bIncludePins);
        TArray<FVisit> Queue{{Start, 0, 0, false}};
        TSet<FString> Scheduled;
        TSet<UEdGraphNode*> Included;
        TSet<FString> EdgeKeys;
        TMap<UEdGraphNode*, int32> ExpandedAtCallDepth;
        TArray<TSharedPtr<FJsonValue>> Nodes, Edges, Expansions, Boundaries, Frontier;
        TSet<FString> FrontierKeys;
        int32 RevisitCount = 0, UnresolvedCount = 0;
        bool bTruncated = false;
        auto VisitKey = [](const FVisit& Visit)
        {
            return Visit.Node->GetPathName() + (Visit.bDependency ? TEXT("|data|") : TEXT("|flow|")) + FString::FromInt(Visit.CallDepth);
        };
        Scheduled.Add(VisitKey(Queue[0]));
        auto Defer = [&](const FVisit& Visit, const TCHAR* Reason)
        {
            bTruncated = true;
            const FString Key = VisitKey(Visit);
            if (FrontierKeys.Contains(Key)) return;
            FrontierKeys.Add(Key);
            auto Item = DescribeNode(Visit.Node, false);
            Item->SetStringField(TEXT("reason"), Reason);
            Item->SetBoolField(TEXT("dataDependency"), Visit.bDependency);
            Frontier.Add(MakeShared<FJsonValueObject>(Item));
        };
        auto Schedule = [&](UEdGraphNode* Node, const FVisit& From, bool bDependency, bool bCall)
        {
            if (!Node) return;
            FVisit Next{Node, From.Depth + 1, From.CallDepth + (bCall ? 1 : 0), bDependency};
            if (Scheduled.Contains(VisitKey(Next))) { ++RevisitCount; return; }
            if (Next.Depth > MaxDepth) { Defer(Next, TEXT("max_depth")); return; }
            if (Next.CallDepth > MaxCallDepth) { Defer(Next, TEXT("max_call_depth")); return; }
            Scheduled.Add(VisitKey(Next));
            Queue.Add(Next);
        };
        for (int32 Index = 0; Index < Queue.Num(); ++Index)
        {
            // Copy before Schedule can grow the queue.
            const FVisit Visit = Queue[Index];
            UEdGraphNode* Node = Visit.Node;
            if (!Included.Contains(Node))
            {
                if (Included.Num() >= MaxNodes) { Defer(Visit, TEXT("max_nodes")); continue; }
                Included.Add(Node);
                auto Item = DescribeNode(Node, bIncludePins);
                Item->SetNumberField(TEXT("depth"), Visit.Depth);
                Item->SetBoolField(TEXT("firstReachedAsDataDependency"), Visit.bDependency);
                Nodes.Add(MakeShared<FJsonValueObject>(Item));
            }
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
                const bool bUpstreamData = !bExec && Pin->Direction == EGPD_Input;
                const bool bDownstream = !Visit.bDependency && Pin->Direction == EGPD_Output;
                if (!bUpstreamData && !bDownstream) continue;
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    if (!Linked || !Linked->GetOwningNode()) continue;
                    const UEdGraphPin* Source = Pin->Direction == EGPD_Output ? Pin : Linked;
                    const UEdGraphPin* Dest = Pin->Direction == EGPD_Output ? Linked : Pin;
                    const FString Key = Source->GetOwningNode()->GetPathName() + BlueprintGraphEditToolUtils::GetPinId(Source)
                        + TEXT("->") + Dest->GetOwningNode()->GetPathName() + BlueprintGraphEditToolUtils::GetPinId(Dest);
                    if (!EdgeKeys.Contains(Key))
                    {
                        EdgeKeys.Add(Key);
                        auto Edge = MakeShared<FJsonObject>();
                        Edge->SetObjectField(TEXT("from"), Endpoint(Source));
                        Edge->SetObjectField(TEXT("to"), Endpoint(Dest));
                        Edge->SetStringField(TEXT("kind"), bExec ? TEXT("exec") : TEXT("data"));
                        Edge->SetStringField(TEXT("source"), TEXT("live_editor"));
                        Edge->SetStringField(TEXT("confidence"), TEXT("exact_pin_link"));
                        Edges.Add(MakeShared<FJsonValueObject>(Edge));
                    }
                    Schedule(Linked->GetOwningNode(), Visit, bUpstreamData, false);
                }
            }

            const int32* PreviousCallDepth = ExpandedAtCallDepth.Find(Node);
            if (!PreviousCallDepth || Visit.CallDepth < *PreviousCallDepth)
            {
                ExpandedAtCallDepth.Add(Node, Visit.CallDepth);
                const FBody Body = FindBody(Node);
                if (Body.bBoundary)
                {
                    auto Boundary = DescribeNode(Node, false);
                    Boundary->SetStringField(TEXT("reason"), Body.Reason);
                    Boundary->SetBoolField(TEXT("callerContinuationFollowed"), !Visit.bDependency);
                    Boundaries.Add(MakeShared<FJsonValueObject>(Boundary));
                    if (Body.Reason != TEXT("native_function_body")) ++UnresolvedCount;
                }
                else if (!Body.Entries.IsEmpty())
                {
                    auto Expansion = DescribeNode(Node, false);
                    Expansion->SetStringField(TEXT("bodyGraphPath"), Body.Entries[0]->GetGraph()->GetPathName());
                    Expansion->SetStringField(TEXT("confidence"), Body.Confidence);
                    Expansion->SetArrayField(TEXT("pinBindings"), Bindings(Node, Body));
                    Expansion->SetStringField(TEXT("semantics"), TEXT("static_body_and_caller_continuation_not_runtime_order"));
                    Expansions.Add(MakeShared<FJsonValueObject>(Expansion));
                    for (UEdGraphNode* Entry : Body.Entries) Schedule(Entry, Visit, false, true);
                    // Return inputs also expose pure-function and macro data producers.
                    for (UEdGraphNode* Exit : Body.Exits) Schedule(Exit, Visit, true, true);
                }
            }
        }

        Result = DescribeTarget(Target);
        Result->SetStringField(TEXT("startNodeGuid"), BlueprintGraphEditToolUtils::GetNodeGuid(Start));
        Result->SetStringField(TEXT("startGraphPath"), Start->GetGraph()->GetPathName());
        Result->SetArrayField(TEXT("nodes"), Nodes);
        Result->SetArrayField(TEXT("edges"), Edges);
        Result->SetArrayField(TEXT("expansions"), Expansions);
        Result->SetArrayField(TEXT("boundaries"), Boundaries);
        Result->SetArrayField(TEXT("frontier"), Frontier);
        Result->SetNumberField(TEXT("nodeCount"), Nodes.Num());
        Result->SetNumberField(TEXT("edgeCount"), Edges.Num());
        Result->SetNumberField(TEXT("maxNodes"), MaxNodes);
        Result->SetNumberField(TEXT("maxDepth"), MaxDepth);
        Result->SetNumberField(TEXT("maxCallDepth"), MaxCallDepth);
        Result->SetNumberField(TEXT("revisitedNodeCount"), RevisitCount);
        Result->SetNumberField(TEXT("unresolvedBoundaryCount"), UnresolvedCount);
        Result->SetBoolField(TEXT("truncated"), bTruncated);
        Result->SetBoolField(TEXT("hasMore"), bTruncated);
        Result->SetBoolField(TEXT("coverageComplete"), !bTruncated && UnresolvedCount == 0);
        Result->SetStringField(TEXT("coverageScope"), TEXT("connected_static_wiring_and_resident_declared_bodies; native implementations and runtime dispatch are boundaries"));
        Result->SetStringField(TEXT("continuationHint"), TEXT("Increase bounds, or inspect frontier nodes using objectPath/graphPath/nodeGuid. Trace a frontier node with startNodeGuid; dataDependency frontiers are value producers, not proof of execution."));
        return true;
    }
}
