#include "Tools/TraceBlueprintFlowToolInternal.h"

namespace
{
    FString BuildNodeLabel(const FTraceNodeRecord& Node)
    {
        if (!Node.NodeTitle.IsEmpty())
        {
            return Node.NodeTitle;
        }

        if (!Node.MemberName.IsEmpty())
        {
            return Node.MemberName;
        }

        return Node.NodeName;
    }

    TSharedRef<FJsonObject> BuildCompactNodeObject(const FTraceNodeRecord& Node)
    {
        TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
        NodeObject->SetStringField(TEXT("blueprintObjectPath"), Node.BlueprintObjectPath);
        NodeObject->SetStringField(TEXT("graphName"), Node.GraphName);
        NodeObject->SetStringField(TEXT("nodeTitle"), BuildNodeLabel(Node));
        NodeObject->SetStringField(TEXT("nodeType"), Node.NodeType);
        NodeObject->SetStringField(TEXT("memberName"), Node.MemberName);
        NodeObject->SetNumberField(TEXT("depth"), Node.Depth);
        NodeObject->SetBoolField(TEXT("isEntry"), Node.bIsEntry);

        const bool bIsCrossBlueprintReference = Node.bHasResolvedMemberAsset
            && !Node.ResolvedMemberAssetObjectPath.Equals(Node.BlueprintObjectPath, ESearchCase::CaseSensitive);
        NodeObject->SetBoolField(TEXT("isCrossBlueprintReference"), bIsCrossBlueprintReference);
        if (bIsCrossBlueprintReference)
        {
            NodeObject->SetStringField(TEXT("targetBlueprintObjectPath"), Node.ResolvedMemberAssetObjectPath);
        }

        return NodeObject;
    }

