#include "Tools/TraceBlueprintFlowToolInternal.h"

#include "Containers/Queue.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"

namespace
{
    FString MakeExpansionEdgeKey(const FTraceEdgeRecord& Edge)
    {
        return FString::Printf(TEXT("%s|%s|%s|%s|%s|%s|%s|%s|%s"),
            *Edge.SourceBlueprintObjectPath,
            *Edge.SourceGraphName,
            *Edge.SourceNodeGuid,
            *Edge.SourcePinId,
            *Edge.TargetBlueprintObjectPath,
            *Edge.TargetGraphName,
            *Edge.TargetNodeGuid,
            *Edge.TargetPinId,
            *Edge.EdgeKind);
    }

    void AddExpansionTargets(
        const FTraceNodeRecord& CurrentNode,
        const TArray<FString>& TargetNodeKeys,
        const FString& EdgeKind,
        int32 CurrentDepth,
        int32 CurrentCallDepth,
        FTraceTraversalState& State,
        TSet<FString>& TraversedEdgeKeys,
        TArray<FTraceEdgeRecord>& TraversedEdges,
        TSet<FString>& VisitedNodeSet,
        TQueue<FTraceFrontierItem>& Frontier)
    {
        for (const FString& TargetNodeKey : TargetNodeKeys)
        {
            const FTraceNodeRecord* TargetNode = State.NodesByKey.Find(TargetNodeKey);
            if (TargetNode == nullptr)
            {
                continue;
            }

            FTraceEdgeRecord Edge;
            Edge.SourceBlueprintObjectPath = CurrentNode.BlueprintObjectPath;
            Edge.SourceGraphName = CurrentNode.GraphName;
            Edge.SourceNodeGuid = CurrentNode.NodeGuid;
            Edge.TargetBlueprintObjectPath = TargetNode->BlueprintObjectPath;
            Edge.TargetGraphName = TargetNode->GraphName;
            Edge.TargetNodeGuid = TargetNode->NodeGuid;
            Edge.EdgeKind = EdgeKind;

            const FString EdgeKey = MakeExpansionEdgeKey(Edge);
            if (!TraversedEdgeKeys.Contains(EdgeKey))
            {
                TraversedEdgeKeys.Add(EdgeKey);
                TraversedEdges.Add(Edge);
            }

            if (VisitedNodeSet.Contains(TargetNodeKey))
            {
                continue;
            }

            if (FTraceNodeRecord* MutableTargetNode = State.NodesByKey.Find(TargetNodeKey))
            {
                if (MutableTargetNode->Depth == INDEX_NONE || CurrentDepth + 1 < MutableTargetNode->Depth)
                {
                    MutableTargetNode->Depth = CurrentDepth + 1;
                }
            }

            Frontier.Enqueue(FTraceFrontierItem{ TargetNodeKey, CurrentDepth + 1, CurrentCallDepth + 1 });
        }
    }

    void AppendMemberEntries(
        const FString& BlueprintObjectPath,
        const FString& MemberName,
        const FTraceTraversalState& State,
        TArray<FString>& OutTargetNodeKeys)
    {
        if (const TArray<FString>* Entries = State.EntryNodeKeysByAssetAndMember.Find(MakeTraceMemberLookupKey(BlueprintObjectPath, MemberName)))
        {
            for (const FString& Entry : *Entries)
            {
                OutTargetNodeKeys.AddUnique(Entry);
            }
        }
    }

