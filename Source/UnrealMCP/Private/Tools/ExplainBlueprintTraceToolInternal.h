#pragma once

#include "Containers/Queue.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "SQLiteDatabase.h"
#include "Tools/TraceBlueprintFlowToolInternal.h"

inline FString BuildExplainNodeLabel(const FTraceNodeRecord& Node)
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

inline void CollectExplainStartNodes(
    const FString& RootObjectPath,
    const FString& GraphNameFilter,
    const FString& StartNodeQuery,
    const FTraceTraversalState& State,
    int32 MaxStartNodes,
    TArray<FString>& OutStartNodeKeys,
    TMap<FString, FString>& OutStartReasonsByKey)
{
    if (!StartNodeQuery.IsEmpty())
    {
        for (const TPair<FString, FTraceNodeRecord>& Pair : State.NodesByKey)
        {
            const FTraceNodeRecord& Node = Pair.Value;
            if (!Node.BlueprintObjectPath.Equals(RootObjectPath, ESearchCase::CaseSensitive))
            {
                continue;
            }

            if (!GraphNameFilter.IsEmpty() && !Node.GraphName.Equals(GraphNameFilter, ESearchCase::CaseSensitive))
            {
                continue;
            }

            FString MatchReason;
            if (TraceStringMatchesQuery(Node.NodeTitle, StartNodeQuery))
            {
                MatchReason = TEXT("node_title");
            }
            else if (TraceStringMatchesQuery(Node.NodeName, StartNodeQuery))
            {
                MatchReason = TEXT("node_name");
            }
            else if (TraceStringMatchesQuery(Node.NodeType, StartNodeQuery))
            {
                MatchReason = TEXT("node_type");
            }
            else if (TraceStringMatchesQuery(Node.MemberName, StartNodeQuery))
            {
                MatchReason = TEXT("member_name");
            }
            else if (TraceStringMatchesQuery(Node.GraphName, StartNodeQuery))
            {
                MatchReason = TEXT("graph_name");
            }
            else if (const FString* PinMatchReason = State.PinMatchReasonsByNodeKey.Find(Pair.Key))
            {
                MatchReason = *PinMatchReason;
            }

            if (!MatchReason.IsEmpty())
            {
                OutStartNodeKeys.Add(Pair.Key);
                OutStartReasonsByKey.Add(Pair.Key, MatchReason);
            }
        }

        OutStartNodeKeys.Sort([&State](const FString& LeftKey, const FString& RightKey)
        {
            const FTraceNodeRecord& Left = State.NodesByKey[LeftKey];
            const FTraceNodeRecord& Right = State.NodesByKey[RightKey];
            if (Left.BlueprintObjectPath != Right.BlueprintObjectPath)
            {
                return Left.BlueprintObjectPath < Right.BlueprintObjectPath;
            }

            return Left.GraphName == Right.GraphName
                ? Left.NodeTitle < Right.NodeTitle
                : Left.GraphName < Right.GraphName;
        });
    }
    else
    {
        for (const FGraphEntryRecord& Entry : State.GraphEntries)
        {
            if (!Entry.BlueprintObjectPath.Equals(RootObjectPath, ESearchCase::CaseSensitive))
            {
                continue;
            }

            if (!GraphNameFilter.IsEmpty() && !Entry.GraphName.Equals(GraphNameFilter, ESearchCase::CaseSensitive))
            {
                continue;
            }

            const FString EntryKey = MakeTraceNodeKey(RootObjectPath, Entry.GraphName, Entry.EntryNodeGuid);
            if (State.NodesByKey.Contains(EntryKey))
            {
                OutStartNodeKeys.Add(EntryKey);
                OutStartReasonsByKey.Add(EntryKey, TEXT("entry_node"));
            }
        }
    }

    if (OutStartNodeKeys.Num() > MaxStartNodes)
    {
        OutStartNodeKeys.SetNum(MaxStartNodes);
    }
}

inline FString BuildTraceConfidence(
    const TArray<FString>& StartNodeKeys,
    const FTraceTraversalState& State,
    const TArray<FTraceEdgeRecord>& TraversedEdges)
{
    return CalculateIndexedTraceConfidence(StartNodeKeys, State, TraversedEdges);
}

inline FString BuildExplanationText(const FTraceTraversalState& State, const TArray<FString>& VisitedOrder, const FString& Confidence)
{
    if (VisitedOrder.Num() == 0)
    {
        return TEXT("No Blueprint flow steps were found for the requested trace.");
    }

    TArray<FString> Segments;
    const int32 Limit = FMath::Min(VisitedOrder.Num(), 6);
    for (int32 Index = 0; Index < Limit; ++Index)
    {
        const FTraceNodeRecord* Node = State.NodesByKey.Find(VisitedOrder[Index]);
        if (Node == nullptr)
        {
            continue;
        }

        FString Segment = BuildExplainNodeLabel(*Node);
        const bool bIsCrossBlueprint = Node->bHasResolvedMemberAsset
            && !Node->ResolvedMemberAssetObjectPath.IsEmpty()
            && !Node->ResolvedMemberAssetObjectPath.Equals(Node->BlueprintObjectPath, ESearchCase::CaseSensitive);
        if (bIsCrossBlueprint)
        {
            Segment += FString::Printf(TEXT(" calls into %s"), *Node->ResolvedMemberAssetObjectPath);
        }
        Segments.Add(Segment);
    }

    FString Prefix;
    if (Confidence == TEXT("exact"))
    {
        Prefix = TEXT("The indexed trace is strong and directly matched the requested Blueprint flow.");
    }
    else if (Confidence == TEXT("inferred"))
    {
        Prefix = TEXT("The indexed trace is partly inferred and should be read as a guided explanation rather than a guaranteed full execution path.");
    }
    else
    {
        Prefix = TEXT("The indexed trace reached one or more unresolved transitions; the reported path is incomplete at those boundaries.");
    }

    return Prefix + TEXT(" Flow summary: ") + FString::Join(Segments, TEXT(" -> "));
}