    TArray<TSharedPtr<FJsonValue>> BuildCompactNodes(const FTraceTraversalState& State, const TArray<FString>& NodeKeys)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (const FString& NodeKey : NodeKeys)
        {
            if (const FTraceNodeRecord* Node = State.NodesByKey.Find(NodeKey))
            {
                Result.Add(MakeShared<FJsonValueObject>(BuildCompactNodeObject(*Node)));
            }
        }
        return Result;
    }

    TArray<TSharedPtr<FJsonValue>> BuildCompactEdges(const TArray<FTraceEdgeRecord>& TraversedEdges)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (const FTraceEdgeRecord& Edge : TraversedEdges)
        {
            TSharedRef<FJsonObject> EdgeObject = MakeShared<FJsonObject>();
            EdgeObject->SetStringField(TEXT("sourceBlueprintObjectPath"), Edge.SourceBlueprintObjectPath);
            EdgeObject->SetStringField(TEXT("sourceGraphName"), Edge.SourceGraphName);
            EdgeObject->SetStringField(TEXT("targetBlueprintObjectPath"), Edge.TargetBlueprintObjectPath);
            EdgeObject->SetStringField(TEXT("targetGraphName"), Edge.TargetGraphName);
            EdgeObject->SetStringField(TEXT("edgeKind"), Edge.EdgeKind);
            Result.Add(MakeShared<FJsonValueObject>(EdgeObject));
        }
        return Result;
    }

    TArray<TSharedPtr<FJsonValue>> BuildCrossBlueprintCalls(const FTraceTraversalState& State, const TArray<FString>& VisitedOrder)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        TSet<FString> SeenKeys;

        for (const FString& NodeKey : VisitedOrder)
        {
            const FTraceNodeRecord* Node = State.NodesByKey.Find(NodeKey);
            if (Node == nullptr
                || !Node->bHasResolvedMemberAsset
                || Node->ResolvedMemberAssetObjectPath.IsEmpty()
                || Node->ResolvedMemberAssetObjectPath.Equals(Node->BlueprintObjectPath, ESearchCase::CaseSensitive))
            {
                continue;
            }

            const FString SeenKey = Node->BlueprintObjectPath + TEXT("|") + Node->MemberName + TEXT("|") + Node->ResolvedMemberAssetObjectPath;
            if (SeenKeys.Contains(SeenKey))
            {
                continue;
            }
            SeenKeys.Add(SeenKey);

            TSharedRef<FJsonObject> CallObject = MakeShared<FJsonObject>();
            CallObject->SetStringField(TEXT("sourceBlueprintObjectPath"), Node->BlueprintObjectPath);
            CallObject->SetStringField(TEXT("sourceGraphName"), Node->GraphName);
            CallObject->SetStringField(TEXT("sourceNodeTitle"), BuildNodeLabel(*Node));
            CallObject->SetStringField(TEXT("memberName"), Node->MemberName);
            CallObject->SetStringField(TEXT("targetBlueprintObjectPath"), Node->ResolvedMemberAssetObjectPath);
            Result.Add(MakeShared<FJsonValueObject>(CallObject));
        }

        return Result;
    }

    TArray<TSharedPtr<FJsonValue>> BuildSummarySteps(const FTraceTraversalState& State, const TArray<FString>& VisitedOrder)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (const FString& NodeKey : VisitedOrder)
        {
            const FTraceNodeRecord* Node = State.NodesByKey.Find(NodeKey);
            if (Node == nullptr)
            {
                continue;
            }

            FString Step = BuildNodeLabel(*Node);
            if (Node->bHasResolvedMemberAsset
                && !Node->ResolvedMemberAssetObjectPath.IsEmpty()
                && !Node->ResolvedMemberAssetObjectPath.Equals(Node->BlueprintObjectPath, ESearchCase::CaseSensitive))
            {
                Step += FString::Printf(TEXT(" -> %s"), *Node->ResolvedMemberAssetObjectPath);
            }

            Result.Add(MakeShared<FJsonValueString>(Step));
        }
        return Result;
    }

    FString BuildSummaryText(const FTraceTraversalState& State, const TArray<FString>& VisitedOrder)
    {
        TArray<FString> Labels;
        const int32 Limit = FMath::Min(VisitedOrder.Num(), 8);
        for (int32 Index = 0; Index < Limit; ++Index)
        {
            const FTraceNodeRecord* Node = State.NodesByKey.Find(VisitedOrder[Index]);
            if (Node == nullptr)
            {
                continue;
            }

            FString Label = BuildNodeLabel(*Node);
            if (Node->bHasResolvedMemberAsset
                && !Node->ResolvedMemberAssetObjectPath.IsEmpty()
                && !Node->ResolvedMemberAssetObjectPath.Equals(Node->BlueprintObjectPath, ESearchCase::CaseSensitive))
            {
                Label += FString::Printf(TEXT(" -> %s"), *Node->ResolvedMemberAssetObjectPath);
            }
            Labels.Add(Label);
        }

        return Labels.Num() > 0
            ? FString::Join(Labels, TEXT(" -> "))
            : TEXT("No trace steps were collected.");
    }
}

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
    const TArray<FTraceEdgeRecord>& TraversedEdges)
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("graphName"), GraphNameFilter);
    Result->SetStringField(TEXT("startNodeQuery"), StartNodeQuery);
    Result->SetStringField(TEXT("outputMode"), OutputMode);
    Result->SetBoolField(TEXT("resolveExternalMembers"), bResolveExternalMembers);
    Result->SetBoolField(TEXT("expandCalls"), bExpandCalls);
    Result->SetBoolField(TEXT("followCrossBlueprintCalls"), bFollowCrossBlueprintCalls);
    Result->SetStringField(TEXT("direction"), Direction);
    Result->SetNumberField(TEXT("maxDepth"), MaxDepth);
    Result->SetNumberField(TEXT("maxNodes"), MaxNodes);
    Result->SetNumberField(TEXT("maxStartNodes"), MaxStartNodes);
    Result->SetNumberField(TEXT("maxCallDepth"), MaxCallDepth);
    Result->SetBoolField(TEXT("matchedStartNodes"), bMatchedStartNodes);
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    Result->SetNumberField(TEXT("startNodeCount"), StartNodeCount);
    Result->SetNumberField(TEXT("tracedNodeCount"), VisitedOrder.Num());
    Result->SetNumberField(TEXT("tracedEdgeCount"), TraversedEdges.Num());
    if (BlueprintAsset.IsValid())
    {
        Result->SetObjectField(TEXT("asset"), BlueprintAsset.ToSharedRef());
    }

    if (OutputMode.Equals(TEXT("full"), ESearchCase::IgnoreCase))
    {
        TArray<TSharedPtr<FJsonValue>> GraphEntriesJson;
        for (const FGraphEntryRecord& Entry : State.GraphEntries)
        {
            TSharedRef<FJsonObject> EntryObject = MakeShared<FJsonObject>();
            EntryObject->SetStringField(TEXT("blueprintObjectPath"), Entry.BlueprintObjectPath);
            EntryObject->SetStringField(TEXT("graphName"), Entry.GraphName);
            EntryObject->SetStringField(TEXT("entryNodeGuid"), Entry.EntryNodeGuid);
            EntryObject->SetStringField(TEXT("graphType"), Entry.GraphType);
            EntryObject->SetNumberField(TEXT("nodeCount"), Entry.NodeCount);
            GraphEntriesJson.Add(MakeShared<FJsonValueObject>(EntryObject));
        }

        Result->SetArrayField(TEXT("graphEntries"), GraphEntriesJson);
        Result->SetArrayField(TEXT("startNodes"), BuildCompactNodes(State, StartNodeKeys));

        TArray<TSharedPtr<FJsonValue>> FullTraceNodes;
        for (const FString& NodeKey : VisitedOrder)
        {
            if (const FTraceNodeRecord* Node = State.NodesByKey.Find(NodeKey))
            {
                FullTraceNodes.Add(MakeShared<FJsonValueObject>(SerializeTraceNode(*Node)));
            }
        }
        Result->SetArrayField(TEXT("traceNodes"), FullTraceNodes);

        TArray<TSharedPtr<FJsonValue>> FullTraceEdges;
        for (const FTraceEdgeRecord& Edge : TraversedEdges)
        {
            FullTraceEdges.Add(MakeShared<FJsonValueObject>(SerializeTraceEdge(Edge)));
        }
        Result->SetArrayField(TEXT("traceEdges"), FullTraceEdges);
        Result->SetStringField(TEXT("nextStepHint"), TEXT("Use outputMode=summary for a smaller response or compact for a smaller structured payload."));
        return Result;
    }

    Result->SetArrayField(TEXT("startNodes"), BuildCompactNodes(State, StartNodeKeys));
    Result->SetArrayField(TEXT("crossBlueprintCalls"), BuildCrossBlueprintCalls(State, VisitedOrder));

    if (OutputMode.Equals(TEXT("compact"), ESearchCase::IgnoreCase))
    {
        Result->SetArrayField(TEXT("traceNodes"), BuildCompactNodes(State, VisitedOrder));
        Result->SetArrayField(TEXT("traceEdges"), BuildCompactEdges(TraversedEdges));
        Result->SetStringField(TEXT("nextStepHint"), TEXT("Use outputMode=full only when you need the full node and edge details."));
        return Result;
    }

    Result->SetStringField(TEXT("summaryText"), BuildSummaryText(State, VisitedOrder));
    Result->SetArrayField(TEXT("summarySteps"), BuildSummarySteps(State, VisitedOrder));
    Result->SetStringField(TEXT("nextStepHint"), TEXT("Use outputMode=compact or full if you need more structure than this summary."));
    return Result;
}
