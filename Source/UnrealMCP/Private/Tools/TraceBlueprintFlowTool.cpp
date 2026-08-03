#include "Tools/TraceBlueprintFlowTool.h"

#include "Containers/Queue.h"
#include "SQLitePreparedStatement.h"
#include "Tools/IndexedQueryToolUtils.h"
#include "Tools/TraceBlueprintFlowToolInternal.h"

namespace
{
    void CollectStartNodes(
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

    bool ExpandForwardCallTargets(
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
        FString& OutError)
    {
        if (CurrentNode.NodeType != TEXT("call_function")
            || CurrentNode.MemberName.IsEmpty()
            || CurrentDepth >= MaxDepth
            || CurrentCallDepth >= MaxCallDepth
            || !CurrentNode.bHasResolvedMemberAsset
            || !CurrentNode.bResolvedMemberAssetIsBlueprint
            || CurrentNode.ResolvedMemberAssetObjectPath.IsEmpty())
        {
            return true;
        }

        const bool bIsCrossBlueprintCall = !CurrentNode.ResolvedMemberAssetObjectPath.Equals(CurrentNode.BlueprintObjectPath, ESearchCase::CaseSensitive);
        if (bIsCrossBlueprintCall && !bFollowCrossBlueprintCalls)
        {
            return true;
        }

        if (!LoadBlueprintTraceData(Database, CurrentNode.ResolvedMemberAssetObjectPath, true, State, OutError))
        {
            return false;
        }

        const FString MemberLookupKey = MakeTraceMemberLookupKey(CurrentNode.ResolvedMemberAssetObjectPath, CurrentNode.MemberName);
        const TArray<FString>* CandidateEntryKeys = State.EntryNodeKeysByAssetAndMember.Find(MemberLookupKey);
        if (CandidateEntryKeys == nullptr)
        {
            return true;
        }

        const FString SourceNodeKey = MakeTraceNodeKey(CurrentNode.BlueprintObjectPath, CurrentNode.GraphName, CurrentNode.NodeGuid);
        for (const FString& TargetNodeKey : *CandidateEntryKeys)
        {
            const FTraceNodeRecord* TargetNode = State.NodesByKey.Find(TargetNodeKey);
            if (TargetNode == nullptr)
            {
                continue;
            }

            FTraceEdgeRecord CallEdge;
            CallEdge.SourceBlueprintObjectPath = CurrentNode.BlueprintObjectPath;
            CallEdge.SourceGraphName = CurrentNode.GraphName;
            CallEdge.SourceNodeGuid = CurrentNode.NodeGuid;
            CallEdge.TargetBlueprintObjectPath = TargetNode->BlueprintObjectPath;
            CallEdge.TargetGraphName = TargetNode->GraphName;
            CallEdge.TargetNodeGuid = TargetNode->NodeGuid;
            CallEdge.EdgeKind = TEXT("call");

            const FString EdgeKey = FString::Printf(TEXT("%s|%s|%s|%s|%s|%s|%s|%s|%s"),
                *CallEdge.SourceBlueprintObjectPath,
                *CallEdge.SourceGraphName,
                *CallEdge.SourceNodeGuid,
                *CallEdge.SourcePinId,
                *CallEdge.TargetBlueprintObjectPath,
                *CallEdge.TargetGraphName,
                *CallEdge.TargetNodeGuid,
                *CallEdge.TargetPinId,
                *CallEdge.EdgeKind);

            if (!TraversedEdgeKeys.Contains(EdgeKey))
            {
                TraversedEdgeKeys.Add(EdgeKey);
                TraversedEdges.Add(CallEdge);
            }

            if (VisitedNodeSet.Contains(TargetNodeKey))
            {
                continue;
            }

            if (FTraceNodeRecord* MutableTargetNode = State.NodesByKey.Find(TargetNodeKey))
            {
                if (MutableTargetNode->Depth == INDEX_NONE || (CurrentDepth + 1) < MutableTargetNode->Depth)
                {
                    MutableTargetNode->Depth = CurrentDepth + 1;
                }
            }

            Frontier.Enqueue(FTraceFrontierItem
            {
                TargetNodeKey,
                CurrentDepth + 1,
                CurrentCallDepth + 1
            });
        }

        return true;
    }
}

FTraceBlueprintFlowTool::FTraceBlueprintFlowTool()
    : FMCPToolBase(TEXT("TraceBlueprintFlow"), TEXT("Traces indexed Blueprint graph flow from matching nodes or graph entry nodes using the local UnrealMCP graph index."))
{
}

UnrealMCP::FMCPResponse FTraceBlueprintFlowTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FString ObjectPath = GetTraceOptionalString(Request.Params, TEXT("objectPath"));
    if (ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("TraceBlueprintFlow requires params.objectPath for a Blueprint asset."));
    }

    const FString GraphNameFilter = GetTraceOptionalString(Request.Params, TEXT("graphName"));
    const FString StartNodeQuery = GetTraceOptionalString(Request.Params, TEXT("startNodeQuery"));
    FString OutputMode = GetTraceOptionalString(Request.Params, TEXT("outputMode"));
    if (OutputMode.IsEmpty())
    {
        OutputMode = TEXT("summary");
    }

    bool bResolveExternalMembers = true;
    bool bExpandCalls = true;
    bool bFollowCrossBlueprintCalls = true;
    if (Request.Params.IsValid())
    {
        Request.Params->TryGetBoolField(TEXT("resolveExternalMembers"), bResolveExternalMembers);
        Request.Params->TryGetBoolField(TEXT("expandCalls"), bExpandCalls);
        Request.Params->TryGetBoolField(TEXT("followCrossBlueprintCalls"), bFollowCrossBlueprintCalls);
    }

    FString Direction = GetTraceOptionalString(Request.Params, TEXT("direction"));
    if (Direction.IsEmpty())
    {
        Direction = TEXT("forward");
    }

    if (Direction != TEXT("forward") && Direction != TEXT("backward") && Direction != TEXT("both"))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("TraceBlueprintFlow params.direction must be forward, backward, or both."));
    }

    if (OutputMode != TEXT("summary") && OutputMode != TEXT("compact") && OutputMode != TEXT("full"))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("TraceBlueprintFlow params.outputMode must be summary, compact, or full."));
    }

    const int32 MaxDepth = GetTraceOptionalClampedInt(Request.Params, TEXT("maxDepth"), 4, 0, 12);
    const int32 MaxNodes = GetTraceOptionalClampedInt(Request.Params, TEXT("maxNodes"), 40, 1, 200);
    const int32 MaxStartNodes = GetTraceOptionalClampedInt(Request.Params, TEXT("maxStartNodes"), 8, 1, 32);
    const int32 MaxCallDepth = GetTraceOptionalClampedInt(Request.Params, TEXT("maxCallDepth"), 1, 0, 6);

    FTraceTraversalState State;
    TArray<FString> StartNodeKeysFinal;
    TArray<FString> VisitedOrderFinal;
    TArray<FTraceEdgeRecord> TraversedEdgesFinal;
    int32 StartNodeCount = 0;
    bool bMatchedStartNodes = false;
    bool bTruncated = false;

    FString Error;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            if (!LoadBlueprintTraceData(Database, ObjectPath, bResolveExternalMembers, State, OutExecError))
            {
                return false;
            }

            if (!StartNodeQuery.IsEmpty())
            {
                FString Sql = TEXT("SELECT DISTINCT graph_name, node_guid, pin_name FROM blueprint_pins WHERE blueprint_object_path = ?1 AND pin_name LIKE ?2");
                if (!GraphNameFilter.IsEmpty())
                {
                    Sql += TEXT(" AND graph_name = ?3");
                }
                Sql += TEXT(" ORDER BY graph_name ASC, node_guid ASC;");

                FSQLitePreparedStatement Statement(Database, *Sql, ESQLitePreparedStatementFlags::None);
                const FString LikeValue = FString::Printf(TEXT("%%%s%%"), *StartNodeQuery);
                if (!Statement.IsValid()
                    || !Statement.SetBindingValueByIndex(1, ObjectPath)
                    || !Statement.SetBindingValueByIndex(2, LikeValue)
                    || (!GraphNameFilter.IsEmpty() && !Statement.SetBindingValueByIndex(3, GraphNameFilter)))
                {
                    OutExecError = TEXT("TraceBlueprintFlow could not prepare the Blueprint pin-match query.");
                    return false;
                }

                const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
                {
                    FString GraphName;
                    FString NodeGuid;
                    FString PinName;
                    if (!Row.GetColumnValueByIndex(0, GraphName)
                        || !Row.GetColumnValueByIndex(1, NodeGuid)
                        || !Row.GetColumnValueByIndex(2, PinName))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }

                    State.PinMatchReasonsByNodeKey.Add(
                        MakeTraceNodeKey(ObjectPath, GraphName, NodeGuid),
                        FString::Printf(TEXT("pin_name:%s"), *PinName));
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                });

                if (QueryResult == INDEX_NONE)
                {
                    OutExecError = Database.GetLastError().IsEmpty() ? TEXT("TraceBlueprintFlow pin-match query failed.") : Database.GetLastError();
                    return false;
                }
            }

            TArray<FString> StartNodeKeys;
            TMap<FString, FString> StartReasonsByKey;
            CollectStartNodes(ObjectPath, GraphNameFilter, StartNodeQuery, State, MaxStartNodes, StartNodeKeys, StartReasonsByKey);

            TQueue<FTraceFrontierItem> Frontier;
            TArray<FString> VisitedOrder;
            TSet<FString> VisitedNodeSet;
            TSet<FString> TraversedEdgeKeys;
            TArray<FTraceEdgeRecord> TraversedEdges;

            for (const FString& StartKey : StartNodeKeys)
            {
                if (FTraceNodeRecord* Node = State.NodesByKey.Find(StartKey))
                {
                    Node->Depth = 0;
                    Node->MatchReason = StartReasonsByKey.FindRef(StartKey);
                    Frontier.Enqueue(FTraceFrontierItem{ StartKey, 0, 0 });
                }
            }

            while (!Frontier.IsEmpty())
            {
                FTraceFrontierItem Current;
                Frontier.Dequeue(Current);

                if (VisitedNodeSet.Contains(Current.NodeKey))
                {
                    continue;
                }

                FTraceNodeRecord* CurrentNode = State.NodesByKey.Find(Current.NodeKey);
                if (CurrentNode == nullptr)
                {
                    continue;
                }

                VisitedNodeSet.Add(Current.NodeKey);
                VisitedOrder.Add(Current.NodeKey);
                if (VisitedOrder.Num() >= MaxNodes)
                {
                    bTruncated = !Frontier.IsEmpty();
                    break;
                }

                auto VisitEdges = [&](const TMultiMap<FString, FTraceEdgeRecord>& EdgeMap, bool bUseTargetNode)
                {
                    TArray<FTraceEdgeRecord> ConnectedEdges;
                    EdgeMap.MultiFind(Current.NodeKey, ConnectedEdges);
                    for (const FTraceEdgeRecord& Edge : ConnectedEdges)
                    {
                        const FString NextKey = bUseTargetNode
                            ? MakeTraceNodeKey(Edge.TargetBlueprintObjectPath, Edge.TargetGraphName, Edge.TargetNodeGuid)
                            : MakeTraceNodeKey(Edge.SourceBlueprintObjectPath, Edge.SourceGraphName, Edge.SourceNodeGuid);

                        const FString EdgeKey = FString::Printf(TEXT("%s|%s|%s|%s|%s|%s|%s|%s|%s"),
                            *Edge.SourceBlueprintObjectPath,
                            *Edge.SourceGraphName,
                            *Edge.SourceNodeGuid,
                            *Edge.SourcePinId,
                            *Edge.TargetBlueprintObjectPath,
                            *Edge.TargetGraphName,
                            *Edge.TargetNodeGuid,
                            *Edge.TargetPinId,
                            *Edge.EdgeKind);

                        if (!TraversedEdgeKeys.Contains(EdgeKey))
                        {
                            TraversedEdgeKeys.Add(EdgeKey);
                            TraversedEdges.Add(Edge);
                        }

                        if (Current.Depth >= MaxDepth || VisitedNodeSet.Contains(NextKey))
                        {
                            continue;
                        }

                        if (FTraceNodeRecord* NextNode = State.NodesByKey.Find(NextKey))
                        {
                            if (NextNode->Depth == INDEX_NONE || (Current.Depth + 1) < NextNode->Depth)
                            {
                                NextNode->Depth = Current.Depth + 1;
                            }
                        }

                        Frontier.Enqueue(FTraceFrontierItem{ NextKey, Current.Depth + 1, Current.CallDepth });
                    }
                };

                if (Direction == TEXT("forward") || Direction == TEXT("both"))
                {
                    VisitEdges(State.OutgoingEdgesByNode, true);
                }
                if (Direction == TEXT("backward") || Direction == TEXT("both"))
                {
                    VisitEdges(State.IncomingEdgesByNode, false);
                }

                if ((Direction == TEXT("forward") || Direction == TEXT("both"))
                    && bExpandCalls
                    && !ExpandForwardCallTargets(
                        Database,
                        *CurrentNode,
                        Current.Depth,
                        Current.CallDepth,
                        MaxDepth,
                        MaxCallDepth,
                        bFollowCrossBlueprintCalls,
                        State,
                        TraversedEdgeKeys,
                        TraversedEdges,
                        VisitedNodeSet,
                        Frontier,
                        OutExecError))
                {
                    return false;
                }
            }

            StartNodeCount = StartNodeKeys.Num();
            bMatchedStartNodes = StartNodeCount > 0;
            StartNodeKeysFinal = StartNodeKeys;
            VisitedOrderFinal = VisitedOrder;
            TraversedEdgesFinal = TraversedEdges;
            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("TraceBlueprintFlow failed to query the project index: %s"), *Error));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedPtr<FJsonObject> BlueprintAsset = State.AssetsByObjectPath.FindRef(ObjectPath);
    Response.Result = BuildTraceFlowOutput(
        OutputMode,
        ObjectPath,
        GraphNameFilter,
        StartNodeQuery,
        bResolveExternalMembers,
        bExpandCalls,
        bFollowCrossBlueprintCalls,
        Direction,
        MaxDepth,
        MaxNodes,
        MaxStartNodes,
        MaxCallDepth,
        bMatchedStartNodes,
        bTruncated,
        StartNodeCount,
        BlueprintAsset,
        State,
        StartNodeKeysFinal,
        VisitedOrderFinal,
        TraversedEdgesFinal);
    return Response;
}

