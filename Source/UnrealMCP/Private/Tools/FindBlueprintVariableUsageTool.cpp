#include "Tools/FindBlueprintVariableUsageTool.h"

#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    struct FVariableUsageNode
    {
        FString GraphName;
        FString NodeGuid;
        FString NodeName;
        FString NodeTitle;
        FString NodeType;
        FString MemberName;
        FString MatchReason;
        int32 InputPinCount = 0;
        int32 OutputPinCount = 0;
        int32 IncomingConnectionCount = 0;
        int32 OutgoingConnectionCount = 0;
    };

    FString MakeVariableNodeKey(const FString& GraphName, const FString& NodeGuid)
    {
        return GraphName + TEXT("::") + NodeGuid;
    }

    FString GetRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName)
    {
        FString Value;
        if (Params.IsValid())
        {
            Params->TryGetStringField(FieldName, Value);
        }
        return Value;
    }

    bool MatchesVariable(const FString& Value, const FString& VariableName)
    {
        return !Value.IsEmpty() && !VariableName.IsEmpty() && Value.Contains(VariableName, ESearchCase::IgnoreCase);
    }
}

FFindBlueprintVariableUsageTool::FFindBlueprintVariableUsageTool()
    : FMCPToolBase(TEXT("FindBlueprintVariableUsage"), TEXT("Finds indexed Blueprint nodes that appear to read or write a variable, and summarizes their local connection counts."))
{
}

