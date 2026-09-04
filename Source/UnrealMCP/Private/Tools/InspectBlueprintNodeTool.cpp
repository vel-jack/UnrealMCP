#include "Tools/InspectBlueprintNodeTool.h"

#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/IndexedQueryToolUtils.h"
#include "Tools/TraceBlueprintFlowToolInternal.h"

namespace
{
    struct FNodeInspectMatch
    {
        FString NodeKey;
        FString MatchReason;
        int32 Score = 0;
    };

    FString GetOptionalStringField(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName)
    {
        FString Value;
        if (Params.IsValid())
        {
            Params->TryGetStringField(FieldName, Value);
        }
        return Value;
    }

    bool NodeMatchesQuery(const FTraceNodeRecord& Node, const FString& Query, FString& OutReason, int32& OutScore)
    {
        const FString LowerQuery = Query.ToLower();
        const FString LowerTitle = Node.NodeTitle.ToLower();
        const FString LowerName = Node.NodeName.ToLower();
        const FString LowerMember = Node.MemberName.ToLower();
        const FString LowerType = Node.NodeType.ToLower();
        const FString LowerGraph = Node.GraphName.ToLower();

        if (LowerQuery.IsEmpty())
        {
            return false;
        }

        if (!LowerTitle.IsEmpty() && LowerTitle == LowerQuery)
        {
            OutReason = TEXT("exact_node_title_match");
            OutScore = 120;
            return true;
        }

        if (!LowerMember.IsEmpty() && LowerMember == LowerQuery)
        {
            OutReason = TEXT("exact_member_name_match");
            OutScore = 115;
            return true;
        }

        if (!LowerName.IsEmpty() && LowerName == LowerQuery)
        {
            OutReason = TEXT("exact_node_name_match");
            OutScore = 108;
            return true;
        }

        if (!LowerTitle.IsEmpty() && LowerTitle.StartsWith(LowerQuery))
        {
            OutReason = TEXT("node_title_prefix_match");
            OutScore = 92;
            return true;
        }

        if (!LowerMember.IsEmpty() && LowerMember.StartsWith(LowerQuery))
        {
            OutReason = TEXT("member_name_prefix_match");
            OutScore = 88;
            return true;
        }

        if (!LowerTitle.IsEmpty() && LowerTitle.Contains(LowerQuery))
        {
            OutReason = TEXT("node_title_contains_query");
            OutScore = 76;
            return true;
        }

        if (!LowerMember.IsEmpty() && LowerMember.Contains(LowerQuery))
        {
            OutReason = TEXT("member_name_contains_query");
            OutScore = 72;
            return true;
        }

        if (!LowerName.IsEmpty() && LowerName.Contains(LowerQuery))
        {
            OutReason = TEXT("node_name_contains_query");
            OutScore = 58;
            return true;
        }

        if (!LowerGraph.IsEmpty() && LowerGraph.Contains(LowerQuery))
        {
            OutReason = TEXT("graph_name_contains_query");
            OutScore = 36;
            return true;
        }

        if (!LowerType.IsEmpty() && LowerType.Contains(LowerQuery))
        {
            OutReason = TEXT("node_type_contains_query");
            OutScore = 32;
            return true;
        }

        return false;
    }

    FString DescribeNodeRole(const FTraceNodeRecord& Node)
    {
        if (Node.NodeType == TEXT("event"))
        {
            return TEXT("Engine or Blueprint event entry point.");
        }
        if (Node.NodeType == TEXT("custom_event"))
        {
            return TEXT("Custom event entry point that can be called from other graph locations.");
        }
        if (Node.NodeType == TEXT("function_entry"))
        {
            return TEXT("Function entry node for this Blueprint function graph.");
        }
        if (Node.NodeType == TEXT("call_function"))
        {
            return Node.bHasResolvedMemberAsset && !Node.ResolvedMemberAssetObjectPath.Equals(Node.BlueprintObjectPath, ESearchCase::CaseSensitive)
                ? TEXT("Function call node that targets another Blueprint.")
                : TEXT("Function call node inside this Blueprint flow.");
        }
        if (Node.NodeType == TEXT("variable_get"))
        {
            return TEXT("Reads a Blueprint variable value.");
        }
        if (Node.NodeType == TEXT("variable_set"))
        {
            return TEXT("Writes or updates a Blueprint variable value.");
        }

        return TEXT("Indexed Blueprint graph node.");
    }