TSharedPtr<FJsonObject> FTraceBlueprintFlowTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Blueprint asset object path, for example /Game/MyFolder/BP_Door.BP_Door."));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);

    TSharedRef<FJsonObject> GraphNameProperty = MakeShared<FJsonObject>();
    GraphNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    GraphNameProperty->SetStringField(TEXT("description"), TEXT("Optional Blueprint graph name filter, such as EventGraph or a function graph name."));
    Properties->SetObjectField(TEXT("graphName"), GraphNameProperty);

    TSharedRef<FJsonObject> StartNodeQueryProperty = MakeShared<FJsonObject>();
    StartNodeQueryProperty->SetStringField(TEXT("type"), TEXT("string"));
    StartNodeQueryProperty->SetStringField(TEXT("description"), TEXT("Optional node, event, function, variable, graph, or pin text to choose start nodes. When omitted, graph entry nodes are used."));
    Properties->SetObjectField(TEXT("startNodeQuery"), StartNodeQueryProperty);

    TSharedRef<FJsonObject> OutputModeProperty = MakeShared<FJsonObject>();
    OutputModeProperty->SetStringField(TEXT("type"), TEXT("string"));
    OutputModeProperty->SetStringField(TEXT("description"), TEXT("Optional response mode: summary, compact, or full. Defaults to summary."));
    Properties->SetObjectField(TEXT("outputMode"), OutputModeProperty);

    TSharedRef<FJsonObject> ResolveExternalMembersProperty = MakeShared<FJsonObject>();
    ResolveExternalMembersProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    ResolveExternalMembersProperty->SetStringField(TEXT("description"), TEXT("Optional. When true, node member owners are resolved back to indexed assets so cross-Blueprint references are visible in the trace output. Defaults to true."));
    Properties->SetObjectField(TEXT("resolveExternalMembers"), ResolveExternalMembersProperty);

    TSharedRef<FJsonObject> ExpandCallsProperty = MakeShared<FJsonObject>();
    ExpandCallsProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    ExpandCallsProperty->SetStringField(TEXT("description"), TEXT("Optional. When true, the trace may enter called functions or custom events using a bounded call-expansion budget. Defaults to true."));
    Properties->SetObjectField(TEXT("expandCalls"), ExpandCallsProperty);

    TSharedRef<FJsonObject> FollowCrossBlueprintCallsProperty = MakeShared<FJsonObject>();
    FollowCrossBlueprintCallsProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    FollowCrossBlueprintCallsProperty->SetStringField(TEXT("description"), TEXT("Optional. When true, bounded call expansion may continue into target Blueprints as well. Defaults to true."));
    Properties->SetObjectField(TEXT("followCrossBlueprintCalls"), FollowCrossBlueprintCallsProperty);

    TSharedRef<FJsonObject> DirectionProperty = MakeShared<FJsonObject>();
    DirectionProperty->SetStringField(TEXT("type"), TEXT("string"));
    DirectionProperty->SetStringField(TEXT("description"), TEXT("Traversal direction: forward, backward, or both. Defaults to forward."));
    Properties->SetObjectField(TEXT("direction"), DirectionProperty);

    TSharedRef<FJsonObject> MaxDepthProperty = MakeShared<FJsonObject>();
    MaxDepthProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxDepthProperty->SetStringField(TEXT("description"), TEXT("Optional max traversal depth across graph edges. Defaults to 4, clamped to 0-12."));
    Properties->SetObjectField(TEXT("maxDepth"), MaxDepthProperty);

    TSharedRef<FJsonObject> MaxNodesProperty = MakeShared<FJsonObject>();
    MaxNodesProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxNodesProperty->SetStringField(TEXT("description"), TEXT("Optional max traced node count. Defaults to 40, clamped to 1-200."));
    Properties->SetObjectField(TEXT("maxNodes"), MaxNodesProperty);

    TSharedRef<FJsonObject> MaxStartNodesProperty = MakeShared<FJsonObject>();
    MaxStartNodesProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxStartNodesProperty->SetStringField(TEXT("description"), TEXT("Optional max number of matched start nodes to seed the trace. Defaults to 8, clamped to 1-32."));
    Properties->SetObjectField(TEXT("maxStartNodes"), MaxStartNodesProperty);

    TSharedRef<FJsonObject> MaxCallDepthProperty = MakeShared<FJsonObject>();
    MaxCallDepthProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxCallDepthProperty->SetStringField(TEXT("description"), TEXT("Optional max number of function or custom-event call expansions. Defaults to 1, clamped to 0-6."));
    Properties->SetObjectField(TEXT("maxCallDepth"), MaxCallDepthProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