UnrealMCP::FMCPResponse FFindBlueprintVariableUsageTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FString ObjectPath = GetRequiredString(Request.Params, TEXT("objectPath"));
    const FString VariableName = GetRequiredString(Request.Params, TEXT("variableName"));
    const FString GraphNameFilter = GetRequiredString(Request.Params, TEXT("graphName"));

    if (ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindBlueprintVariableUsage requires params.objectPath."));
    }

    if (VariableName.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindBlueprintVariableUsage requires params.variableName."));
    }

    TSharedPtr<FJsonObject> BlueprintAsset;
    TMap<FString, FVariableUsageNode> UsageNodesByKey;
    TMap<FString, int32> IncomingCountsByNodeKey;
    TMap<FString, int32> OutgoingCountsByNodeKey;
    TMap<FString, FString> PinMatchReasonByNodeKey;

    FString Error;
    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            if (!UnrealMCP::IndexedQueryToolUtils::QueryAssetByObjectPath(Database, ObjectPath, BlueprintAsset, OutExecError) || !BlueprintAsset.IsValid())
            {
                OutExecError = OutExecError.IsEmpty()
                    ? TEXT("FindBlueprintVariableUsage could not find the requested Blueprint in the project index.")
                    : OutExecError;
                return false;
            }

            bool bIsBlueprint = false;
            if (!BlueprintAsset->TryGetBoolField(TEXT("isBlueprint"), bIsBlueprint) || !bIsBlueprint)
            {
                OutExecError = TEXT("FindBlueprintVariableUsage requires params.objectPath to reference an indexed Blueprint asset.");
                return false;
            }

            {
                FString Sql = TEXT("SELECT DISTINCT graph_name, node_guid, pin_name FROM blueprint_pins WHERE blueprint_object_path = ?1 AND pin_name LIKE ?2");
                if (!GraphNameFilter.IsEmpty())
                {
                    Sql += TEXT(" AND graph_name = ?3");
                }
                Sql += TEXT(" ORDER BY graph_name ASC, node_guid ASC;");

                FSQLitePreparedStatement Statement(Database, *Sql, ESQLitePreparedStatementFlags::None);
                const FString LikeValue = FString::Printf(TEXT("%%%s%%"), *VariableName);
                if (!Statement.IsValid()
                    || !Statement.SetBindingValueByIndex(1, ObjectPath)
                    || !Statement.SetBindingValueByIndex(2, LikeValue)
                    || (!GraphNameFilter.IsEmpty() && !Statement.SetBindingValueByIndex(3, GraphNameFilter)))
                {
                    OutExecError = TEXT("FindBlueprintVariableUsage could not prepare the pin query.");
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

                    PinMatchReasonByNodeKey.Add(MakeVariableNodeKey(GraphName, NodeGuid), FString::Printf(TEXT("pin_name:%s"), *PinName));
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                });

                if (QueryResult == INDEX_NONE)
                {
                    OutExecError = Database.GetLastError().IsEmpty() ? TEXT("FindBlueprintVariableUsage pin query failed.") : Database.GetLastError();
                    return false;
                }
            }

            {
                FString Sql = TEXT("SELECT graph_name, node_guid, node_name, node_title, node_type, member_name, input_pin_count, output_pin_count "
                                   "FROM blueprint_nodes WHERE blueprint_object_path = ?1");
                if (!GraphNameFilter.IsEmpty())
                {
                    Sql += TEXT(" AND graph_name = ?2");
                }
                Sql += TEXT(" ORDER BY graph_name ASC, node_guid ASC;");

                FSQLitePreparedStatement Statement(Database, *Sql, ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid()
                    || !Statement.SetBindingValueByIndex(1, ObjectPath)
                    || (!GraphNameFilter.IsEmpty() && !Statement.SetBindingValueByIndex(2, GraphNameFilter)))
                {
                    OutExecError = TEXT("FindBlueprintVariableUsage could not prepare the node query.");
                    return false;
                }

                const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
                {
                    FVariableUsageNode Node;
                    if (!Row.GetColumnValueByIndex(0, Node.GraphName)
                        || !Row.GetColumnValueByIndex(1, Node.NodeGuid)
                        || !Row.GetColumnValueByIndex(2, Node.NodeName)
                        || !Row.GetColumnValueByIndex(3, Node.NodeTitle)
                        || !Row.GetColumnValueByIndex(4, Node.NodeType)
                        || !Row.GetColumnValueByIndex(5, Node.MemberName)
                        || !Row.GetColumnValueByIndex(6, Node.InputPinCount)
                        || !Row.GetColumnValueByIndex(7, Node.OutputPinCount))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }

                    const FString NodeKey = MakeVariableNodeKey(Node.GraphName, Node.NodeGuid);
                    if (MatchesVariable(Node.MemberName, VariableName))
                    {
                        Node.MatchReason = TEXT("member_name");
                    }
                    else if (Node.NodeType == TEXT("variable_get") || Node.NodeType == TEXT("variable_set"))
                    {
                        if (MatchesVariable(Node.NodeTitle, VariableName))
                        {
                            Node.MatchReason = TEXT("node_title");
                        }
                        else if (MatchesVariable(Node.NodeName, VariableName))
                        {
                            Node.MatchReason = TEXT("node_name");
                        }
                    }
                    else if (MatchesVariable(Node.NodeTitle, VariableName))
                    {
                        Node.MatchReason = TEXT("node_title");
                    }
                    else if (MatchesVariable(Node.NodeName, VariableName))
                    {
                        Node.MatchReason = TEXT("node_name");
                    }
                    else if (const FString* PinMatchReason = PinMatchReasonByNodeKey.Find(NodeKey))
                    {
                        Node.MatchReason = *PinMatchReason;
                    }

                    if (!Node.MatchReason.IsEmpty())
                    {
                        UsageNodesByKey.Add(NodeKey, MoveTemp(Node));
                    }

                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                });

                if (QueryResult == INDEX_NONE)
                {
                    OutExecError = Database.GetLastError().IsEmpty() ? TEXT("FindBlueprintVariableUsage node query failed.") : Database.GetLastError();
                    return false;
                }
            }

            if (UsageNodesByKey.Num() == 0)
            {
                return true;
            }

            {
                FString Sql = TEXT("SELECT source_graph_name, source_node_guid, target_graph_name, target_node_guid "
                                   "FROM blueprint_edges WHERE blueprint_object_path = ?1");
                if (!GraphNameFilter.IsEmpty())
                {
                    Sql += TEXT(" AND source_graph_name = ?2 AND target_graph_name = ?2");
                }
                Sql += TEXT(";");

                FSQLitePreparedStatement Statement(Database, *Sql, ESQLitePreparedStatementFlags::None);
                if (!Statement.IsValid()
                    || !Statement.SetBindingValueByIndex(1, ObjectPath)
                    || (!GraphNameFilter.IsEmpty() && !Statement.SetBindingValueByIndex(2, GraphNameFilter)))
                {
                    OutExecError = TEXT("FindBlueprintVariableUsage could not prepare the edge query.");
                    return false;
                }

                const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
                {
                    FString SourceGraphName;
                    FString SourceNodeGuid;
                    FString TargetGraphName;
                    FString TargetNodeGuid;
                    if (!Row.GetColumnValueByIndex(0, SourceGraphName)
                        || !Row.GetColumnValueByIndex(1, SourceNodeGuid)
                        || !Row.GetColumnValueByIndex(2, TargetGraphName)
                        || !Row.GetColumnValueByIndex(3, TargetNodeGuid))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }

                    const FString SourceKey = MakeVariableNodeKey(SourceGraphName, SourceNodeGuid);
                    const FString TargetKey = MakeVariableNodeKey(TargetGraphName, TargetNodeGuid);
                    if (OutgoingCountsByNodeKey.Contains(SourceKey))
                    {
                        OutgoingCountsByNodeKey[SourceKey] += 1;
                    }
                    else
                    {
                        OutgoingCountsByNodeKey.Add(SourceKey, 1);
                    }

                    if (IncomingCountsByNodeKey.Contains(TargetKey))
                    {
                        IncomingCountsByNodeKey[TargetKey] += 1;
                    }
                    else
                    {
                        IncomingCountsByNodeKey.Add(TargetKey, 1);
                    }

                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                });

                if (QueryResult == INDEX_NONE)
                {
                    OutExecError = Database.GetLastError().IsEmpty() ? TEXT("FindBlueprintVariableUsage edge query failed.") : Database.GetLastError();
                    return false;
                }
            }

            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("FindBlueprintVariableUsage failed to query the project index: %s"), *Error));
    }

    TArray<FVariableUsageNode> SortedUsages;
    UsageNodesByKey.GenerateValueArray(SortedUsages);
    SortedUsages.Sort([](const FVariableUsageNode& Left, const FVariableUsageNode& Right)
    {
        return Left.GraphName == Right.GraphName
            ? Left.NodeTitle < Right.NodeTitle
            : Left.GraphName < Right.GraphName;
    });

    TArray<TSharedPtr<FJsonValue>> UsageJson;
    for (FVariableUsageNode& Usage : SortedUsages)
    {
        const FString NodeKey = MakeVariableNodeKey(Usage.GraphName, Usage.NodeGuid);
        Usage.IncomingConnectionCount = IncomingCountsByNodeKey.FindRef(NodeKey);
        Usage.OutgoingConnectionCount = OutgoingCountsByNodeKey.FindRef(NodeKey);

        TSharedRef<FJsonObject> UsageObject = MakeShared<FJsonObject>();
        UsageObject->SetStringField(TEXT("graphName"), Usage.GraphName);
        UsageObject->SetStringField(TEXT("nodeGuid"), Usage.NodeGuid);
        UsageObject->SetStringField(TEXT("nodeName"), Usage.NodeName);
        UsageObject->SetStringField(TEXT("nodeTitle"), Usage.NodeTitle);
        UsageObject->SetStringField(TEXT("nodeType"), Usage.NodeType);
        UsageObject->SetStringField(TEXT("memberName"), Usage.MemberName);
        UsageObject->SetStringField(TEXT("matchReason"), Usage.MatchReason);
        UsageObject->SetNumberField(TEXT("inputPinCount"), Usage.InputPinCount);
        UsageObject->SetNumberField(TEXT("outputPinCount"), Usage.OutputPinCount);
        UsageObject->SetNumberField(TEXT("incomingConnectionCount"), Usage.IncomingConnectionCount);
        UsageObject->SetNumberField(TEXT("outgoingConnectionCount"), Usage.OutgoingConnectionCount);
        UsageJson.Add(MakeShared<FJsonValueObject>(UsageObject));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("variableName"), VariableName);
    Result->SetStringField(TEXT("graphName"), GraphNameFilter);
    Result->SetObjectField(TEXT("asset"), BlueprintAsset.ToSharedRef());
    Result->SetNumberField(TEXT("usageCount"), UsageJson.Num());
    Result->SetArrayField(TEXT("usages"), UsageJson);
    Result->SetStringField(TEXT("nextStepHint"), TEXT("Use TraceBlueprintFlow with startNodeQuery set to one of these node titles or pin names for a larger execution trace."));
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindBlueprintVariableUsageTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Blueprint asset object path, for example /Game/MyFolder/BP_Door.BP_Door."));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);

    TSharedRef<FJsonObject> VariableNameProperty = MakeShared<FJsonObject>();
    VariableNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    VariableNameProperty->SetStringField(TEXT("description"), TEXT("Variable name or partial variable text to search for in the indexed Blueprint graph."));
    Properties->SetObjectField(TEXT("variableName"), VariableNameProperty);

    TSharedRef<FJsonObject> GraphNameProperty = MakeShared<FJsonObject>();
    GraphNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    GraphNameProperty->SetStringField(TEXT("description"), TEXT("Optional Blueprint graph name filter, such as EventGraph."));
    Properties->SetObjectField(TEXT("graphName"), GraphNameProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Required.Add(MakeShared<FJsonValueString>(TEXT("variableName")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
