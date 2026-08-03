#include "Tools/FindBlueprintNodeReferencesTool.h"

#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    struct FReferenceMatch
    {
        FString ReferencerObjectPath;
        FString ReferencerAssetName;
        FString ReferencerPackageName;
        FString ReferencerContentScope;
        FString GraphName;
        FString NodeGuid;
        FString NodeTitle;
        FString NodeType;
        FString MemberName;
        FString MemberParentPath;
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

    bool GetOptionalBoolField(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool bDefaultValue)
    {
        bool bValue = bDefaultValue;
        if (Params.IsValid())
        {
            Params->TryGetBoolField(FieldName, bValue);
        }
        return bValue;
    }

    void ScoreReferenceMatch(
        FReferenceMatch& Match,
        const FString& RootObjectPath,
        const FString& RootGeneratedClassPath,
        const FString& RequestedMemberName,
        const FString& RequestedNodeGuid,
        const FString& RequestedQuery)
    {
        if (!RequestedNodeGuid.IsEmpty() && Match.NodeGuid.Equals(RequestedNodeGuid, ESearchCase::IgnoreCase))
        {
            Match.Score += 220;
            Match.MatchReason = TEXT("exact_node_guid_match");
            return;
        }

        if (!RequestedMemberName.IsEmpty() && Match.MemberName.Equals(RequestedMemberName, ESearchCase::IgnoreCase))
        {
            Match.Score += 160;
            Match.MatchReason = TEXT("exact_member_name_match");
        }
        else if (!RequestedQuery.IsEmpty() && Match.NodeTitle.Contains(RequestedQuery, ESearchCase::IgnoreCase))
        {
            Match.Score += 120;
            Match.MatchReason = TEXT("node_title_contains_query");
        }
        else if (!RequestedQuery.IsEmpty() && Match.MemberName.Contains(RequestedQuery, ESearchCase::IgnoreCase))
        {
            Match.Score += 116;
            Match.MatchReason = TEXT("member_name_contains_query");
        }
        else if (!RequestedQuery.IsEmpty() && Match.GraphName.Contains(RequestedQuery, ESearchCase::IgnoreCase))
        {
            Match.Score += 64;
            Match.MatchReason = TEXT("graph_name_contains_query");
        }

        const bool bCanUseParentClassFallback = RequestedMemberName.IsEmpty() && RequestedQuery.IsEmpty() && !RequestedNodeGuid.IsEmpty();
        if (bCanUseParentClassFallback && !RootGeneratedClassPath.IsEmpty() && Match.MemberParentPath.Equals(RootGeneratedClassPath, ESearchCase::CaseSensitive))
        {
            Match.Score += 50;
            if (Match.MatchReason.IsEmpty())
            {
                Match.MatchReason = TEXT("member_parent_class_match");
            }
        }

        if (!Match.ReferencerObjectPath.Equals(RootObjectPath, ESearchCase::CaseSensitive))
        {
            Match.Score += 16;
        }
    }
}

FFindBlueprintNodeReferencesTool::FFindBlueprintNodeReferencesTool()
    : FMCPToolBase(TEXT("FindBlueprintNodeReferences"), TEXT("Finds indexed Blueprint call sites that reference a Blueprint node member, function, or event across the project."))
{
}

