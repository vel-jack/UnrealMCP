#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FSQLiteDatabase;

struct FTraceNodeRecord
{
    FString BlueprintObjectPath;
    FString GraphName;
    FString NodeGuid;
    FString NodeName;
    FString NodeClassPath;
    FString NodeTitle;
    FString NodeType;
    FString MemberName;
    FString MemberParentPath;
    int32 PosX = 0;
    int32 PosY = 0;
    bool bIsPure = false;
    int32 InputPinCount = 0;
    int32 OutputPinCount = 0;
    bool bIsEntry = false;
    FString MatchReason;
    int32 Depth = INDEX_NONE;
    bool bHasResolvedMemberAsset = false;
    FString ResolvedMemberAssetObjectPath;
    FString ResolvedMemberAssetName;
    FString ResolvedMemberAssetPackageName;
    FString ResolvedMemberAssetScope;
    bool bResolvedMemberAssetIsBlueprint = false;
    FString ConnectedTargetClassPath;
    bool bHasResolvedTargetAsset = false;
    FString ResolvedTargetAssetObjectPath;
    FString ResolvedTargetAssetName;
    FString ExecutionSemantic;
    FString SemanticConfidence;
    FString SemanticReason;
    FString ContinuationModel;
    FString CallbackMemberName;
};

struct FTraceEdgeRecord
{
    FString SourceBlueprintObjectPath;
    FString SourceGraphName;
    FString SourceNodeGuid;
    FString SourcePinId;
    FString TargetBlueprintObjectPath;
    FString TargetGraphName;
    FString TargetNodeGuid;
    FString TargetPinId;
    FString EdgeKind;
    FString Confidence = TEXT("exact");
    FString ConfidenceReason = TEXT("indexed_pin_link");
};

struct FTraceUnresolvedTransitionRecord
{
    FString SourceBlueprintObjectPath;
    FString SourceGraphName;
    FString SourceNodeGuid;
    FString SourceNodeTitle;
    FString TransitionKind;
    FString Reason;
};

struct FGraphEntryRecord
{
    FString BlueprintObjectPath;
    FString GraphName;
    FString EntryNodeGuid;
    FString GraphType;
    int32 NodeCount = 0;
};

struct FTraceTraversalState
{
    TMap<FString, TSharedPtr<FJsonObject>> AssetsByObjectPath;
    TMap<FString, FTraceNodeRecord> NodesByKey;
    TMultiMap<FString, FTraceEdgeRecord> OutgoingEdgesByNode;
    TMultiMap<FString, FTraceEdgeRecord> IncomingEdgesByNode;
    TArray<FGraphEntryRecord> GraphEntries;
    TMap<FString, FString> PinMatchReasonsByNodeKey;
    TMap<FString, TArray<FString>> EntryNodeKeysByAssetAndMember;
    TSet<FString> LoadedBlueprintObjectPaths;
    TArray<FTraceUnresolvedTransitionRecord> UnresolvedTransitions;
};

struct FTraceFrontierItem
{
    FString NodeKey;
    int32 Depth = 0;
    int32 CallDepth = 0;
};

inline FString MakeTraceNodeKey(const FString& GraphName, const FString& NodeGuid)
{
    return GraphName + TEXT("::") + NodeGuid;
}

inline FString MakeTraceNodeKey(const FString& BlueprintObjectPath, const FString& GraphName, const FString& NodeGuid)
{
    return BlueprintObjectPath + TEXT("::") + GraphName + TEXT("::") + NodeGuid;
}

inline FString MakeTraceMemberLookupKey(const FString& BlueprintObjectPath, const FString& MemberName)
{
    return BlueprintObjectPath + TEXT("::") + MemberName.ToLower();
}

inline int32 GetTraceOptionalClampedInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 DefaultValue, int32 MinValue, int32 MaxValue)
{
    int32 Value = DefaultValue;
    if (Params.IsValid())
    {
        Params->TryGetNumberField(FieldName, Value);
    }

    return FMath::Clamp(Value, MinValue, MaxValue);
}

inline FString GetTraceOptionalString(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName)
{
    FString Value;
    if (Params.IsValid())
    {
        Params->TryGetStringField(FieldName, Value);
    }
    return Value;
}

inline bool TraceStringMatchesQuery(const FString& Value, const FString& Query)
{
    return !Value.IsEmpty() && !Query.IsEmpty() && Value.Contains(Query, ESearchCase::IgnoreCase);
}