    TSharedRef<FJsonObject> BuildAdjacentNodeObject(const FTraceNodeRecord& Node)
    {
        TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
        NodeObject->SetStringField(TEXT("blueprintObjectPath"), Node.BlueprintObjectPath);
        NodeObject->SetStringField(TEXT("graphName"), Node.GraphName);
        NodeObject->SetStringField(TEXT("nodeGuid"), Node.NodeGuid);
        NodeObject->SetStringField(TEXT("nodeTitle"), Node.NodeTitle);
        NodeObject->SetStringField(TEXT("nodeType"), Node.NodeType);
        NodeObject->SetStringField(TEXT("memberName"), Node.MemberName);
        NodeObject->SetBoolField(TEXT("isEntry"), Node.bIsEntry);
        NodeObject->SetBoolField(TEXT("isCrossBlueprintReference"), Node.bHasResolvedMemberAsset && !Node.ResolvedMemberAssetObjectPath.Equals(Node.BlueprintObjectPath, ESearchCase::CaseSensitive));
        if (Node.bHasResolvedMemberAsset)
        {
            TSharedRef<FJsonObject> TargetObject = MakeShared<FJsonObject>();
            TargetObject->SetStringField(TEXT("objectPath"), Node.ResolvedMemberAssetObjectPath);
            TargetObject->SetStringField(TEXT("assetName"), Node.ResolvedMemberAssetName);
            TargetObject->SetStringField(TEXT("packageName"), Node.ResolvedMemberAssetPackageName);
            TargetObject->SetStringField(TEXT("contentScope"), Node.ResolvedMemberAssetScope);
            TargetObject->SetBoolField(TEXT("isBlueprint"), Node.bResolvedMemberAssetIsBlueprint);
            NodeObject->SetObjectField(TEXT("resolvedMemberAsset"), TargetObject);
        }
        return NodeObject;
    }
}

FInspectBlueprintNodeTool::FInspectBlueprintNodeTool()
    : FMCPToolBase(TEXT("InspectBlueprintNode"), TEXT("Explains one or more indexed Blueprint nodes, including immediate previous and next connected nodes plus trace hints."))
{
}

