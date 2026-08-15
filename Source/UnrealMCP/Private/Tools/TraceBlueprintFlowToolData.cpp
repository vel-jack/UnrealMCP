#include "Tools/TraceBlueprintFlowToolInternal.h"

#include "SQLitePreparedStatement.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    bool ResolveNodeAssetReferences(FSQLiteDatabase& Database, FTraceTraversalState& State, FString& OutError)
    {
        for (TPair<FString, FTraceNodeRecord>& Pair : State.NodesByKey)
        {
            FTraceNodeRecord& Node = Pair.Value;
            if (Node.bHasResolvedMemberAsset || Node.MemberParentPath.IsEmpty())
            {
                continue;
            }

            FSQLitePreparedStatement Statement(
                Database,
                TEXT("SELECT object_path, asset_name, package_name, content_scope, is_blueprint "
                     "FROM assets WHERE generated_class_path = ?1 ORDER BY object_path ASC LIMIT 1;"),
                ESQLitePreparedStatementFlags::None);
            if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, Node.MemberParentPath))
            {
                OutError = TEXT("TraceBlueprintFlow could not prepare the external member asset query.");
                return false;
            }

            const int64 QueryResult = Statement.Execute([&Node](const FSQLitePreparedStatement& Row)
            {
                int32 bIsBlueprint = 0;
                if (!Row.GetColumnValueByIndex(0, Node.ResolvedMemberAssetObjectPath)
                    || !Row.GetColumnValueByIndex(1, Node.ResolvedMemberAssetName)
                    || !Row.GetColumnValueByIndex(2, Node.ResolvedMemberAssetPackageName)
                    || !Row.GetColumnValueByIndex(3, Node.ResolvedMemberAssetScope)
                    || !Row.GetColumnValueByIndex(4, bIsBlueprint))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }

                Node.bHasResolvedMemberAsset = true;
                Node.bResolvedMemberAssetIsBlueprint = bIsBlueprint != 0;
                return ESQLitePreparedStatementExecuteRowResult::Stop;
            });

            if (QueryResult == INDEX_NONE)
            {
                OutError = Database.GetLastError().IsEmpty() ? TEXT("TraceBlueprintFlow external member asset query failed.") : Database.GetLastError();
                return false;
            }
        }

        return true;
    }

    bool ResolveConnectedTargetAssets(FSQLiteDatabase& Database, const FString& ObjectPath, FTraceTraversalState& State, FString& OutError)
    {
        FSQLitePreparedStatement TargetStatement(
            Database,
            TEXT("SELECT e.target_graph_name, e.target_node_guid, source_pin.subcategory_object_path "
                 "FROM blueprint_edges e "
                 "JOIN blueprint_pins target_pin ON target_pin.blueprint_object_path = e.blueprint_object_path AND target_pin.graph_name = e.target_graph_name AND target_pin.node_guid = e.target_node_guid AND target_pin.pin_id = e.target_pin_id "
                 "JOIN blueprint_pins source_pin ON source_pin.blueprint_object_path = e.blueprint_object_path AND source_pin.graph_name = e.source_graph_name AND source_pin.node_guid = e.source_node_guid AND source_pin.pin_id = e.source_pin_id "
                 "WHERE e.blueprint_object_path = ?1 AND lower(target_pin.pin_name) IN ('self', 'target') AND source_pin.subcategory_object_path <> '' "
                 "ORDER BY e.target_graph_name ASC, e.target_node_guid ASC;"),
            ESQLitePreparedStatementFlags::None);
        if (!TargetStatement.IsValid() || !TargetStatement.SetBindingValueByIndex(1, ObjectPath))
        {
            OutError = TEXT("TraceBlueprintFlow could not prepare the connected target query.");
            return false;
        }

        const int64 TargetResult = TargetStatement.Execute([&](const FSQLitePreparedStatement& Row)
        {
            FString GraphName;
            FString NodeGuid;
            FString TargetClassPath;
            if (!Row.GetColumnValueByIndex(0, GraphName)
                || !Row.GetColumnValueByIndex(1, NodeGuid)
                || !Row.GetColumnValueByIndex(2, TargetClassPath))
            {
                return ESQLitePreparedStatementExecuteRowResult::Error;
            }

            if (FTraceNodeRecord* Node = State.NodesByKey.Find(MakeTraceNodeKey(ObjectPath, GraphName, NodeGuid)))
            {
                Node->ConnectedTargetClassPath = TargetClassPath;
            }
            return ESQLitePreparedStatementExecuteRowResult::Continue;
        });
        if (TargetResult == INDEX_NONE)
        {
            OutError = Database.GetLastError().IsEmpty() ? TEXT("TraceBlueprintFlow connected target query failed.") : Database.GetLastError();
            return false;
        }

        TMap<FString, TPair<FString, FString>> AssetByClassPath;
        for (TPair<FString, FTraceNodeRecord>& Pair : State.NodesByKey)
        {
            FTraceNodeRecord& Node = Pair.Value;
            if (!Node.BlueprintObjectPath.Equals(ObjectPath, ESearchCase::CaseSensitive) || Node.ConnectedTargetClassPath.IsEmpty())
            {
                continue;
            }

            if (!AssetByClassPath.Contains(Node.ConnectedTargetClassPath))
            {
                FString AssetObjectPath;
                FString AssetName;
                FSQLitePreparedStatement AssetStatement(
                    Database,
                    TEXT("SELECT object_path, asset_name FROM assets WHERE generated_class_path = ?1 AND is_blueprint = 1 ORDER BY object_path ASC LIMIT 1;"),
                    ESQLitePreparedStatementFlags::None);
                if (!AssetStatement.IsValid() || !AssetStatement.SetBindingValueByIndex(1, Node.ConnectedTargetClassPath))
                {
                    OutError = TEXT("TraceBlueprintFlow could not prepare the connected target asset query.");
                    return false;
                }

                const int64 AssetResult = AssetStatement.Execute([&](const FSQLitePreparedStatement& AssetRow)
                {
                    if (!AssetRow.GetColumnValueByIndex(0, AssetObjectPath) || !AssetRow.GetColumnValueByIndex(1, AssetName))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }
                    return ESQLitePreparedStatementExecuteRowResult::Stop;
                });
                if (AssetResult == INDEX_NONE)
                {
                    OutError = Database.GetLastError().IsEmpty() ? TEXT("TraceBlueprintFlow connected target asset query failed.") : Database.GetLastError();
                    return false;
                }
                AssetByClassPath.Add(Node.ConnectedTargetClassPath, TPair<FString, FString>(MoveTemp(AssetObjectPath), MoveTemp(AssetName)));
            }

            const TPair<FString, FString>& Asset = AssetByClassPath[Node.ConnectedTargetClassPath];
            if (!Asset.Key.IsEmpty())
            {
                Node.bHasResolvedTargetAsset = true;
                Node.ResolvedTargetAssetObjectPath = Asset.Key;
                Node.ResolvedTargetAssetName = Asset.Value;
            }
        }
        return true;
    }

    void IndexEntryNodesForBlueprint(FTraceTraversalState& State, const FString& ObjectPath)
    {
        for (const FGraphEntryRecord& Entry : State.GraphEntries)
        {
            if (!Entry.BlueprintObjectPath.Equals(ObjectPath, ESearchCase::CaseSensitive) || Entry.EntryNodeGuid.IsEmpty())
            {
                continue;
            }

            const FString EntryKey = MakeTraceNodeKey(ObjectPath, Entry.GraphName, Entry.EntryNodeGuid);
            if (FTraceNodeRecord* Node = State.NodesByKey.Find(EntryKey))
            {
                Node->bIsEntry = true;
                State.EntryNodeKeysByAssetAndMember.FindOrAdd(MakeTraceMemberLookupKey(ObjectPath, Entry.GraphName)).AddUnique(EntryKey);
                if (!Node->MemberName.IsEmpty()
                    && (Node->NodeType == TEXT("function_entry") || Node->NodeType == TEXT("custom_event") || Node->NodeType == TEXT("macro_entry")))
                {
                    State.EntryNodeKeysByAssetAndMember.FindOrAdd(MakeTraceMemberLookupKey(ObjectPath, Node->MemberName)).AddUnique(EntryKey);
                }
            }
        }

        for (TPair<FString, FTraceNodeRecord>& Pair : State.NodesByKey)
        {
            FTraceNodeRecord& Node = Pair.Value;
            if (!Node.BlueprintObjectPath.Equals(ObjectPath, ESearchCase::CaseSensitive)
                || Node.MemberName.IsEmpty()
                || (Node.NodeType != TEXT("function_entry")
                    && Node.NodeType != TEXT("custom_event")
                    && Node.NodeType != TEXT("event")
                    && Node.NodeType != TEXT("macro_entry")
                    && Node.NodeType != TEXT("composite_entry")))
            {
                continue;
            }

            Node.bIsEntry = true;
            State.EntryNodeKeysByAssetAndMember.FindOrAdd(MakeTraceMemberLookupKey(ObjectPath, Node.MemberName)).AddUnique(Pair.Key);
        }
    }
}