inline void ClassifyTraceNodeSemantics(FTraceNodeRecord& Node)
{
    if (Node.NodeType == TEXT("timeline"))
    {
        Node.ExecutionSemantic = TEXT("timeline");
        Node.SemanticConfidence = TEXT("exact");
        Node.SemanticReason = TEXT("indexed_timeline_node");
        Node.ContinuationModel = TEXT("update_and_finished_exec_outputs");
        return;
    }

    if (Node.NodeClassPath.Contains(TEXT("K2Node_AsyncAction"))
        || Node.NodeClassPath.Contains(TEXT("K2Node_BaseAsyncTask"))
        || Node.NodeClassPath.Contains(TEXT("K2Node_LatentGameplayTaskCall")))
    {
        Node.ExecutionSemantic = TEXT("async");
        Node.SemanticConfidence = TEXT("exact");
        Node.SemanticReason = TEXT("async_node_class");
        Node.ContinuationModel = TEXT("callback_exec_outputs");
        return;
    }

    const FString MemberLower = Node.MemberName.ToLower();
    if (MemberLower.Contains(TEXT("timer")))
    {
        Node.ExecutionSemantic = TEXT("timer");
        Node.SemanticConfidence = TEXT("inferred");
        Node.SemanticReason = TEXT("timer_member_name");
        Node.ContinuationModel = MemberLower.StartsWith(TEXT("settimer"))
            ? TEXT("scheduled_callback")
            : TEXT("timer_control");
        return;
    }

    static const TSet<FString> KnownLatentMembers = {
        TEXT("delay"),
        TEXT("retriggerabledelay"),
        TEXT("movecomponentto"),
        TEXT("loadstreamlevel"),
        TEXT("unloadstreamlevel"),
        TEXT("aimoveto")
    };
    if (KnownLatentMembers.Contains(MemberLower)
        || Node.NodeClassPath.Contains(TEXT("Latent"), ESearchCase::IgnoreCase))
    {
        Node.ExecutionSemantic = TEXT("latent");
        Node.SemanticConfidence = KnownLatentMembers.Contains(MemberLower) ? TEXT("inferred") : TEXT("exact");
        Node.SemanticReason = KnownLatentMembers.Contains(MemberLower) ? TEXT("known_latent_member") : TEXT("latent_node_class");
        Node.ContinuationModel = TEXT("deferred_exec_output");
    }
}

inline void AddUnresolvedTraceTransition(
    FTraceTraversalState& State,
    const FTraceNodeRecord& SourceNode,
    const FString& TransitionKind,
    const FString& Reason)
{
    for (const FTraceUnresolvedTransitionRecord& Existing : State.UnresolvedTransitions)
    {
        if (Existing.SourceBlueprintObjectPath == SourceNode.BlueprintObjectPath
            && Existing.SourceGraphName == SourceNode.GraphName
            && Existing.SourceNodeGuid == SourceNode.NodeGuid
            && Existing.TransitionKind == TransitionKind
            && Existing.Reason == Reason)
        {
            return;
        }
    }

    FTraceUnresolvedTransitionRecord& Record = State.UnresolvedTransitions.AddDefaulted_GetRef();
    Record.SourceBlueprintObjectPath = SourceNode.BlueprintObjectPath;
    Record.SourceGraphName = SourceNode.GraphName;
    Record.SourceNodeGuid = SourceNode.NodeGuid;
    Record.SourceNodeTitle = SourceNode.NodeTitle;
    Record.TransitionKind = TransitionKind;
    Record.Reason = Reason;
}