UnrealMCP::FMCPResponse FInspectBlueprintNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FString ObjectPath = GetOptionalStringField(Request.Params, TEXT("objectPath"));
    const FString Query = GetOptionalStringField(Request.Params, TEXT("query")).TrimStartAndEnd();
    const FString GraphNameFilter = GetOptionalStringField(Request.Params, TEXT("graphName"));
    const FString NodeGuid = GetOptionalStringField(Request.Params, TEXT("nodeGuid"));
    const int32 Limit = FMath::Clamp(UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params, 5), 1, 20);

    if (ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("InspectBlueprintNode requires params.objectPath."));
    }

    if (Query.IsEmpty() && NodeGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("InspectBlueprintNode requires params.query or params.nodeGuid."));
    }

    TSharedPtr<FJsonObject> BlueprintAsset;
    FTraceTraversalState State;
    TArray<FNodeInspectMatch> Matches;
    FString Error;

    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            if (!UnrealMCP::IndexedQueryToolUtils::QueryAssetByObjectPath(Database, ObjectPath, BlueprintAsset, OutExecError) || !BlueprintAsset.IsValid())
            {
                OutExecError = OutExecError.IsEmpty()
                    ? TEXT("InspectBlueprintNode could not find the requested Blueprint in the project index.")
                    : OutExecError;
                return false;
            }

            bool bIsBlueprint = false;
            if (!BlueprintAsset->TryGetBoolField(TEXT("isBlueprint"), bIsBlueprint) || !bIsBlueprint)
            {
                OutExecError = TEXT("InspectBlueprintNode requires params.objectPath to reference an indexed Blueprint asset.");
                return false;
            }

            if (!LoadBlueprintTraceData(Database, ObjectPath, true, State, OutExecError))
            {
                return false;
            }

            for (const TPair<FString, FTraceNodeRecord>& Pair : State.NodesByKey)
            {
                const FTraceNodeRecord& Node = Pair.Value;
                if (!Node.BlueprintObjectPath.Equals(ObjectPath, ESearchCase::CaseSensitive))
                {
                    continue;
                }

                if (!GraphNameFilter.IsEmpty() && !Node.GraphName.Equals(GraphNameFilter, ESearchCase::CaseSensitive))
                {
                    continue;
                }

                FString MatchReason;
                int32 MatchScore = 0;
                if (!NodeGuid.IsEmpty())
                {
                    if (!Node.NodeGuid.Equals(NodeGuid, ESearchCase::IgnoreCase))
                    {
                        continue;
                    }
                    MatchReason = TEXT("node_guid");
                    MatchScore = 200;
                }
                else if (!NodeMatchesQuery(Node, Query, MatchReason, MatchScore))
                {
                    continue;
                }

                Matches.Add(FNodeInspectMatch
                {
                    Pair.Key,
                    MatchReason,
                    MatchScore
                });
            }

            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("InspectBlueprintNode failed to query the project index: %s"), *Error));
    }

    Matches.Sort([&State](const FNodeInspectMatch& Left, const FNodeInspectMatch& Right)
    {
        if (Left.Score != Right.Score)
        {
            return Left.Score > Right.Score;
        }

        const FTraceNodeRecord& LeftNode = State.NodesByKey[Left.NodeKey];
        const FTraceNodeRecord& RightNode = State.NodesByKey[Right.NodeKey];
        if (LeftNode.GraphName != RightNode.GraphName)
        {
            return LeftNode.GraphName < RightNode.GraphName;
        }
        if (LeftNode.NodeTitle != RightNode.NodeTitle)
        {
            return LeftNode.NodeTitle < RightNode.NodeTitle;
        }
        return LeftNode.NodeGuid < RightNode.NodeGuid;
    });

    if (Matches.Num() > Limit)
    {
        Matches.SetNum(Limit);
    }

    TArray<TSharedPtr<FJsonValue>> MatchJson;
    TArray<TSharedPtr<FJsonValue>> SummaryJson;

    for (const FNodeInspectMatch& Match : Matches)
    {
        const FTraceNodeRecord* Node = State.NodesByKey.Find(Match.NodeKey);
        if (Node == nullptr)
        {
            continue;
        }

        TArray<FTraceEdgeRecord> IncomingEdges;
        State.IncomingEdgesByNode.MultiFind(Match.NodeKey, IncomingEdges);
        TArray<FTraceEdgeRecord> OutgoingEdges;
        State.OutgoingEdgesByNode.MultiFind(Match.NodeKey, OutgoingEdges);

        TArray<TSharedPtr<FJsonValue>> PreviousNodesJson;
        TSet<FString> PreviousNodeKeys;
        for (const FTraceEdgeRecord& Edge : IncomingEdges)
        {
            const FString PreviousKey = MakeTraceNodeKey(Edge.SourceBlueprintObjectPath, Edge.SourceGraphName, Edge.SourceNodeGuid);
            if (PreviousNodeKeys.Contains(PreviousKey))
            {
                continue;
            }

            if (const FTraceNodeRecord* PreviousNode = State.NodesByKey.Find(PreviousKey))
            {
                PreviousNodeKeys.Add(PreviousKey);
                PreviousNodesJson.Add(MakeShared<FJsonValueObject>(BuildAdjacentNodeObject(*PreviousNode)));
            }
        }

        TArray<TSharedPtr<FJsonValue>> NextNodesJson;
        TSet<FString> NextNodeKeys;
        for (const FTraceEdgeRecord& Edge : OutgoingEdges)
        {
            const FString NextKey = MakeTraceNodeKey(Edge.TargetBlueprintObjectPath, Edge.TargetGraphName, Edge.TargetNodeGuid);
            if (NextNodeKeys.Contains(NextKey))
            {
                continue;
            }

            if (const FTraceNodeRecord* NextNode = State.NodesByKey.Find(NextKey))
            {
                NextNodeKeys.Add(NextKey);
                NextNodesJson.Add(MakeShared<FJsonValueObject>(BuildAdjacentNodeObject(*NextNode)));
            }
        }

        TSharedRef<FJsonObject> NodeObject = SerializeTraceNode(*Node);
        NodeObject->SetStringField(TEXT("matchReason"), Match.MatchReason);
        NodeObject->SetNumberField(TEXT("matchScore"), Match.Score);
        NodeObject->SetStringField(TEXT("roleSummary"), DescribeNodeRole(*Node));
        NodeObject->SetNumberField(TEXT("previousNodeCount"), PreviousNodesJson.Num());
        NodeObject->SetNumberField(TEXT("nextNodeCount"), NextNodesJson.Num());
        NodeObject->SetArrayField(TEXT("previousNodes"), PreviousNodesJson);
        NodeObject->SetArrayField(TEXT("nextNodes"), NextNodesJson);

        TSharedRef<FJsonObject> SuggestedForwardTrace = MakeShared<FJsonObject>();
        SuggestedForwardTrace->SetStringField(TEXT("objectPath"), ObjectPath);
        SuggestedForwardTrace->SetStringField(TEXT("startNodeQuery"), !Node->MemberName.IsEmpty() ? Node->MemberName : Node->NodeTitle);
        SuggestedForwardTrace->SetStringField(TEXT("graphName"), Node->GraphName);
        SuggestedForwardTrace->SetStringField(TEXT("direction"), TEXT("forward"));
        SuggestedForwardTrace->SetBoolField(TEXT("expandCalls"), true);
        SuggestedForwardTrace->SetBoolField(TEXT("followCrossBlueprintCalls"), true);
        SuggestedForwardTrace->SetNumberField(TEXT("maxCallDepth"), 1);
        SuggestedForwardTrace->SetStringField(TEXT("outputMode"), TEXT("summary"));
        NodeObject->SetObjectField(TEXT("suggestedForwardTrace"), SuggestedForwardTrace);

        TSharedRef<FJsonObject> SuggestedBackwardTrace = MakeShared<FJsonObject>();
        SuggestedBackwardTrace->SetStringField(TEXT("objectPath"), ObjectPath);
        SuggestedBackwardTrace->SetStringField(TEXT("startNodeQuery"), !Node->MemberName.IsEmpty() ? Node->MemberName : Node->NodeTitle);
        SuggestedBackwardTrace->SetStringField(TEXT("graphName"), Node->GraphName);
        SuggestedBackwardTrace->SetStringField(TEXT("direction"), TEXT("backward"));
        SuggestedBackwardTrace->SetBoolField(TEXT("expandCalls"), true);
        SuggestedBackwardTrace->SetBoolField(TEXT("followCrossBlueprintCalls"), true);
        SuggestedBackwardTrace->SetNumberField(TEXT("maxCallDepth"), 1);
        SuggestedBackwardTrace->SetStringField(TEXT("outputMode"), TEXT("summary"));
        NodeObject->SetObjectField(TEXT("suggestedBackwardTrace"), SuggestedBackwardTrace);

        MatchJson.Add(MakeShared<FJsonValueObject>(NodeObject));

        if (SummaryJson.Num() < 12)
        {
            SummaryJson.Add(MakeShared<FJsonValueString>(
                FString::Printf(TEXT("%s [%s] in %s"), *Node->NodeTitle, *Node->NodeType, *Node->GraphName)));
        }
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("source"), TEXT("cached_index"));
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("query"), Query);
    Result->SetStringField(TEXT("graphName"), GraphNameFilter);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetNumberField(TEXT("matchCount"), MatchJson.Num());
    Result->SetObjectField(TEXT("asset"), BlueprintAsset.ToSharedRef());
    Result->SetStringField(
        TEXT("summaryText"),
        MatchJson.Num() > 0
            ? FString::Printf(TEXT("Found %d matching Blueprint node(s) in %s."), MatchJson.Num(), *ObjectPath)
            : TEXT("No matching Blueprint nodes were found."));
    Result->SetArrayField(TEXT("summaryNodes"), SummaryJson);
    Result->SetArrayField(TEXT("matches"), MatchJson);
    Result->SetStringField(TEXT("nextStepHint"), TEXT("Use suggestedForwardTrace or suggestedBackwardTrace when you want the broader execution path around one inspected node."));
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FInspectBlueprintNodeTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Blueprint asset object path, for example /Game/MyFolder/BP_Door.BP_Door."));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);

    TSharedRef<FJsonObject> QueryProperty = MakeShared<FJsonObject>();
    QueryProperty->SetStringField(TEXT("type"), TEXT("string"));
    QueryProperty->SetStringField(TEXT("description"), TEXT("Node search query matched against node title, member name, node name, graph name, or node type."));
    Properties->SetObjectField(TEXT("query"), QueryProperty);

    TSharedRef<FJsonObject> NodeGuidProperty = MakeShared<FJsonObject>();
    NodeGuidProperty->SetStringField(TEXT("type"), TEXT("string"));
    NodeGuidProperty->SetStringField(TEXT("description"), TEXT("Optional exact Blueprint node GUID. Use this when you already know the node identity."));
    Properties->SetObjectField(TEXT("nodeGuid"), NodeGuidProperty);

    TSharedRef<FJsonObject> GraphNameProperty = MakeShared<FJsonObject>();
    GraphNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    GraphNameProperty->SetStringField(TEXT("description"), TEXT("Optional Blueprint graph name filter, such as EventGraph or a function graph name."));
    Properties->SetObjectField(TEXT("graphName"), GraphNameProperty);

    TSharedRef<FJsonObject> LimitProperty = MakeShared<FJsonObject>();
    LimitProperty->SetStringField(TEXT("type"), TEXT("integer"));
    LimitProperty->SetStringField(TEXT("description"), TEXT("Optional maximum number of matching nodes to return. Defaults to 5."));
    Properties->SetObjectField(TEXT("limit"), LimitProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
