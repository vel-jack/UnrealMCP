#include "Tools/FindBlueprintTraceStartPointsTool.h"

#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    struct FTraceStartPointRow
    {
        FString GraphName;
        FString NodeGuid;
        FString NodeName;
        FString NodeTitle;
        FString NodeType;
        FString MemberName;
        FString MemberParentPath;
        bool bIsEntry = false;
        int32 InputPinCount = 0;
        int32 OutputPinCount = 0;
        FString TargetObjectPath;
        FString TargetAssetName;
        FString TargetPackageName;
        FString TargetContentScope;
        int32 Score = 0;
        TArray<FString> Reasons;
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

    void AddReason(TArray<FString>& Reasons, const FString& Reason)
    {
        if (!Reason.IsEmpty())
        {
            Reasons.AddUnique(Reason);
        }
    }

    bool IsEntryLikeNodeType(const FString& NodeType)
    {
        return NodeType == TEXT("event")
            || NodeType == TEXT("custom_event")
            || NodeType == TEXT("function_entry");
    }

    int32 ScoreRow(FTraceStartPointRow& Row, const FString& ObjectPath, const FString& Query)
    {
        const FString LowerQuery = Query.ToLower();
        const FString LowerGraphName = Row.GraphName.ToLower();
        const FString LowerNodeTitle = Row.NodeTitle.ToLower();
        const FString LowerNodeName = Row.NodeName.ToLower();
        const FString LowerMemberName = Row.MemberName.ToLower();
        int32 Score = 0;

        if (Row.bIsEntry)
        {
            Score += 40;
            AddReason(Row.Reasons, TEXT("graph_entry_node"));
        }

        if (Row.NodeType == TEXT("custom_event"))
        {
            Score += 30;
            AddReason(Row.Reasons, TEXT("custom_event_entry"));
        }
        else if (Row.NodeType == TEXT("event"))
        {
            Score += 26;
            AddReason(Row.Reasons, TEXT("event_entry"));
        }
        else if (Row.NodeType == TEXT("function_entry"))
        {
            Score += 24;
            AddReason(Row.Reasons, TEXT("function_entry"));
        }
        else if (Row.NodeType == TEXT("call_function"))
        {
            Score += 18;
            AddReason(Row.Reasons, TEXT("function_call_site"));
        }

        if (!Row.TargetObjectPath.IsEmpty() && !Row.TargetObjectPath.Equals(ObjectPath, ESearchCase::CaseSensitive))
        {
            Score += 36;
            AddReason(Row.Reasons, TEXT("cross_blueprint_call"));
        }

        if (Row.OutputPinCount > 0)
        {
            Score += FMath::Min(Row.OutputPinCount * 3, 12);
            AddReason(Row.Reasons, TEXT("has_outgoing_flow"));
        }

        if (LowerGraphName == TEXT("eventgraph"))
        {
            Score += 12;
            AddReason(Row.Reasons, TEXT("event_graph"));
        }

        if (!LowerQuery.IsEmpty())
        {
            if (LowerNodeTitle == LowerQuery)
            {
                Score += 120;
                AddReason(Row.Reasons, TEXT("exact_node_title_match"));
            }
            else if (LowerMemberName == LowerQuery)
            {
                Score += 110;
                AddReason(Row.Reasons, TEXT("exact_member_name_match"));
            }
            else if (LowerNodeTitle.StartsWith(LowerQuery))
            {
                Score += 92;
                AddReason(Row.Reasons, TEXT("node_title_prefix_match"));
            }
            else if (LowerMemberName.StartsWith(LowerQuery))
            {
                Score += 88;
                AddReason(Row.Reasons, TEXT("member_name_prefix_match"));
            }
            else if (LowerNodeTitle.Contains(LowerQuery))
            {
                Score += 72;
                AddReason(Row.Reasons, TEXT("node_title_contains_query"));
            }
            else if (LowerMemberName.Contains(LowerQuery))
            {
                Score += 68;
                AddReason(Row.Reasons, TEXT("member_name_contains_query"));
            }
            else if (LowerNodeName.Contains(LowerQuery))
            {
                Score += 48;
                AddReason(Row.Reasons, TEXT("node_name_contains_query"));
            }
            else if (LowerGraphName.Contains(LowerQuery))
            {
                Score += 24;
                AddReason(Row.Reasons, TEXT("graph_name_contains_query"));
            }
        }

        return Score;
    }

    bool ShouldKeepRow(
        const FTraceStartPointRow& Row,
        const FString& ObjectPath,
        const FString& Query,
        const bool bIncludeEvents,
        const bool bIncludeFunctionEntries,
        const bool bIncludeCallSites,
        const bool bCrossBlueprintOnly)
    {
        const bool bIsCrossBlueprintCall = !Row.TargetObjectPath.IsEmpty()
            && !Row.TargetObjectPath.Equals(ObjectPath, ESearchCase::CaseSensitive);
        if (bCrossBlueprintOnly && !bIsCrossBlueprintCall)
        {
            return false;
        }

        if (Row.NodeType == TEXT("event") || Row.NodeType == TEXT("custom_event"))
        {
            return bIncludeEvents;
        }

        if (Row.NodeType == TEXT("function_entry"))
        {
            return bIncludeFunctionEntries;
        }

        if (Row.NodeType == TEXT("call_function"))
        {
            if (!bIncludeCallSites)
            {
                return false;
            }

            if (!Query.IsEmpty())
            {
                return true;
            }

            return bIsCrossBlueprintCall;
        }

        return false;
    }
}