inline TSharedRef<FJsonObject> SerializeTraceNode(const FTraceNodeRecord& Node)
{
    TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
    NodeObject->SetStringField(TEXT("blueprintObjectPath"), Node.BlueprintObjectPath);
    NodeObject->SetStringField(TEXT("graphName"), Node.GraphName);
    NodeObject->SetStringField(TEXT("nodeGuid"), Node.NodeGuid);
    NodeObject->SetStringField(TEXT("nodeName"), Node.NodeName);
    NodeObject->SetStringField(TEXT("nodeClassPath"), Node.NodeClassPath);
    NodeObject->SetStringField(TEXT("nodeTitle"), Node.NodeTitle);
    NodeObject->SetStringField(TEXT("nodeType"), Node.NodeType);
    NodeObject->SetStringField(TEXT("memberName"), Node.MemberName);
    NodeObject->SetStringField(TEXT("memberParentPath"), Node.MemberParentPath);
    NodeObject->SetNumberField(TEXT("posX"), Node.PosX);
    NodeObject->SetNumberField(TEXT("posY"), Node.PosY);
    NodeObject->SetBoolField(TEXT("isPure"), Node.bIsPure);
    NodeObject->SetNumberField(TEXT("inputPinCount"), Node.InputPinCount);
    NodeObject->SetNumberField(TEXT("outputPinCount"), Node.OutputPinCount);
    NodeObject->SetBoolField(TEXT("isEntry"), Node.bIsEntry);
    NodeObject->SetStringField(TEXT("matchReason"), Node.MatchReason);
    NodeObject->SetNumberField(TEXT("depth"), Node.Depth);
    if (!Node.ExecutionSemantic.IsEmpty())
    {
        NodeObject->SetStringField(TEXT("executionSemantic"), Node.ExecutionSemantic);
        NodeObject->SetStringField(TEXT("semanticConfidence"), Node.SemanticConfidence);
        NodeObject->SetStringField(TEXT("semanticReason"), Node.SemanticReason);
        NodeObject->SetStringField(TEXT("continuationModel"), Node.ContinuationModel);
        if (!Node.CallbackMemberName.IsEmpty())
        {
            NodeObject->SetStringField(TEXT("callbackMemberName"), Node.CallbackMemberName);
        }
    }
    NodeObject->SetBoolField(TEXT("hasResolvedMemberAsset"), Node.bHasResolvedMemberAsset);
    NodeObject->SetBoolField(TEXT("isCrossBlueprintReference"), Node.bHasResolvedMemberAsset && !Node.ResolvedMemberAssetObjectPath.Equals(Node.BlueprintObjectPath, ESearchCase::CaseSensitive));
    if (Node.bHasResolvedMemberAsset)
    {
        TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
        AssetObject->SetStringField(TEXT("objectPath"), Node.ResolvedMemberAssetObjectPath);
        AssetObject->SetStringField(TEXT("assetName"), Node.ResolvedMemberAssetName);
        AssetObject->SetStringField(TEXT("packageName"), Node.ResolvedMemberAssetPackageName);
        AssetObject->SetStringField(TEXT("contentScope"), Node.ResolvedMemberAssetScope);
        AssetObject->SetBoolField(TEXT("isBlueprint"), Node.bResolvedMemberAssetIsBlueprint);
        NodeObject->SetObjectField(TEXT("resolvedMemberAsset"), AssetObject);
    }
    NodeObject->SetStringField(TEXT("connectedTargetClassPath"), Node.ConnectedTargetClassPath);
    NodeObject->SetBoolField(TEXT("hasResolvedTargetAsset"), Node.bHasResolvedTargetAsset);
    NodeObject->SetBoolField(
        TEXT("isConnectedTargetCrossBlueprint"),
        Node.bHasResolvedTargetAsset && !Node.ResolvedTargetAssetObjectPath.Equals(Node.BlueprintObjectPath, ESearchCase::CaseSensitive));
    if (Node.bHasResolvedTargetAsset)
    {
        TSharedRef<FJsonObject> TargetAssetObject = MakeShared<FJsonObject>();
        TargetAssetObject->SetStringField(TEXT("objectPath"), Node.ResolvedTargetAssetObjectPath);
        TargetAssetObject->SetStringField(TEXT("assetName"), Node.ResolvedTargetAssetName);
        NodeObject->SetObjectField(TEXT("resolvedTargetAsset"), TargetAssetObject);
    }
    return NodeObject;
}

inline TSharedRef<FJsonObject> SerializeTraceEdge(const FTraceEdgeRecord& Edge)
{
    TSharedRef<FJsonObject> EdgeObject = MakeShared<FJsonObject>();
    EdgeObject->SetStringField(TEXT("sourceBlueprintObjectPath"), Edge.SourceBlueprintObjectPath);
    EdgeObject->SetStringField(TEXT("sourceGraphName"), Edge.SourceGraphName);
    EdgeObject->SetStringField(TEXT("sourceNodeGuid"), Edge.SourceNodeGuid);
    EdgeObject->SetStringField(TEXT("sourcePinId"), Edge.SourcePinId);
    EdgeObject->SetStringField(TEXT("targetBlueprintObjectPath"), Edge.TargetBlueprintObjectPath);
    EdgeObject->SetStringField(TEXT("targetGraphName"), Edge.TargetGraphName);
    EdgeObject->SetStringField(TEXT("targetNodeGuid"), Edge.TargetNodeGuid);
    EdgeObject->SetStringField(TEXT("targetPinId"), Edge.TargetPinId);
    EdgeObject->SetStringField(TEXT("edgeKind"), Edge.EdgeKind);
    EdgeObject->SetStringField(TEXT("confidence"), Edge.Confidence);
    EdgeObject->SetStringField(TEXT("confidenceReason"), Edge.ConfidenceReason);
    return EdgeObject;
}