    bool QueryInterfaceImplementations(
        FSQLiteDatabase& Database,
        const FTraceNodeRecord& CurrentNode,
        bool bFollowCrossBlueprintCalls,
        FTraceTraversalState& State,
        TArray<FString>& OutTargetNodeKeys,
        FString& OutError)
    {
        if (CurrentNode.bHasResolvedTargetAsset)
        {
            if (!bFollowCrossBlueprintCalls
                && !CurrentNode.ResolvedTargetAssetObjectPath.Equals(CurrentNode.BlueprintObjectPath, ESearchCase::CaseSensitive))
            {
                return true;
            }
            if (!LoadBlueprintTraceData(Database, CurrentNode.ResolvedTargetAssetObjectPath, true, State, OutError))
            {
                return false;
            }
            AppendMemberEntries(CurrentNode.ResolvedTargetAssetObjectPath, CurrentNode.MemberName, State, OutTargetNodeKeys);
            if (OutTargetNodeKeys.Num() > 0)
            {
                return true;
            }
        }

        FSQLitePreparedStatement Statement(
            Database,
            TEXT("SELECT DISTINCT blueprint_object_path FROM blueprint_functions WHERE name = ?1 AND interface_path = ?2 ORDER BY blueprint_object_path ASC LIMIT 32;"),
            ESQLitePreparedStatementFlags::None);
        if (!Statement.IsValid()
            || !Statement.SetBindingValueByIndex(1, CurrentNode.MemberName)
            || !Statement.SetBindingValueByIndex(2, CurrentNode.MemberParentPath))
        {
            OutError = TEXT("TraceBlueprintFlow could not prepare the interface implementation query.");
            return false;
        }

        TArray<FString> ImplementingBlueprints;
        const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
        {
            FString ObjectPath;
            if (!Row.GetColumnValueByIndex(0, ObjectPath))
            {
                return ESQLitePreparedStatementExecuteRowResult::Error;
            }
            ImplementingBlueprints.Add(ObjectPath);
            return ESQLitePreparedStatementExecuteRowResult::Continue;
        });
        if (QueryResult == INDEX_NONE)
        {
            OutError = Database.GetLastError().IsEmpty() ? TEXT("TraceBlueprintFlow interface implementation query failed.") : Database.GetLastError();
            return false;
        }

