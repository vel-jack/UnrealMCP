#include "Tools/ExplainBlueprintTraceTool.h"

#include "Containers/Queue.h"
#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/ExplainBlueprintTraceToolInternal.h"
#include "Tools/IndexedQueryToolUtils.h"
#include "Tools/TraceBlueprintFlowToolInternal.h"

FExplainBlueprintTraceTool::FExplainBlueprintTraceTool()
    : FMCPToolBase(TEXT("ExplainBlueprintTrace"), TEXT("Explains an indexed Blueprint trace in a higher-level, token-efficient summary with confidence markers and cross-Blueprint notes."))
{
}

UnrealMCP::FMCPResponse FExplainBlueprintTraceTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FString ObjectPath = GetTraceOptionalString(Request.Params, TEXT("objectPath"));
    if (ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ExplainBlueprintTrace requires params.objectPath for a Blueprint asset."));
    }

    const FString GraphNameFilter = GetTraceOptionalString(Request.Params, TEXT("graphName"));
    const FString StartNodeQuery = GetTraceOptionalString(Request.Params, TEXT("startNodeQuery"));
    FString Direction = GetTraceOptionalString(Request.Params, TEXT("direction"));
    if (Direction.IsEmpty())
    {
        Direction = TEXT("forward");
    }

    if (Direction != TEXT("forward") && Direction != TEXT("backward") && Direction != TEXT("both"))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ExplainBlueprintTrace params.direction must be forward, backward, or both."));
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

    const int32 MaxDepth = GetTraceOptionalClampedInt(Request.Params, TEXT("maxDepth"), 4, 0, 12);
    const int32 MaxNodes = GetTraceOptionalClampedInt(Request.Params, TEXT("maxNodes"), 32, 1, 200);
    const int32 MaxStartNodes = GetTraceOptionalClampedInt(Request.Params, TEXT("maxStartNodes"), 6, 1, 32);
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
                    OutExecError = TEXT("ExplainBlueprintTrace could not prepare the Blueprint pin-match query.");
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
                    OutExecError = Database.GetLastError().IsEmpty() ? TEXT("ExplainBlueprintTrace pin-match query failed.") : Database.GetLastError();
                    return false;
                }
            }

            TArray<FString> StartNodeKeys;
            TMap<FString, FString> StartReasonsByKey;
            CollectExplainStartNodes(ObjectPath, GraphNameFilter, StartNodeQuery, State, MaxStartNodes, StartNodeKeys, StartReasonsByKey);

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

                auto VisitEdges = [&](const TMultiMap<FString, FTraceEdgeRecord>& EdgeMap, bool bUseTargetNode, bool bDataOnly = false)
                {
                    TArray<FTraceEdgeRecord> ConnectedEdges;
                    EdgeMap.MultiFind(Current.NodeKey, ConnectedEdges);
                    for (const FTraceEdgeRecord& Edge : ConnectedEdges)
                    {
                        if (bDataOnly && Edge.EdgeKind != TEXT("data"))
                        {
                            continue;
                        }

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
                    VisitEdges(State.IncomingEdgesByNode, false, true);
                }
                if (Direction == TEXT("backward") || Direction == TEXT("both"))
                {
                    VisitEdges(State.IncomingEdgesByNode, false);
                }

                if ((Direction == TEXT("forward") || Direction == TEXT("both"))
                    && bExpandCalls
                    && !ExpandBlueprintTraceTargets(
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
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("ExplainBlueprintTrace failed to query the project index: %s"), *Error));
    }

    TSet<FString> UniqueBlueprints;
    int32 CrossBlueprintStepCount = 0;
    int32 CallNodeCount = 0;
    int32 EventNodeCount = 0;
    int32 VariableNodeCount = 0;
    int32 MacroNodeCount = 0;
    int32 CollapsedGraphNodeCount = 0;
    int32 InterfaceCallNodeCount = 0;
    int32 DispatcherNodeCount = 0;
    TArray<TSharedPtr<FJsonValue>> SummarySteps;

    for (const FString& NodeKey : VisitedOrderFinal)
    {
        const FTraceNodeRecord* Node = State.NodesByKey.Find(NodeKey);
        if (Node == nullptr)
        {
            continue;
        }

        UniqueBlueprints.Add(Node->BlueprintObjectPath);
        if (Node->NodeType == TEXT("call_function"))
        {
            ++CallNodeCount;
        }
        if (Node->NodeType == TEXT("event") || Node->NodeType == TEXT("custom_event") || Node->NodeType == TEXT("function_entry"))
        {
            ++EventNodeCount;
        }
        if (Node->NodeType == TEXT("variable_get") || Node->NodeType == TEXT("variable_set"))
        {
            ++VariableNodeCount;
        }
        if (Node->NodeType == TEXT("macro_instance") || Node->NodeType == TEXT("macro_entry") || Node->NodeType == TEXT("macro_exit"))
        {
            ++MacroNodeCount;
        }
        if (Node->NodeType == TEXT("composite_instance") || Node->NodeType == TEXT("composite_entry") || Node->NodeType == TEXT("composite_exit"))
        {
            ++CollapsedGraphNodeCount;
        }
        if (Node->NodeType == TEXT("interface_call"))
        {
            ++InterfaceCallNodeCount;
        }
        if (Node->NodeType.StartsWith(TEXT("delegate_")))
        {
            ++DispatcherNodeCount;
        }

        const bool bIsCrossBlueprint = Node->bHasResolvedMemberAsset
            && !Node->ResolvedMemberAssetObjectPath.IsEmpty()
            && !Node->ResolvedMemberAssetObjectPath.Equals(Node->BlueprintObjectPath, ESearchCase::CaseSensitive);
        if (bIsCrossBlueprint)
        {
            ++CrossBlueprintStepCount;
        }

        if (SummarySteps.Num() < 12)
        {
            SummarySteps.Add(MakeShared<FJsonValueString>(BuildExplainNodeLabel(*Node)));
        }
    }

    const FString Confidence = BuildTraceConfidence(StartNodeKeysFinal, State, bFollowCrossBlueprintCalls);
    const FString Explanation = BuildExplanationText(State, VisitedOrderFinal, Confidence);

    int32 MacroTransitionCount = 0;
    int32 CollapsedGraphTransitionCount = 0;
    int32 InterfaceTransitionCount = 0;
    int32 DispatcherTransitionCount = 0;
    for (const FTraceEdgeRecord& Edge : TraversedEdgesFinal)
    {
        MacroTransitionCount += Edge.EdgeKind == TEXT("macro") ? 1 : 0;
        CollapsedGraphTransitionCount += Edge.EdgeKind == TEXT("collapsed_graph") ? 1 : 0;
        InterfaceTransitionCount += Edge.EdgeKind == TEXT("interface_call") ? 1 : 0;
        DispatcherTransitionCount += Edge.EdgeKind == TEXT("delegate_bind") || Edge.EdgeKind == TEXT("delegate_broadcast") ? 1 : 0;
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("graphName"), GraphNameFilter);
    Result->SetStringField(TEXT("startNodeQuery"), StartNodeQuery);
    Result->SetStringField(TEXT("direction"), Direction);
    Result->SetBoolField(TEXT("resolveExternalMembers"), bResolveExternalMembers);
    Result->SetBoolField(TEXT("expandCalls"), bExpandCalls);
    Result->SetBoolField(TEXT("followCrossBlueprintCalls"), bFollowCrossBlueprintCalls);
    Result->SetStringField(TEXT("traceConfidence"), Confidence);
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    Result->SetBoolField(TEXT("matchedStartNodes"), bMatchedStartNodes);
    Result->SetNumberField(TEXT("startNodeCount"), StartNodeCount);
    Result->SetNumberField(TEXT("tracedNodeCount"), VisitedOrderFinal.Num());
    Result->SetNumberField(TEXT("tracedEdgeCount"), TraversedEdgesFinal.Num());
    Result->SetNumberField(TEXT("tracedBlueprintCount"), UniqueBlueprints.Num());
    Result->SetNumberField(TEXT("crossBlueprintStepCount"), CrossBlueprintStepCount);
    Result->SetNumberField(TEXT("callNodeCount"), CallNodeCount);
    Result->SetNumberField(TEXT("eventNodeCount"), EventNodeCount);
    Result->SetNumberField(TEXT("variableNodeCount"), VariableNodeCount);
    Result->SetNumberField(TEXT("macroNodeCount"), MacroNodeCount);
    Result->SetNumberField(TEXT("collapsedGraphNodeCount"), CollapsedGraphNodeCount);
    Result->SetNumberField(TEXT("interfaceCallNodeCount"), InterfaceCallNodeCount);
    Result->SetNumberField(TEXT("dispatcherNodeCount"), DispatcherNodeCount);
    Result->SetNumberField(TEXT("macroTransitionCount"), MacroTransitionCount);
    Result->SetNumberField(TEXT("collapsedGraphTransitionCount"), CollapsedGraphTransitionCount);
    Result->SetNumberField(TEXT("interfaceTransitionCount"), InterfaceTransitionCount);
    Result->SetNumberField(TEXT("dispatcherTransitionCount"), DispatcherTransitionCount);
    Result->SetStringField(TEXT("summaryText"), Explanation);
    Result->SetArrayField(TEXT("summarySteps"), SummarySteps);
    Result->SetStringField(TEXT("nextStepHint"), TEXT("Use TraceBlueprintFlow for the raw traced subgraph, or InspectBlueprintNode on one of the summarized steps for local detail."));
    if (const TSharedPtr<FJsonObject> BlueprintAsset = State.AssetsByObjectPath.FindRef(ObjectPath))
    {
        Result->SetObjectField(TEXT("asset"), BlueprintAsset.ToSharedRef());
    }
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FExplainBlueprintTraceTool::BuildInputSchema() const
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
    GraphNameProperty->SetStringField(TEXT("description"), TEXT("Optional Blueprint graph name filter."));
    Properties->SetObjectField(TEXT("graphName"), GraphNameProperty);

    TSharedRef<FJsonObject> StartNodeQueryProperty = MakeShared<FJsonObject>();
    StartNodeQueryProperty->SetStringField(TEXT("type"), TEXT("string"));
    StartNodeQueryProperty->SetStringField(TEXT("description"), TEXT("Optional node, event, function, variable, graph, or pin text to choose start nodes. When omitted, graph entry nodes are used."));
    Properties->SetObjectField(TEXT("startNodeQuery"), StartNodeQueryProperty);

    TSharedRef<FJsonObject> DirectionProperty = MakeShared<FJsonObject>();
    DirectionProperty->SetStringField(TEXT("type"), TEXT("string"));
    DirectionProperty->SetStringField(TEXT("description"), TEXT("Traversal direction: forward, backward, or both. Defaults to forward."));
    Properties->SetObjectField(TEXT("direction"), DirectionProperty);

    TSharedRef<FJsonObject> ResolveExternalMembersProperty = MakeShared<FJsonObject>();
    ResolveExternalMembersProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    ResolveExternalMembersProperty->SetStringField(TEXT("description"), TEXT("Optional. When true, resolve node member owners back to indexed assets. Defaults to true."));
    Properties->SetObjectField(TEXT("resolveExternalMembers"), ResolveExternalMembersProperty);

    TSharedRef<FJsonObject> ExpandCallsProperty = MakeShared<FJsonObject>();
    ExpandCallsProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    ExpandCallsProperty->SetStringField(TEXT("description"), TEXT("Optional. When true, the explanation may continue into called functions or custom events. Defaults to true."));
    Properties->SetObjectField(TEXT("expandCalls"), ExpandCallsProperty);

    TSharedRef<FJsonObject> FollowCrossBlueprintCallsProperty = MakeShared<FJsonObject>();
    FollowCrossBlueprintCallsProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    FollowCrossBlueprintCallsProperty->SetStringField(TEXT("description"), TEXT("Optional. When true, bounded call expansion may continue into target Blueprints. Defaults to true."));
    Properties->SetObjectField(TEXT("followCrossBlueprintCalls"), FollowCrossBlueprintCallsProperty);

    TSharedRef<FJsonObject> MaxDepthProperty = MakeShared<FJsonObject>();
    MaxDepthProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxDepthProperty->SetStringField(TEXT("description"), TEXT("Optional max traversal depth. Defaults to 4."));
    Properties->SetObjectField(TEXT("maxDepth"), MaxDepthProperty);

    TSharedRef<FJsonObject> MaxNodesProperty = MakeShared<FJsonObject>();
    MaxNodesProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxNodesProperty->SetStringField(TEXT("description"), TEXT("Optional max traced node count. Defaults to 32."));
    Properties->SetObjectField(TEXT("maxNodes"), MaxNodesProperty);

    TSharedRef<FJsonObject> MaxStartNodesProperty = MakeShared<FJsonObject>();
    MaxStartNodesProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxStartNodesProperty->SetStringField(TEXT("description"), TEXT("Optional max matched start nodes. Defaults to 6."));
    Properties->SetObjectField(TEXT("maxStartNodes"), MaxStartNodesProperty);

    TSharedRef<FJsonObject> MaxCallDepthProperty = MakeShared<FJsonObject>();
    MaxCallDepthProperty->SetStringField(TEXT("type"), TEXT("integer"));
    MaxCallDepthProperty->SetStringField(TEXT("description"), TEXT("Optional max call expansion depth. Defaults to 1."));
    Properties->SetObjectField(TEXT("maxCallDepth"), MaxCallDepthProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