bool LoadBlueprintTraceData(
    FSQLiteDatabase& Database,
    const FString& ObjectPath,
    bool bResolveExternalMembers,
    FTraceTraversalState& State,
    FString& OutError)
{
    if (State.LoadedBlueprintObjectPaths.Contains(ObjectPath))
    {
        return true;
    }

    TSharedPtr<FJsonObject> BlueprintAsset;
    if (!UnrealMCP::IndexedQueryToolUtils::QueryAssetByObjectPath(Database, ObjectPath, BlueprintAsset, OutError) || !BlueprintAsset.IsValid())
    {
        OutError = OutError.IsEmpty()
            ? TEXT("TraceBlueprintFlow could not find the requested Blueprint in the project index.")
            : OutError;
        return false;
    }

    bool bIsBlueprint = false;
    if (!BlueprintAsset->TryGetBoolField(TEXT("isBlueprint"), bIsBlueprint) || !bIsBlueprint)
    {
        OutError = TEXT("TraceBlueprintFlow requires params.objectPath to reference an indexed Blueprint asset.");
        return false;
    }

    State.AssetsByObjectPath.Add(ObjectPath, BlueprintAsset);

    {
        const FString Sql = TEXT("SELECT graph_name, node_guid, node_name, node_class_path, node_title, node_type, member_name, member_parent_path, pos_x, pos_y, is_pure, input_pin_count, output_pin_count "
                                 "FROM blueprint_nodes WHERE blueprint_object_path = ?1 ORDER BY graph_name ASC, node_guid ASC;");
        FSQLitePreparedStatement Statement(Database, *Sql, ESQLitePreparedStatementFlags::None);
        if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ObjectPath))
        {
            OutError = TEXT("TraceBlueprintFlow could not prepare the Blueprint node query.");
            return false;
        }

        const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
        {
            FTraceNodeRecord Node;
            int32 bIsPureInt = 0;

            Node.BlueprintObjectPath = ObjectPath;
            if (!Row.GetColumnValueByIndex(0, Node.GraphName)
                || !Row.GetColumnValueByIndex(1, Node.NodeGuid)
                || !Row.GetColumnValueByIndex(2, Node.NodeName)
                || !Row.GetColumnValueByIndex(3, Node.NodeClassPath)
                || !Row.GetColumnValueByIndex(4, Node.NodeTitle)
                || !Row.GetColumnValueByIndex(5, Node.NodeType)
                || !Row.GetColumnValueByIndex(6, Node.MemberName)
                || !Row.GetColumnValueByIndex(7, Node.MemberParentPath)
                || !Row.GetColumnValueByIndex(8, Node.PosX)
                || !Row.GetColumnValueByIndex(9, Node.PosY)
                || !Row.GetColumnValueByIndex(10, bIsPureInt)
                || !Row.GetColumnValueByIndex(11, Node.InputPinCount)
                || !Row.GetColumnValueByIndex(12, Node.OutputPinCount))
            {
                return ESQLitePreparedStatementExecuteRowResult::Error;
            }

            Node.bIsPure = bIsPureInt != 0;
            ClassifyTraceNodeSemantics(Node);
            State.NodesByKey.Add(MakeTraceNodeKey(ObjectPath, Node.GraphName, Node.NodeGuid), MoveTemp(Node));
            return ESQLitePreparedStatementExecuteRowResult::Continue;
        });

        if (QueryResult == INDEX_NONE)
        {
            OutError = Database.GetLastError().IsEmpty() ? TEXT("TraceBlueprintFlow node query failed.") : Database.GetLastError();
            return false;
        }
    }

    {
        const FString Sql = TEXT("SELECT graph_name, node_guid, pin_name, default_value FROM blueprint_pins "
                                 "WHERE blueprint_object_path = ?1 AND default_value <> '' "
                                 "AND lower(pin_name) IN ('functionname', 'function_name') "
                                 "ORDER BY graph_name ASC, node_guid ASC;");
        FSQLitePreparedStatement Statement(Database, *Sql, ESQLitePreparedStatementFlags::None);
        if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ObjectPath))
        {
            OutError = TEXT("TraceBlueprintFlow could not prepare the timer callback query.");
            return false;
        }

        const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
        {
            FString GraphName;
            FString NodeGuid;
            FString PinName;
            FString DefaultValue;
            if (!Row.GetColumnValueByIndex(0, GraphName)
                || !Row.GetColumnValueByIndex(1, NodeGuid)
                || !Row.GetColumnValueByIndex(2, PinName)
                || !Row.GetColumnValueByIndex(3, DefaultValue))
            {
                return ESQLitePreparedStatementExecuteRowResult::Error;
            }

            if (FTraceNodeRecord* Node = State.NodesByKey.Find(MakeTraceNodeKey(ObjectPath, GraphName, NodeGuid));
                Node != nullptr && Node->ExecutionSemantic == TEXT("timer"))
            {
                Node->CallbackMemberName = DefaultValue;
            }
            return ESQLitePreparedStatementExecuteRowResult::Continue;
        });

        if (QueryResult == INDEX_NONE)
        {
            OutError = Database.GetLastError().IsEmpty() ? TEXT("TraceBlueprintFlow timer callback query failed.") : Database.GetLastError();
            return false;
        }
    }

    {
        const FString Sql = TEXT("SELECT graph_name, entry_node_guid, graph_type, node_count FROM blueprint_graphs WHERE blueprint_object_path = ?1 ORDER BY graph_name ASC;");
        FSQLitePreparedStatement Statement(Database, *Sql, ESQLitePreparedStatementFlags::None);
        if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ObjectPath))
        {
            OutError = TEXT("TraceBlueprintFlow could not prepare the Blueprint graph query.");
            return false;
        }

        const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
        {
            FGraphEntryRecord Entry;
            Entry.BlueprintObjectPath = ObjectPath;
            if (!Row.GetColumnValueByIndex(0, Entry.GraphName)
                || !Row.GetColumnValueByIndex(1, Entry.EntryNodeGuid)
                || !Row.GetColumnValueByIndex(2, Entry.GraphType)
                || !Row.GetColumnValueByIndex(3, Entry.NodeCount))
            {
                return ESQLitePreparedStatementExecuteRowResult::Error;
            }

            State.GraphEntries.Add(MoveTemp(Entry));
            return ESQLitePreparedStatementExecuteRowResult::Continue;
        });

        if (QueryResult == INDEX_NONE)
        {
            OutError = Database.GetLastError().IsEmpty() ? TEXT("TraceBlueprintFlow graph query failed.") : Database.GetLastError();
            return false;
        }
    }

    {
        const FString Sql = TEXT("SELECT source_graph_name, source_node_guid, source_pin_id, target_graph_name, target_node_guid, target_pin_id, edge_kind "
                                 "FROM blueprint_edges WHERE blueprint_object_path = ?1 ORDER BY source_graph_name ASC, source_node_guid ASC;");
        FSQLitePreparedStatement Statement(Database, *Sql, ESQLitePreparedStatementFlags::None);
        if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, ObjectPath))
        {
            OutError = TEXT("TraceBlueprintFlow could not prepare the Blueprint edge query.");
            return false;
        }

        const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
        {
            FTraceEdgeRecord Edge;
            Edge.SourceBlueprintObjectPath = ObjectPath;
            Edge.TargetBlueprintObjectPath = ObjectPath;
            if (!Row.GetColumnValueByIndex(0, Edge.SourceGraphName)
                || !Row.GetColumnValueByIndex(1, Edge.SourceNodeGuid)
                || !Row.GetColumnValueByIndex(2, Edge.SourcePinId)
                || !Row.GetColumnValueByIndex(3, Edge.TargetGraphName)
                || !Row.GetColumnValueByIndex(4, Edge.TargetNodeGuid)
                || !Row.GetColumnValueByIndex(5, Edge.TargetPinId)
                || !Row.GetColumnValueByIndex(6, Edge.EdgeKind))
            {
                return ESQLitePreparedStatementExecuteRowResult::Error;
            }

            State.OutgoingEdgesByNode.Add(MakeTraceNodeKey(ObjectPath, Edge.SourceGraphName, Edge.SourceNodeGuid), Edge);
            State.IncomingEdgesByNode.Add(MakeTraceNodeKey(ObjectPath, Edge.TargetGraphName, Edge.TargetNodeGuid), Edge);
            return ESQLitePreparedStatementExecuteRowResult::Continue;
        });

        if (QueryResult == INDEX_NONE)
        {
            OutError = Database.GetLastError().IsEmpty() ? TEXT("TraceBlueprintFlow edge query failed.") : Database.GetLastError();
            return false;
        }
    }

    if (bResolveExternalMembers && !ResolveNodeAssetReferences(Database, State, OutError))
    {
        return false;
    }

    if (bResolveExternalMembers && !ResolveConnectedTargetAssets(Database, ObjectPath, State, OutError))
    {
        return false;
    }

    IndexEntryNodesForBlueprint(State, ObjectPath);
    State.LoadedBlueprintObjectPaths.Add(ObjectPath);
    return true;
}