FFindBlueprintTraceStartPointsTool::FFindBlueprintTraceStartPointsTool()
    : FMCPToolBase(TEXT("FindBlueprintTraceStartPoints"), TEXT("Finds likely indexed Blueprint graph nodes to use as trace starting points, ranked for tracing workflows."))
{
}

UnrealMCP::FMCPResponse FFindBlueprintTraceStartPointsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FString ObjectPath = GetOptionalStringField(Request.Params, TEXT("objectPath"));
    const FString Query = GetOptionalStringField(Request.Params, TEXT("query")).TrimStartAndEnd();
    const FString GraphNameFilter = GetOptionalStringField(Request.Params, TEXT("graphName"));
    const int32 Limit = FMath::Clamp(UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params, 12), 1, 50);
    const bool bIncludeEvents = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("includeEvents"), true);
    const bool bIncludeFunctionEntries = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("includeFunctionEntries"), true);
    const bool bIncludeCallSites = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("includeCallSites"), true);
    const bool bCrossBlueprintOnly = UnrealMCP::AssetRegistryToolUtils::GetOptionalBool(Request.Params, TEXT("crossBlueprintOnly"), false);

    if (ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindBlueprintTraceStartPoints requires params.objectPath."));
    }

    if (!bIncludeEvents && !bIncludeFunctionEntries && !bIncludeCallSites)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindBlueprintTraceStartPoints requires at least one of includeEvents, includeFunctionEntries, or includeCallSites to be true."));
    }

    TSharedPtr<FJsonObject> BlueprintAsset;
    TArray<FTraceStartPointRow> StartPoints;
    FString Error;

    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            if (!UnrealMCP::IndexedQueryToolUtils::QueryAssetByObjectPath(Database, ObjectPath, BlueprintAsset, OutExecError) || !BlueprintAsset.IsValid())
            {
                OutExecError = OutExecError.IsEmpty()
                    ? TEXT("FindBlueprintTraceStartPoints could not find the requested Blueprint in the project index.")
                    : OutExecError;
                return false;
            }

            bool bIsBlueprint = false;
            if (!BlueprintAsset->TryGetBoolField(TEXT("isBlueprint"), bIsBlueprint) || !bIsBlueprint)
            {
                OutExecError = TEXT("FindBlueprintTraceStartPoints requires params.objectPath to reference an indexed Blueprint asset.");
                return false;
            }

            FString Sql = TEXT(
                "SELECT n.graph_name, n.node_guid, n.node_name, n.node_title, n.node_type, n.member_name, n.member_parent_path, "
                "CASE WHEN n.node_type IN ('event', 'custom_event', 'function_entry') THEN 1 ELSE 0 END AS is_entry, "
                "n.input_pin_count, n.output_pin_count, "
                "COALESCE(a.object_path, ''), COALESCE(a.asset_name, ''), COALESCE(a.package_name, ''), COALESCE(a.content_scope, '') "
                "FROM blueprint_nodes n "
                "LEFT JOIN assets a ON a.generated_class_path = n.member_parent_path "
                "WHERE n.blueprint_object_path = ?1 ");

            if (!GraphNameFilter.IsEmpty())
            {
                Sql += TEXT("AND n.graph_name = ?2 ");
            }

            Sql += TEXT("AND n.node_type IN ('event', 'custom_event', 'function_entry', 'call_function') ");
            Sql += TEXT("ORDER BY n.graph_name ASC, n.node_type ASC, n.node_title ASC, n.node_guid ASC;");

            FSQLitePreparedStatement Statement(Database, *Sql, ESQLitePreparedStatementFlags::None);
            if (!Statement.IsValid()
                || !Statement.SetBindingValueByIndex(1, ObjectPath)
                || (!GraphNameFilter.IsEmpty() && !Statement.SetBindingValueByIndex(2, GraphNameFilter)))
            {
                OutExecError = TEXT("FindBlueprintTraceStartPoints could not prepare the node query.");
                return false;
            }

            const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FTraceStartPointRow StartPoint;
                int32 bIsEntryInt = 0;

                if (!Row.GetColumnValueByIndex(0, StartPoint.GraphName)
                    || !Row.GetColumnValueByIndex(1, StartPoint.NodeGuid)
                    || !Row.GetColumnValueByIndex(2, StartPoint.NodeName)
                    || !Row.GetColumnValueByIndex(3, StartPoint.NodeTitle)
                    || !Row.GetColumnValueByIndex(4, StartPoint.NodeType)
                    || !Row.GetColumnValueByIndex(5, StartPoint.MemberName)
                    || !Row.GetColumnValueByIndex(6, StartPoint.MemberParentPath)
                    || !Row.GetColumnValueByIndex(7, bIsEntryInt)
                    || !Row.GetColumnValueByIndex(8, StartPoint.InputPinCount)
                    || !Row.GetColumnValueByIndex(9, StartPoint.OutputPinCount)
                    || !Row.GetColumnValueByIndex(10, StartPoint.TargetObjectPath)
                    || !Row.GetColumnValueByIndex(11, StartPoint.TargetAssetName)
                    || !Row.GetColumnValueByIndex(12, StartPoint.TargetPackageName)
                    || !Row.GetColumnValueByIndex(13, StartPoint.TargetContentScope))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }

                StartPoint.bIsEntry = bIsEntryInt != 0;

                if (!ShouldKeepRow(StartPoint, ObjectPath, Query, bIncludeEvents, bIncludeFunctionEntries, bIncludeCallSites, bCrossBlueprintOnly))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                }

                StartPoint.Score = ScoreRow(StartPoint, ObjectPath, Query);
                if (StartPoint.Score <= 0)
                {
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                }

                StartPoints.Add(MoveTemp(StartPoint));
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });

            if (QueryResult == INDEX_NONE)
            {
                OutExecError = Database.GetLastError().IsEmpty() ? TEXT("FindBlueprintTraceStartPoints node query failed.") : Database.GetLastError();
                return false;
            }

            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("FindBlueprintTraceStartPoints failed to query the project index: %s"), *Error));
    }

    StartPoints.Sort([](const FTraceStartPointRow& A, const FTraceStartPointRow& B)
    {
        if (A.Score != B.Score)
        {
            return A.Score > B.Score;
        }
        if (A.GraphName != B.GraphName)
        {
            return A.GraphName < B.GraphName;
        }
        if (A.NodeTitle != B.NodeTitle)
        {
            return A.NodeTitle < B.NodeTitle;
        }
        return A.NodeGuid < B.NodeGuid;
    });

    if (StartPoints.Num() > Limit)
    {
        StartPoints.SetNum(Limit);
    }

    TArray<TSharedPtr<FJsonValue>> StartPointsJson;
    TArray<TSharedPtr<FJsonValue>> SummaryLinesJson;

    for (const FTraceStartPointRow& StartPoint : StartPoints)
    {
        TSharedRef<FJsonObject> StartPointObject = MakeShared<FJsonObject>();
        StartPointObject->SetStringField(TEXT("graphName"), StartPoint.GraphName);
        StartPointObject->SetStringField(TEXT("nodeGuid"), StartPoint.NodeGuid);
        StartPointObject->SetStringField(TEXT("nodeName"), StartPoint.NodeName);
        StartPointObject->SetStringField(TEXT("nodeTitle"), StartPoint.NodeTitle);
        StartPointObject->SetStringField(TEXT("nodeType"), StartPoint.NodeType);
        StartPointObject->SetStringField(TEXT("memberName"), StartPoint.MemberName);
        StartPointObject->SetBoolField(TEXT("isEntry"), StartPoint.bIsEntry);
        StartPointObject->SetNumberField(TEXT("inputPinCount"), StartPoint.InputPinCount);
        StartPointObject->SetNumberField(TEXT("outputPinCount"), StartPoint.OutputPinCount);
        StartPointObject->SetNumberField(TEXT("score"), StartPoint.Score);
        StartPointObject->SetBoolField(TEXT("isCrossBlueprintReference"), !StartPoint.TargetObjectPath.IsEmpty() && !StartPoint.TargetObjectPath.Equals(ObjectPath, ESearchCase::CaseSensitive));

        TArray<TSharedPtr<FJsonValue>> ReasonValues;
        for (const FString& Reason : StartPoint.Reasons)
        {
            ReasonValues.Add(MakeShared<FJsonValueString>(Reason));
        }
        StartPointObject->SetArrayField(TEXT("reasons"), ReasonValues);

        if (!StartPoint.TargetObjectPath.IsEmpty())
        {
            TSharedRef<FJsonObject> TargetObject = MakeShared<FJsonObject>();
            TargetObject->SetStringField(TEXT("objectPath"), StartPoint.TargetObjectPath);
            TargetObject->SetStringField(TEXT("assetName"), StartPoint.TargetAssetName);
            TargetObject->SetStringField(TEXT("packageName"), StartPoint.TargetPackageName);
            TargetObject->SetStringField(TEXT("contentScope"), StartPoint.TargetContentScope);
            StartPointObject->SetObjectField(TEXT("targetBlueprint"), TargetObject);
        }

        TSharedRef<FJsonObject> SuggestedTraceParams = MakeShared<FJsonObject>();
        SuggestedTraceParams->SetStringField(TEXT("objectPath"), ObjectPath);
        SuggestedTraceParams->SetStringField(TEXT("startNodeQuery"), !StartPoint.MemberName.IsEmpty() ? StartPoint.MemberName : StartPoint.NodeTitle);
        SuggestedTraceParams->SetStringField(TEXT("graphName"), StartPoint.GraphName);
        SuggestedTraceParams->SetStringField(TEXT("direction"), TEXT("forward"));
        SuggestedTraceParams->SetBoolField(TEXT("expandCalls"), true);
        SuggestedTraceParams->SetBoolField(TEXT("followCrossBlueprintCalls"), true);
        SuggestedTraceParams->SetNumberField(TEXT("maxCallDepth"), 1);
        SuggestedTraceParams->SetStringField(TEXT("outputMode"), TEXT("summary"));
        StartPointObject->SetObjectField(TEXT("suggestedTraceParams"), SuggestedTraceParams);

        StartPointsJson.Add(MakeShared<FJsonValueObject>(StartPointObject));

        if (SummaryLinesJson.Num() < 12)
        {
            const FString Label = !StartPoint.MemberName.IsEmpty() ? StartPoint.MemberName : StartPoint.NodeTitle;
            SummaryLinesJson.Add(MakeShared<FJsonValueString>(
                FString::Printf(TEXT("%s [%s] in %s"), *Label, *StartPoint.NodeType, *StartPoint.GraphName)));
        }
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("query"), Query);
    Result->SetStringField(TEXT("graphName"), GraphNameFilter);
    Result->SetNumberField(TEXT("startPointCount"), StartPoints.Num());
    Result->SetBoolField(TEXT("includeEvents"), bIncludeEvents);
    Result->SetBoolField(TEXT("includeFunctionEntries"), bIncludeFunctionEntries);
    Result->SetBoolField(TEXT("includeCallSites"), bIncludeCallSites);
    Result->SetBoolField(TEXT("crossBlueprintOnly"), bCrossBlueprintOnly);
    Result->SetObjectField(TEXT("asset"), BlueprintAsset.ToSharedRef());
    Result->SetStringField(
        TEXT("summaryText"),
        Query.IsEmpty()
            ? FString::Printf(TEXT("Found %d likely trace starting point(s) for %s."), StartPoints.Num(), *ObjectPath)
            : FString::Printf(TEXT("Found %d likely trace starting point(s) matching '%s'."), StartPoints.Num(), *Query));
    Result->SetArrayField(TEXT("summaryStartPoints"), SummaryLinesJson);
    Result->SetArrayField(TEXT("startPoints"), StartPointsJson);
    Result->SetStringField(TEXT("nextStepHint"), TEXT("Pick one suggestedTraceParams block and use it with TraceBlueprintFlow when you want the detailed execution path."));

    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindBlueprintTraceStartPointsTool::BuildInputSchema() const
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
    QueryProperty->SetStringField(TEXT("description"), TEXT("Optional query used to rank likely start nodes by node title, member name, node name, or graph name."));
    Properties->SetObjectField(TEXT("query"), QueryProperty);

    TSharedRef<FJsonObject> GraphNameProperty = MakeShared<FJsonObject>();
    GraphNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    GraphNameProperty->SetStringField(TEXT("description"), TEXT("Optional Blueprint graph name filter, such as EventGraph or a function graph name."));
    Properties->SetObjectField(TEXT("graphName"), GraphNameProperty);

    TSharedRef<FJsonObject> LimitProperty = MakeShared<FJsonObject>();
    LimitProperty->SetStringField(TEXT("type"), TEXT("integer"));
    LimitProperty->SetStringField(TEXT("description"), TEXT("Optional maximum number of ranked start points to return. Defaults to 12."));
    Properties->SetObjectField(TEXT("limit"), LimitProperty);

    TSharedRef<FJsonObject> IncludeEventsProperty = MakeShared<FJsonObject>();
    IncludeEventsProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    IncludeEventsProperty->SetStringField(TEXT("description"), TEXT("Optional. When true, include event and custom event nodes. Defaults to true."));
    Properties->SetObjectField(TEXT("includeEvents"), IncludeEventsProperty);

    TSharedRef<FJsonObject> IncludeFunctionEntriesProperty = MakeShared<FJsonObject>();
    IncludeFunctionEntriesProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    IncludeFunctionEntriesProperty->SetStringField(TEXT("description"), TEXT("Optional. When true, include function entry nodes. Defaults to true."));
    Properties->SetObjectField(TEXT("includeFunctionEntries"), IncludeFunctionEntriesProperty);

    TSharedRef<FJsonObject> IncludeCallSitesProperty = MakeShared<FJsonObject>();
    IncludeCallSitesProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    IncludeCallSitesProperty->SetStringField(TEXT("description"), TEXT("Optional. When true, include call-function nodes, especially useful for cross-Blueprint tracing. Defaults to true."));
    Properties->SetObjectField(TEXT("includeCallSites"), IncludeCallSitesProperty);

    TSharedRef<FJsonObject> CrossBlueprintOnlyProperty = MakeShared<FJsonObject>();
    CrossBlueprintOnlyProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    CrossBlueprintOnlyProperty->SetStringField(TEXT("description"), TEXT("Optional. When true, return only call sites that target another Blueprint asset."));
    Properties->SetObjectField(TEXT("crossBlueprintOnly"), CrossBlueprintOnlyProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