        for (const FString& ObjectPath : ImplementingBlueprints)
        {
            if (!bFollowCrossBlueprintCalls && !ObjectPath.Equals(CurrentNode.BlueprintObjectPath, ESearchCase::CaseSensitive))
            {
                continue;
            }
            if (!LoadBlueprintTraceData(Database, ObjectPath, true, State, OutError))
            {
                return false;
            }
            AppendMemberEntries(ObjectPath, CurrentNode.MemberName, State, OutTargetNodeKeys);
        }
        return true;
    }

    void FindDelegateHandlerEntries(
        const FString& BindNodeKey,
        const FTraceTraversalState& State,
        TArray<FString>& OutTargetNodeKeys)
    {
        TArray<FTraceEdgeRecord> IncomingEdges;
        State.IncomingEdgesByNode.MultiFind(BindNodeKey, IncomingEdges);
        for (const FTraceEdgeRecord& Edge : IncomingEdges)
        {
            const FString SourceKey = MakeTraceNodeKey(Edge.SourceBlueprintObjectPath, Edge.SourceGraphName, Edge.SourceNodeGuid);
            const FTraceNodeRecord* HandlerNode = State.NodesByKey.Find(SourceKey);
            if (HandlerNode == nullptr || HandlerNode->MemberName.IsEmpty())
            {
                continue;
            }

            if (HandlerNode->NodeType == TEXT("custom_event") || HandlerNode->NodeType == TEXT("function_entry"))
            {
                OutTargetNodeKeys.AddUnique(SourceKey);
                continue;
            }

            if (HandlerNode->NodeType == TEXT("delegate_handler"))
            {
                const FString TargetAsset = HandlerNode->bHasResolvedMemberAsset
                    ? HandlerNode->ResolvedMemberAssetObjectPath
                    : HandlerNode->BlueprintObjectPath;
                AppendMemberEntries(TargetAsset, HandlerNode->MemberName, State, OutTargetNodeKeys);
            }
        }
    }

    bool QueryDelegateBroadcastTargets(
        FSQLiteDatabase& Database,
        const FTraceNodeRecord& CurrentNode,
        bool bFollowCrossBlueprintCalls,
        FTraceTraversalState& State,
        TArray<FString>& OutTargetNodeKeys,
        FString& OutError)
    {
        FSQLitePreparedStatement Statement(
            Database,
            TEXT("SELECT blueprint_object_path, graph_name, node_guid FROM blueprint_nodes WHERE node_type = 'delegate_bind' AND member_name = ?1 AND member_parent_path = ?2 ORDER BY blueprint_object_path ASC LIMIT 64;"),
            ESQLitePreparedStatementFlags::None);
        if (!Statement.IsValid()
            || !Statement.SetBindingValueByIndex(1, CurrentNode.MemberName)
            || !Statement.SetBindingValueByIndex(2, CurrentNode.MemberParentPath))
        {
            OutError = TEXT("TraceBlueprintFlow could not prepare the dispatcher binding query.");
            return false;
        }

        struct FBindLocation { FString ObjectPath; FString GraphName; FString NodeGuid; };
        TArray<FBindLocation> BindLocations;
        const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
        {
            FBindLocation Location;
            if (!Row.GetColumnValueByIndex(0, Location.ObjectPath)
                || !Row.GetColumnValueByIndex(1, Location.GraphName)
                || !Row.GetColumnValueByIndex(2, Location.NodeGuid))
            {
                return ESQLitePreparedStatementExecuteRowResult::Error;
            }
            BindLocations.Add(MoveTemp(Location));
            return ESQLitePreparedStatementExecuteRowResult::Continue;
        });
        if (QueryResult == INDEX_NONE)
        {
            OutError = Database.GetLastError().IsEmpty() ? TEXT("TraceBlueprintFlow dispatcher binding query failed.") : Database.GetLastError();
            return false;
        }

        for (const FBindLocation& Location : BindLocations)
        {
            if (!bFollowCrossBlueprintCalls && !Location.ObjectPath.Equals(CurrentNode.BlueprintObjectPath, ESearchCase::CaseSensitive))
            {
                continue;
            }
            if (!LoadBlueprintTraceData(Database, Location.ObjectPath, true, State, OutError))
            {
                return false;
            }
            FindDelegateHandlerEntries(MakeTraceNodeKey(Location.ObjectPath, Location.GraphName, Location.NodeGuid), State, OutTargetNodeKeys);
        }
        return true;
    }
}

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
    FString& OutError)
{
    // Loading another Blueprint can rehash NodesByKey, so never retain a map value reference across a load.
    const FTraceNodeRecord ExpansionNode = CurrentNode;

    if (ExpansionNode.MemberName.IsEmpty() || CurrentDepth >= MaxDepth || CurrentCallDepth >= MaxCallDepth)
    {
        return true;
    }

    TArray<FString> TargetNodeKeys;
    FString EdgeKind;

    if (ExpansionNode.NodeType == TEXT("call_function")
        || ExpansionNode.NodeType == TEXT("macro_instance")
        || ExpansionNode.NodeType == TEXT("composite_instance"))
    {
        const FString TargetBlueprintPath = ExpansionNode.bHasResolvedTargetAsset
            ? ExpansionNode.ResolvedTargetAssetObjectPath
            : ExpansionNode.ResolvedMemberAssetObjectPath;
        if (TargetBlueprintPath.IsEmpty()
            || (!ExpansionNode.bHasResolvedTargetAsset
                && (!ExpansionNode.bHasResolvedMemberAsset || !ExpansionNode.bResolvedMemberAssetIsBlueprint)))
        {
            return true;
        }

        const bool bIsCrossBlueprint = !TargetBlueprintPath.Equals(ExpansionNode.BlueprintObjectPath, ESearchCase::CaseSensitive);
        if (bIsCrossBlueprint && !bFollowCrossBlueprintCalls)
        {
            return true;
        }
        if (!LoadBlueprintTraceData(Database, TargetBlueprintPath, true, State, OutError))
        {
            return false;
        }
        AppendMemberEntries(TargetBlueprintPath, ExpansionNode.MemberName, State, TargetNodeKeys);
        EdgeKind = ExpansionNode.NodeType == TEXT("macro_instance")
            ? TEXT("macro")
            : (ExpansionNode.NodeType == TEXT("composite_instance") ? TEXT("collapsed_graph") : TEXT("call"));
    }
    else if (ExpansionNode.NodeType == TEXT("interface_call"))
    {
        if (!QueryInterfaceImplementations(Database, ExpansionNode, bFollowCrossBlueprintCalls, State, TargetNodeKeys, OutError))
        {
            return false;
        }
        EdgeKind = TEXT("interface_call");
    }
    else if (ExpansionNode.NodeType == TEXT("delegate_bind"))
    {
        FindDelegateHandlerEntries(
            MakeTraceNodeKey(ExpansionNode.BlueprintObjectPath, ExpansionNode.GraphName, ExpansionNode.NodeGuid),
            State,
            TargetNodeKeys);
        EdgeKind = TEXT("delegate_bind");
    }
    else if (ExpansionNode.NodeType == TEXT("delegate_broadcast"))
    {
        if (!QueryDelegateBroadcastTargets(Database, ExpansionNode, bFollowCrossBlueprintCalls, State, TargetNodeKeys, OutError))
        {
            return false;
        }
        EdgeKind = TEXT("delegate_broadcast");
    }
    else
    {
        return true;
    }

    AddExpansionTargets(
        ExpansionNode,
        TargetNodeKeys,
        EdgeKind,
        CurrentDepth,
        CurrentCallDepth,
        State,
        TraversedEdgeKeys,
        TraversedEdges,
        VisitedNodeSet,
        Frontier);
    return true;
}