UnrealMCP::FMCPResponse FFindBlueprintNodeReferencesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FString ObjectPath = GetOptionalStringField(Request.Params, TEXT("objectPath"));
    const FString Query = GetOptionalStringField(Request.Params, TEXT("query")).TrimStartAndEnd();
    const FString MemberName = GetOptionalStringField(Request.Params, TEXT("memberName")).TrimStartAndEnd();
    const FString NodeGuid = GetOptionalStringField(Request.Params, TEXT("nodeGuid")).TrimStartAndEnd();
    const FString GraphNameFilter = GetOptionalStringField(Request.Params, TEXT("graphName"));
    const int32 Limit = FMath::Clamp(UnrealMCP::AssetRegistryToolUtils::GetOptionalLimit(Request.Params, 20), 1, 100);
    const bool bIncludeSameBlueprint = GetOptionalBoolField(Request.Params, TEXT("includeSameBlueprint"), true);

    if (ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindBlueprintNodeReferences requires params.objectPath."));
    }

    if (Query.IsEmpty() && MemberName.IsEmpty() && NodeGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindBlueprintNodeReferences requires params.query, params.memberName, or params.nodeGuid."));
    }

    TSharedPtr<FJsonObject> BlueprintAsset;
    FString RootGeneratedClassPath;
    TArray<FReferenceMatch> Matches;
    FString Error;

    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            if (!UnrealMCP::IndexedQueryToolUtils::QueryAssetByObjectPath(Database, ObjectPath, BlueprintAsset, OutExecError) || !BlueprintAsset.IsValid())
            {
                OutExecError = OutExecError.IsEmpty()
                    ? TEXT("FindBlueprintNodeReferences could not find the requested Blueprint in the project index.")
                    : OutExecError;
                return false;
            }

            bool bIsBlueprint = false;
            if (!BlueprintAsset->TryGetBoolField(TEXT("isBlueprint"), bIsBlueprint) || !bIsBlueprint)
            {
                OutExecError = TEXT("FindBlueprintNodeReferences requires params.objectPath to reference an indexed Blueprint asset.");
                return false;
            }

            {
                FSQLitePreparedStatement AssetStatement(
                    Database,
                    TEXT("SELECT generated_class_path FROM assets WHERE object_path = ?1;"),
                    ESQLitePreparedStatementFlags::None);
                if (!AssetStatement.IsValid() || !AssetStatement.SetBindingValueByIndex(1, ObjectPath))
                {
                    OutExecError = TEXT("FindBlueprintNodeReferences could not prepare the source asset lookup query.");
                    return false;
                }

                const int64 AssetQueryResult = AssetStatement.Execute([&](const FSQLitePreparedStatement& Row)
                {
                    if (!Row.GetColumnValueByIndex(0, RootGeneratedClassPath))
                    {
                        return ESQLitePreparedStatementExecuteRowResult::Error;
                    }
                    return ESQLitePreparedStatementExecuteRowResult::Stop;
                });

                if (AssetQueryResult == INDEX_NONE)
                {
                    OutExecError = Database.GetLastError().IsEmpty() ? TEXT("FindBlueprintNodeReferences source asset lookup failed.") : Database.GetLastError();
                    return false;
                }
            }

            FString Sql = TEXT(
                "SELECT caller.blueprint_object_path, asset.asset_name, asset.package_name, asset.content_scope, "
                "caller.graph_name, caller.node_guid, caller.node_title, caller.node_type, caller.member_name, caller.member_parent_path "
                "FROM blueprint_nodes caller "
                "LEFT JOIN assets asset ON asset.object_path = caller.blueprint_object_path "
                "WHERE caller.node_type = 'call_function' ");

            if (!GraphNameFilter.IsEmpty())
            {
                Sql += TEXT("AND caller.graph_name = ?4 ");
            }

            Sql += TEXT("AND (");
            if (!NodeGuid.IsEmpty() && MemberName.IsEmpty() && Query.IsEmpty())
            {
                Sql += TEXT("caller.member_parent_path = ?1");
            }
            else
            {
                Sql += TEXT(
                    "(?2 <> '' AND LOWER(caller.member_name) = LOWER(?2)) "
                    "OR (?3 <> '' AND (LOWER(caller.node_title) LIKE ?3 OR LOWER(caller.member_name) LIKE ?3))");
            }
            Sql += TEXT(") "
                "ORDER BY caller.blueprint_object_path ASC, caller.graph_name ASC, caller.node_title ASC, caller.node_guid ASC;");

            FSQLitePreparedStatement Statement(Database, *Sql, ESQLitePreparedStatementFlags::None);
            const FString QueryLikeValue = Query.IsEmpty() ? FString() : FString::Printf(TEXT("%%%s%%"), *Query.ToLower());
            if (!Statement.IsValid()
                || !Statement.SetBindingValueByIndex(1, RootGeneratedClassPath)
                || !Statement.SetBindingValueByIndex(2, MemberName)
                || !Statement.SetBindingValueByIndex(3, QueryLikeValue)
                || (!GraphNameFilter.IsEmpty() && !Statement.SetBindingValueByIndex(4, GraphNameFilter)))
            {
                OutExecError = TEXT("FindBlueprintNodeReferences could not prepare the reference query.");
                return false;
            }

            const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FReferenceMatch Match;
                if (!Row.GetColumnValueByIndex(0, Match.ReferencerObjectPath)
                    || !Row.GetColumnValueByIndex(1, Match.ReferencerAssetName)
                    || !Row.GetColumnValueByIndex(2, Match.ReferencerPackageName)
                    || !Row.GetColumnValueByIndex(3, Match.ReferencerContentScope)
                    || !Row.GetColumnValueByIndex(4, Match.GraphName)
                    || !Row.GetColumnValueByIndex(5, Match.NodeGuid)
                    || !Row.GetColumnValueByIndex(6, Match.NodeTitle)
                    || !Row.GetColumnValueByIndex(7, Match.NodeType)
                    || !Row.GetColumnValueByIndex(8, Match.MemberName)
                    || !Row.GetColumnValueByIndex(9, Match.MemberParentPath))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }

                if (!bIncludeSameBlueprint && Match.ReferencerObjectPath.Equals(ObjectPath, ESearchCase::CaseSensitive))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                }

                ScoreReferenceMatch(Match, ObjectPath, RootGeneratedClassPath, MemberName, NodeGuid, Query);
                if (Match.Score <= 0)
                {
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                }

                Matches.Add(MoveTemp(Match));
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });

            if (QueryResult == INDEX_NONE)
            {
                OutExecError = Database.GetLastError().IsEmpty() ? TEXT("FindBlueprintNodeReferences reference query failed.") : Database.GetLastError();
                return false;
            }

            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("FindBlueprintNodeReferences failed to query the project index: %s"), *Error));
    }

    Matches.Sort([](const FReferenceMatch& Left, const FReferenceMatch& Right)
    {
        if (Left.Score != Right.Score)
        {
            return Left.Score > Right.Score;
        }
        if (Left.ReferencerObjectPath != Right.ReferencerObjectPath)
        {
            return Left.ReferencerObjectPath < Right.ReferencerObjectPath;
        }
        if (Left.GraphName != Right.GraphName)
        {
            return Left.GraphName < Right.GraphName;
        }
        return Left.NodeTitle < Right.NodeTitle;
    });

    if (Matches.Num() > Limit)
    {
        Matches.SetNum(Limit);
    }

    TSet<FString> UniqueReferencerAssets;
    TArray<TSharedPtr<FJsonValue>> MatchesJson;
    TArray<TSharedPtr<FJsonValue>> SummaryJson;

    for (const FReferenceMatch& Match : Matches)
    {
        UniqueReferencerAssets.Add(Match.ReferencerObjectPath);

        TSharedRef<FJsonObject> MatchObject = MakeShared<FJsonObject>();
        MatchObject->SetStringField(TEXT("referencerObjectPath"), Match.ReferencerObjectPath);
        MatchObject->SetStringField(TEXT("referencerAssetName"), Match.ReferencerAssetName);
        MatchObject->SetStringField(TEXT("referencerPackageName"), Match.ReferencerPackageName);
        MatchObject->SetStringField(TEXT("referencerContentScope"), Match.ReferencerContentScope);
        MatchObject->SetStringField(TEXT("graphName"), Match.GraphName);
        MatchObject->SetStringField(TEXT("nodeGuid"), Match.NodeGuid);
        MatchObject->SetStringField(TEXT("nodeTitle"), Match.NodeTitle);
        MatchObject->SetStringField(TEXT("nodeType"), Match.NodeType);
        MatchObject->SetStringField(TEXT("memberName"), Match.MemberName);
        MatchObject->SetStringField(TEXT("memberParentPath"), Match.MemberParentPath);
        MatchObject->SetStringField(TEXT("matchReason"), Match.MatchReason);
        MatchObject->SetNumberField(TEXT("matchScore"), Match.Score);
        MatchObject->SetBoolField(TEXT("isSameBlueprint"), Match.ReferencerObjectPath.Equals(ObjectPath, ESearchCase::CaseSensitive));
        MatchObject->SetBoolField(TEXT("isCrossBlueprintReference"), !Match.ReferencerObjectPath.Equals(ObjectPath, ESearchCase::CaseSensitive));

        TSharedRef<FJsonObject> SuggestedInspectParams = MakeShared<FJsonObject>();
        SuggestedInspectParams->SetStringField(TEXT("objectPath"), Match.ReferencerObjectPath);
        SuggestedInspectParams->SetStringField(TEXT("nodeGuid"), Match.NodeGuid);
        MatchObject->SetObjectField(TEXT("suggestedInspectParams"), SuggestedInspectParams);

        TSharedRef<FJsonObject> SuggestedTraceParams = MakeShared<FJsonObject>();
        SuggestedTraceParams->SetStringField(TEXT("objectPath"), Match.ReferencerObjectPath);
        SuggestedTraceParams->SetStringField(TEXT("startNodeQuery"), !Match.MemberName.IsEmpty() ? Match.MemberName : Match.NodeTitle);
        SuggestedTraceParams->SetStringField(TEXT("graphName"), Match.GraphName);
        SuggestedTraceParams->SetStringField(TEXT("direction"), TEXT("forward"));
        SuggestedTraceParams->SetBoolField(TEXT("expandCalls"), true);
        SuggestedTraceParams->SetBoolField(TEXT("followCrossBlueprintCalls"), true);
        SuggestedTraceParams->SetNumberField(TEXT("maxCallDepth"), 1);
        SuggestedTraceParams->SetStringField(TEXT("outputMode"), TEXT("summary"));
        MatchObject->SetObjectField(TEXT("suggestedTraceParams"), SuggestedTraceParams);

        MatchesJson.Add(MakeShared<FJsonValueObject>(MatchObject));

        if (SummaryJson.Num() < 12)
        {
            SummaryJson.Add(MakeShared<FJsonValueString>(
                FString::Printf(TEXT("%s -> %s (%s)"), *Match.ReferencerAssetName, *Match.NodeTitle, *Match.GraphName)));
        }
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("query"), Query);
    Result->SetStringField(TEXT("memberName"), MemberName);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetStringField(TEXT("graphName"), GraphNameFilter);
    Result->SetBoolField(TEXT("includeSameBlueprint"), bIncludeSameBlueprint);
    Result->SetObjectField(TEXT("asset"), BlueprintAsset.ToSharedRef());
    Result->SetNumberField(TEXT("referenceCount"), Matches.Num());
    Result->SetNumberField(TEXT("uniqueReferencerBlueprintCount"), UniqueReferencerAssets.Num());
    Result->SetStringField(
        TEXT("summaryText"),
        Matches.Num() > 0
            ? FString::Printf(TEXT("Found %d reference call site(s) across %d Blueprint asset(s)."), Matches.Num(), UniqueReferencerAssets.Num())
            : TEXT("No matching Blueprint call-site references were found."));
    Result->SetArrayField(TEXT("summaryReferences"), SummaryJson);
    Result->SetArrayField(TEXT("references"), MatchesJson);
    Result->SetStringField(TEXT("nextStepHint"), TEXT("Use suggestedInspectParams to inspect one caller locally, or suggestedTraceParams to follow the execution path starting from that reference site."));
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindBlueprintNodeReferencesTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ObjectPathProperty = MakeShared<FJsonObject>();
    ObjectPathProperty->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathProperty->SetStringField(TEXT("description"), TEXT("Blueprint asset object path that owns the target function, event, or node member."));
    Properties->SetObjectField(TEXT("objectPath"), ObjectPathProperty);

    TSharedRef<FJsonObject> QueryProperty = MakeShared<FJsonObject>();
    QueryProperty->SetStringField(TEXT("type"), TEXT("string"));
    QueryProperty->SetStringField(TEXT("description"), TEXT("Optional fuzzy lookup text matched against node titles, member names, and graph names."));
    Properties->SetObjectField(TEXT("query"), QueryProperty);

    TSharedRef<FJsonObject> MemberNameProperty = MakeShared<FJsonObject>();
    MemberNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    MemberNameProperty->SetStringField(TEXT("description"), TEXT("Optional exact member name such as SendGoldEvent or RecieveSomeGold."));
    Properties->SetObjectField(TEXT("memberName"), MemberNameProperty);

    TSharedRef<FJsonObject> NodeGuidProperty = MakeShared<FJsonObject>();
    NodeGuidProperty->SetStringField(TEXT("type"), TEXT("string"));
    NodeGuidProperty->SetStringField(TEXT("description"), TEXT("Optional exact node GUID when you want to target a known node identity."));
    Properties->SetObjectField(TEXT("nodeGuid"), NodeGuidProperty);

    TSharedRef<FJsonObject> GraphNameProperty = MakeShared<FJsonObject>();
    GraphNameProperty->SetStringField(TEXT("type"), TEXT("string"));
    GraphNameProperty->SetStringField(TEXT("description"), TEXT("Optional caller graph name filter."));
    Properties->SetObjectField(TEXT("graphName"), GraphNameProperty);

    TSharedRef<FJsonObject> IncludeSameBlueprintProperty = MakeShared<FJsonObject>();
    IncludeSameBlueprintProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    IncludeSameBlueprintProperty->SetStringField(TEXT("description"), TEXT("Optional. When false, only return references from other Blueprints."));
    Properties->SetObjectField(TEXT("includeSameBlueprint"), IncludeSameBlueprintProperty);

    TSharedRef<FJsonObject> LimitProperty = MakeShared<FJsonObject>();
    LimitProperty->SetStringField(TEXT("type"), TEXT("integer"));
    LimitProperty->SetStringField(TEXT("description"), TEXT("Optional maximum number of references to return. Defaults to 20."));
    Properties->SetObjectField(TEXT("limit"), LimitProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
