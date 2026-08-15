#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/TraceBlueprintFlowToolInternal.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPTraceNodeSemanticsTest,
    "UnrealMCP.Trace.Semantics.NodeClassification",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealMCPTraceNodeSemanticsTest::RunTest(const FString& Parameters)
{
    FTraceNodeRecord TimelineNode;
    TimelineNode.NodeType = TEXT("timeline");
    ClassifyTraceNodeSemantics(TimelineNode);
    TestEqual(TEXT("Timeline semantic"), TimelineNode.ExecutionSemantic, FString(TEXT("timeline")));
    TestEqual(TEXT("Timeline confidence"), TimelineNode.SemanticConfidence, FString(TEXT("exact")));
    TestEqual(TEXT("Timeline reason"), TimelineNode.SemanticReason, FString(TEXT("indexed_timeline_node")));
    TestEqual(TEXT("Timeline continuation"), TimelineNode.ContinuationModel, FString(TEXT("update_and_finished_exec_outputs")));

    FTraceNodeRecord AsyncNode;
    AsyncNode.NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_AsyncAction");
    ClassifyTraceNodeSemantics(AsyncNode);
    TestEqual(TEXT("Async semantic"), AsyncNode.ExecutionSemantic, FString(TEXT("async")));
    TestEqual(TEXT("Async confidence"), AsyncNode.SemanticConfidence, FString(TEXT("exact")));
    TestEqual(TEXT("Async reason"), AsyncNode.SemanticReason, FString(TEXT("async_node_class")));
    TestEqual(TEXT("Async continuation"), AsyncNode.ContinuationModel, FString(TEXT("callback_exec_outputs")));

    FTraceNodeRecord TimerNode;
    TimerNode.MemberName = TEXT("SetTimerByFunctionName");
    ClassifyTraceNodeSemantics(TimerNode);
    TestEqual(TEXT("Timer semantic"), TimerNode.ExecutionSemantic, FString(TEXT("timer")));
    TestEqual(TEXT("Timer confidence"), TimerNode.SemanticConfidence, FString(TEXT("inferred")));
    TestEqual(TEXT("Timer reason"), TimerNode.SemanticReason, FString(TEXT("timer_member_name")));
    TestEqual(TEXT("Timer continuation"), TimerNode.ContinuationModel, FString(TEXT("scheduled_callback")));

    FTraceNodeRecord LatentNode;
    LatentNode.MemberName = TEXT("Delay");
    ClassifyTraceNodeSemantics(LatentNode);
    TestEqual(TEXT("Latent semantic"), LatentNode.ExecutionSemantic, FString(TEXT("latent")));
    TestEqual(TEXT("Latent confidence"), LatentNode.SemanticConfidence, FString(TEXT("inferred")));
    TestEqual(TEXT("Latent reason"), LatentNode.SemanticReason, FString(TEXT("known_latent_member")));
    TestEqual(TEXT("Latent continuation"), LatentNode.ContinuationModel, FString(TEXT("deferred_exec_output")));

    FTraceNodeRecord OrdinaryNode;
    OrdinaryNode.NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_CallFunction");
    OrdinaryNode.MemberName = TEXT("GetActorLocation");
    ClassifyTraceNodeSemantics(OrdinaryNode);
    TestTrue(TEXT("Ordinary node has no semantic"), OrdinaryNode.ExecutionSemantic.IsEmpty());
    TestTrue(TEXT("Ordinary node has no semantic confidence"), OrdinaryNode.SemanticConfidence.IsEmpty());
    TestTrue(TEXT("Ordinary node has no continuation model"), OrdinaryNode.ContinuationModel.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUnrealMCPIndexedTraceConfidenceTest,
    "UnrealMCP.Trace.Semantics.IndexedTraceConfidence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealMCPIndexedTraceConfidenceTest::RunTest(const FString& Parameters)
{
    const FString StartKey = TEXT("/Game/BP_Test.BP_Test::EventGraph::StartNode");
    const TArray<FString> StartNodeKeys = {StartKey};

    FTraceTraversalState ExactState;
    FTraceNodeRecord ExactNode;
    ExactNode.MatchReason = TEXT("node_title");
    ExactNode.Depth = 0;
    ExactState.NodesByKey.Add(StartKey, ExactNode);
    TArray<FTraceEdgeRecord> ExactEdges;
    ExactEdges.AddDefaulted();
    TestEqual(
        TEXT("Exact indexed trace"),
        CalculateIndexedTraceConfidence(StartNodeKeys, ExactState, ExactEdges),
        FString(TEXT("exact")));

    FTraceTraversalState InferredSemanticState = ExactState;
    InferredSemanticState.NodesByKey[StartKey].SemanticConfidence = TEXT("inferred");
    TestEqual(
        TEXT("Inferred node semantic lowers trace confidence"),
        CalculateIndexedTraceConfidence(StartNodeKeys, InferredSemanticState, ExactEdges),
        FString(TEXT("inferred")));

    TArray<FTraceEdgeRecord> InferredEdges;
    FTraceEdgeRecord& InferredEdge = InferredEdges.AddDefaulted_GetRef();
    InferredEdge.Confidence = TEXT("inferred");
    TestEqual(
        TEXT("Inferred edge lowers trace confidence"),
        CalculateIndexedTraceConfidence(StartNodeKeys, ExactState, InferredEdges),
        FString(TEXT("inferred")));

    FTraceTraversalState UnresolvedState = ExactState;
    UnresolvedState.UnresolvedTransitions.AddDefaulted();
    TestEqual(
        TEXT("Unresolved transition dominates trace confidence"),
        CalculateIndexedTraceConfidence(StartNodeKeys, UnresolvedState, ExactEdges),
        FString(TEXT("unresolved")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