inline TSharedRef<FJsonObject> SerializeUnresolvedTraceTransition(const FTraceUnresolvedTransitionRecord& Transition)
{
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("sourceBlueprintObjectPath"), Transition.SourceBlueprintObjectPath);
    Object->SetStringField(TEXT("sourceGraphName"), Transition.SourceGraphName);
    Object->SetStringField(TEXT("sourceNodeGuid"), Transition.SourceNodeGuid);
    Object->SetStringField(TEXT("sourceNodeTitle"), Transition.SourceNodeTitle);
    Object->SetStringField(TEXT("transitionKind"), Transition.TransitionKind);
    Object->SetStringField(TEXT("reason"), Transition.Reason);
    Object->SetStringField(TEXT("confidence"), TEXT("unresolved"));
    return Object;
}

inline FString CalculateIndexedTraceConfidence(
    const TArray<FString>& StartNodeKeys,
    const FTraceTraversalState& State,
    const TArray<FTraceEdgeRecord>& TraversedEdges)
{
    if (StartNodeKeys.IsEmpty())
    {
        return TEXT("unresolved");
    }

    if (State.UnresolvedTransitions.Num() > 0)
    {
        return TEXT("unresolved");
    }

    for (const FString& StartKey : StartNodeKeys)
    {
        if (const FTraceNodeRecord* Node = State.NodesByKey.Find(StartKey))
        {
            if (!Node->MatchReason.IsEmpty()
                && Node->MatchReason != TEXT("entry_node")
                && Node->MatchReason != TEXT("node_title")
                && Node->MatchReason != TEXT("member_name"))
            {
                return TEXT("inferred");
            }
        }
    }

    for (const FTraceEdgeRecord& Edge : TraversedEdges)
    {
        if (Edge.Confidence == TEXT("inferred"))
        {
            return TEXT("inferred");
        }
    }

    for (const TPair<FString, FTraceNodeRecord>& Pair : State.NodesByKey)
    {
        if (Pair.Value.Depth != INDEX_NONE && Pair.Value.SemanticConfidence == TEXT("inferred"))
        {
            return TEXT("inferred");
        }
    }

    return TEXT("exact");
}

bool LoadBlueprintTraceData(
    FSQLiteDatabase& Database,
    const FString& ObjectPath,
    bool bResolveExternalMembers,
    FTraceTraversalState& State,
    FString& OutError);

bool ExpandBlueprintTraceTargets(
    FSQLiteDatabase& Database,
    const FTraceNodeRecord& CurrentNode,
    int32 CurrentDepth,
    int32 CurrentCallDepth,
    int32 MaxDepth,
    int32 MaxCallDepth,
    bool bFollowCrossBlueprintCalls,
    FTraceTraversalState& State,
    TSet<FString>& TraversedEdgeKeys,
    TArray<FTraceEdgeRecord>& TraversedEdges,
    TSet<FString>& VisitedNodeSet,
    TQueue<FTraceFrontierItem>& Frontier,
    FString& OutError);

TSharedRef<FJsonObject> BuildTraceFlowOutput(
    const FString& OutputMode,
    const FString& ObjectPath,
    const FString& GraphNameFilter,
    const FString& StartNodeQuery,
    bool bResolveExternalMembers,
    bool bExpandCalls,
    bool bFollowCrossBlueprintCalls,
    const FString& Direction,
    int32 MaxDepth,
    int32 MaxNodes,
    int32 MaxStartNodes,
    int32 MaxCallDepth,
    bool bMatchedStartNodes,
    bool bTruncated,
    int32 StartNodeCount,
    const TSharedPtr<FJsonObject>& BlueprintAsset,
    const FTraceTraversalState& State,
    const TArray<FString>& StartNodeKeys,
    const TArray<FString>& VisitedOrder,
    const TArray<FTraceEdgeRecord>& TraversedEdges);
