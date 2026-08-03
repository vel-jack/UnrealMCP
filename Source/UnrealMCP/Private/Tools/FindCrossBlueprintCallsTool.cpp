#include "Tools/FindCrossBlueprintCallsTool.h"

#include "Dom/JsonObject.h"
#include "SQLitePreparedStatement.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    struct FCrossBlueprintCallRow
    {
        FString GraphName;
        FString NodeGuid;
        FString NodeTitle;
        FString MemberName;
        FString MemberParentPath;
        FString TargetObjectPath;
        FString TargetAssetName;
        FString TargetPackageName;
        FString TargetContentScope;
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
}

FFindCrossBlueprintCallsTool::FFindCrossBlueprintCallsTool()
    : FMCPToolBase(TEXT("FindCrossBlueprintCalls"), TEXT("Finds indexed Blueprint function call nodes that target other Blueprint assets, with compact or full output modes."))
{
}

UnrealMCP::FMCPResponse FFindCrossBlueprintCallsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FString ObjectPath = GetOptionalStringField(Request.Params, TEXT("objectPath"));
    const FString GraphNameFilter = GetOptionalStringField(Request.Params, TEXT("graphName"));
    FString OutputMode = GetOptionalStringField(Request.Params, TEXT("outputMode"));
    if (OutputMode.IsEmpty())
    {
        OutputMode = TEXT("summary");
    }

    const bool bIncludeSameBlueprintCalls = GetOptionalBoolField(Request.Params, TEXT("includeSameBlueprintCalls"), false);

    if (ObjectPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindCrossBlueprintCalls requires params.objectPath."));
    }

    if (OutputMode != TEXT("summary") && OutputMode != TEXT("compact") && OutputMode != TEXT("full"))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FindCrossBlueprintCalls params.outputMode must be summary, compact, or full."));
    }

    TSharedPtr<FJsonObject> BlueprintAsset;
    TArray<FCrossBlueprintCallRow> Calls;
    FString Error;

    const bool bQuerySucceeded = UnrealMCP::IndexedQueryToolUtils::ExecuteWithProjectIndex(
        [&](FSQLiteDatabase& Database, FString& OutExecError)
        {
            if (!UnrealMCP::IndexedQueryToolUtils::QueryAssetByObjectPath(Database, ObjectPath, BlueprintAsset, OutExecError) || !BlueprintAsset.IsValid())
            {
                OutExecError = OutExecError.IsEmpty()
                    ? TEXT("FindCrossBlueprintCalls could not find the requested Blueprint in the project index.")
                    : OutExecError;
                return false;
            }

            bool bIsBlueprint = false;
            if (!BlueprintAsset->TryGetBoolField(TEXT("isBlueprint"), bIsBlueprint) || !bIsBlueprint)
            {
                OutExecError = TEXT("FindCrossBlueprintCalls requires params.objectPath to reference an indexed Blueprint asset.");
                return false;
            }

            FString Sql = TEXT(
                "SELECT n.graph_name, n.node_guid, n.node_title, n.member_name, n.member_parent_path, "
                "a.object_path, a.asset_name, a.package_name, a.content_scope "
                "FROM blueprint_nodes n "
                "LEFT JOIN assets a ON a.generated_class_path = n.member_parent_path "
                "WHERE n.blueprint_object_path = ?1 AND n.node_type = 'call_function' AND n.member_parent_path <> ''");

            if (!GraphNameFilter.IsEmpty())
            {
                Sql += TEXT(" AND n.graph_name = ?2");
            }
            Sql += TEXT(" ORDER BY n.graph_name ASC, n.node_title ASC, n.node_guid ASC;");

            FSQLitePreparedStatement Statement(Database, *Sql, ESQLitePreparedStatementFlags::None);
            if (!Statement.IsValid()
                || !Statement.SetBindingValueByIndex(1, ObjectPath)
                || (!GraphNameFilter.IsEmpty() && !Statement.SetBindingValueByIndex(2, GraphNameFilter)))
            {
                OutExecError = TEXT("FindCrossBlueprintCalls could not prepare the node query.");
                return false;
            }

            const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
            {
                FCrossBlueprintCallRow Call;
                if (!Row.GetColumnValueByIndex(0, Call.GraphName)
                    || !Row.GetColumnValueByIndex(1, Call.NodeGuid)
                    || !Row.GetColumnValueByIndex(2, Call.NodeTitle)
                    || !Row.GetColumnValueByIndex(3, Call.MemberName)
                    || !Row.GetColumnValueByIndex(4, Call.MemberParentPath)
                    || !Row.GetColumnValueByIndex(5, Call.TargetObjectPath)
                    || !Row.GetColumnValueByIndex(6, Call.TargetAssetName)
                    || !Row.GetColumnValueByIndex(7, Call.TargetPackageName)
                    || !Row.GetColumnValueByIndex(8, Call.TargetContentScope))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Error;
                }

                if (Call.TargetObjectPath.IsEmpty())
                {
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                }

                if (!bIncludeSameBlueprintCalls && Call.TargetObjectPath.Equals(ObjectPath, ESearchCase::CaseSensitive))
                {
                    return ESQLitePreparedStatementExecuteRowResult::Continue;
                }

                Calls.Add(MoveTemp(Call));
                return ESQLitePreparedStatementExecuteRowResult::Continue;
            });

            if (QueryResult == INDEX_NONE)
            {
                OutExecError = Database.GetLastError().IsEmpty() ? TEXT("FindCrossBlueprintCalls node query failed.") : Database.GetLastError();
                return false;
            }

            return true;
        },
        Error);

    if (!bQuerySucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("FindCrossBlueprintCalls failed to query the project index: %s"), *Error));
    }

    Calls.Sort([](const FCrossBlueprintCallRow& Left, const FCrossBlueprintCallRow& Right)
    {
        if (Left.TargetObjectPath != Right.TargetObjectPath)
        {
            return Left.TargetObjectPath < Right.TargetObjectPath;
        }
        if (Left.GraphName != Right.GraphName)
        {
            return Left.GraphName < Right.GraphName;
        }
        return Left.NodeTitle < Right.NodeTitle;
    });

    TSet<FString> UniqueTargets;
    TSet<FString> UniqueMembers;
    TArray<TSharedPtr<FJsonValue>> CallsJson;
    TArray<FString> SummaryLines;

    for (const FCrossBlueprintCallRow& Call : Calls)
    {
        UniqueTargets.Add(Call.TargetObjectPath);
        UniqueMembers.Add(Call.MemberName + TEXT("|") + Call.TargetObjectPath);

        TSharedRef<FJsonObject> CallObject = MakeShared<FJsonObject>();
        CallObject->SetStringField(TEXT("graphName"), Call.GraphName);
        CallObject->SetStringField(TEXT("nodeGuid"), Call.NodeGuid);
        CallObject->SetStringField(TEXT("nodeTitle"), Call.NodeTitle);
        CallObject->SetStringField(TEXT("memberName"), Call.MemberName);
        CallObject->SetStringField(TEXT("memberParentPath"), Call.MemberParentPath);
        CallObject->SetStringField(TEXT("targetObjectPath"), Call.TargetObjectPath);
        CallObject->SetStringField(TEXT("targetAssetName"), Call.TargetAssetName);
        CallObject->SetStringField(TEXT("targetPackageName"), Call.TargetPackageName);
        CallObject->SetStringField(TEXT("targetContentScope"), Call.TargetContentScope);

        if (OutputMode == TEXT("full"))
        {
            CallsJson.Add(MakeShared<FJsonValueObject>(CallObject));
        }
        else
        {
            TSharedRef<FJsonObject> CompactObject = MakeShared<FJsonObject>();
            CompactObject->SetStringField(TEXT("graphName"), Call.GraphName);
            CompactObject->SetStringField(TEXT("nodeTitle"), Call.NodeTitle);
            CompactObject->SetStringField(TEXT("memberName"), Call.MemberName);
            CompactObject->SetStringField(TEXT("targetObjectPath"), Call.TargetObjectPath);
            CompactObject->SetStringField(TEXT("targetAssetName"), Call.TargetAssetName);
            CallsJson.Add(MakeShared<FJsonValueObject>(CompactObject));
        }

        if (SummaryLines.Num() < 12)
        {
            SummaryLines.Add(FString::Printf(TEXT("%s -> %s (%s)"), *Call.NodeTitle, *Call.TargetAssetName, *Call.TargetObjectPath));
        }
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("graphName"), GraphNameFilter);
    Result->SetStringField(TEXT("outputMode"), OutputMode);
    Result->SetBoolField(TEXT("includeSameBlueprintCalls"), bIncludeSameBlueprintCalls);
    Result->SetObjectField(TEXT("asset"), BlueprintAsset.ToSharedRef());
    Result->SetNumberField(TEXT("callCount"), Calls.Num());
    Result->SetNumberField(TEXT("uniqueTargetBlueprintCount"), UniqueTargets.Num());
    Result->SetNumberField(TEXT("uniqueTargetMemberCount"), UniqueMembers.Num());
    Result->SetStringField(
        TEXT("summaryText"),
        Calls.Num() > 0
            ? FString::Printf(TEXT("Found %d cross-Blueprint call site(s) from %d target Blueprint(s)."), Calls.Num(), UniqueTargets.Num())
            : TEXT("No cross-Blueprint Blueprint call sites were found."));

    TArray<TSharedPtr<FJsonValue>> SummaryJson;
    for (const FString& Line : SummaryLines)
    {
        SummaryJson.Add(MakeShared<FJsonValueString>(Line));
    }
    Result->SetArrayField(TEXT("summaryCalls"), SummaryJson);
    Result->SetArrayField(TEXT("calls"), CallsJson);
    Result->SetStringField(TEXT("nextStepHint"), TEXT("Use TraceBlueprintFlow with one of these member or node titles when you want the full execution path around a specific cross-Blueprint call."));
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FFindCrossBlueprintCallsTool::BuildInputSchema() const
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

    TSharedRef<FJsonObject> OutputModeProperty = MakeShared<FJsonObject>();
    OutputModeProperty->SetStringField(TEXT("type"), TEXT("string"));
    OutputModeProperty->SetStringField(TEXT("description"), TEXT("Optional response mode: summary, compact, or full. Defaults to summary."));
    Properties->SetObjectField(TEXT("outputMode"), OutputModeProperty);

    TSharedRef<FJsonObject> IncludeSameBlueprintCallsProperty = MakeShared<FJsonObject>();
    IncludeSameBlueprintCallsProperty->SetStringField(TEXT("type"), TEXT("boolean"));
    IncludeSameBlueprintCallsProperty->SetStringField(TEXT("description"), TEXT("Optional. When true, include same-Blueprint function calls as well. Defaults to false."));
    Properties->SetObjectField(TEXT("includeSameBlueprintCalls"), IncludeSameBlueprintCallsProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
