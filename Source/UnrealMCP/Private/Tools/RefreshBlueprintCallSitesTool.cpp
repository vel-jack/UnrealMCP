#include "Tools/RefreshBlueprintCallSitesTool.h"

#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "MCP/MutationRequestTracker.h"
#include "SQLitePreparedStatement.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "Tools/IndexedQueryToolUtils.h"
#include "Tools/RefreshBlueprintCallSitesEngine.h"

FRefreshBlueprintCallSitesTool::FRefreshBlueprintCallSitesTool()
    : FMCPToolBase(
        TEXT("RefreshBlueprintCallSites"),
        TEXT("Finds every same-Blueprint and cross-Blueprint call site of a Blueprint function via the project index and reconstructs each call node to match the function's current signature, compiling callers without saving."))
{
}

UnrealMCP::FMCPResponse FRefreshBlueprintCallSitesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    using namespace UnrealMCP::CallSiteRefresh;

    FString OwnerBlueprint, FunctionName, OperationId;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("ownerBlueprint"), OwnerBlueprint)
        || !Request.Params->TryGetStringField(TEXT("functionName"), FunctionName)
        || OwnerBlueprint.IsEmpty()
        || FunctionName.IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("RefreshBlueprintCallSites requires ownerBlueprint and functionName."));
    }
    Request.Params->TryGetStringField(TEXT("operationId"), OperationId);

    FOptions Options;
    Options.OperationId = OperationId;
    Options.bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    Options.bCompileCallers = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileCallers"), true);
    Options.bRefreshIndex = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("refreshIndex"), false);

    FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Preflighting,
        TEXT("Resolving the owning function and querying the project index for call sites."));

    FString ExecutionError;
    FString GeneratedClassPath;
    TArray<FCallSiteRow> Rows;

    const bool bResolved = BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            UBlueprint* OwnerObject = nullptr;
            if (!BlueprintEditToolUtils::ResolveBlueprint(OwnerBlueprint, OwnerObject, OutError))
            {
                return false;
            }
            if (OwnerObject->GeneratedClass == nullptr || OwnerObject->GeneratedClass->FindFunctionByName(*FunctionName) == nullptr)
            {
                OutError = FString::Printf(TEXT("Function '%s' was not found on '%s'."), *FunctionName, *OwnerBlueprint);
                return false;
            }
            GeneratedClassPath = OwnerObject->GeneratedClass->GetPathName();

            FString IndexError;
            const bool bQueried = IndexedQueryToolUtils::ExecuteWithProjectIndex(
                [&](FSQLiteDatabase& Database, FString& OutExecError)
                {
                    FSQLitePreparedStatement Statement(
                        Database,
                        TEXT("SELECT blueprint_object_path, graph_name, node_guid FROM blueprint_nodes "
                             "WHERE node_type = 'call_function' AND member_parent_path = ?1 AND member_name = ?2 "
                             "ORDER BY blueprint_object_path ASC, graph_name ASC, node_guid ASC;"),
                        ESQLitePreparedStatementFlags::None);
                    if (!Statement.IsValid()
                        || !Statement.SetBindingValueByIndex(1, GeneratedClassPath)
                        || !Statement.SetBindingValueByIndex(2, FunctionName))
                    {
                        OutExecError = TEXT("RefreshBlueprintCallSites could not prepare the call-site query.");
                        return false;
                    }

                    const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
                    {
                        FCallSiteRow CallSite;
                        if (!Row.GetColumnValueByIndex(0, CallSite.BlueprintObjectPath)
                            || !Row.GetColumnValueByIndex(1, CallSite.GraphName)
                            || !Row.GetColumnValueByIndex(2, CallSite.NodeGuid))
                        {
                            return ESQLitePreparedStatementExecuteRowResult::Error;
                        }
                        Rows.Add(MoveTemp(CallSite));
                        return ESQLitePreparedStatementExecuteRowResult::Continue;
                    });

                    if (QueryResult == INDEX_NONE)
                    {
                        OutExecError = Database.GetLastError().IsEmpty() ? TEXT("RefreshBlueprintCallSites call-site query failed.") : Database.GetLastError();
                        return false;
                    }
                    return true;
                },
                IndexError);

            if (!bQueried)
            {
                OutError = FString::Printf(TEXT("RefreshBlueprintCallSites could not query the project index (%s). Run BuildProjectIndex or RefreshProjectIndex first."), *IndexError);
                return false;
            }
            return true;
        },
        ExecutionError);

    if (!bResolved)
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError);
    }

    TMap<FString, TArray<FCallSiteRow>> RowsByBlueprint;
    for (const FCallSiteRow& Row : Rows)
    {
        RowsByBlueprint.FindOrAdd(Row.BlueprintObjectPath).Add(Row);
    }

    TArray<TSharedPtr<FJsonValue>> AffectedBlueprints;
    int32 FailedBlueprintCount = 0;

    for (const TPair<FString, TArray<FCallSiteRow>>& Entry : RowsByBlueprint)
    {
        TSharedRef<FJsonObject> AssetResult = MakeShared<FJsonObject>();
        if (!RefreshCallSitesInBlueprint(Entry.Key, Entry.Value, Options, AssetResult))
        {
            ++FailedBlueprintCount;
        }
        AffectedBlueprints.Add(MakeShared<FJsonValueObject>(AssetResult));
    }

    const bool bAllSucceeded = FailedBlueprintCount == 0;

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(bAllSucceeded);
    Result->SetStringField(TEXT("ownerBlueprint"), OwnerBlueprint);
    Result->SetStringField(TEXT("functionName"), FunctionName);
    Result->SetNumberField(TEXT("totalCallSites"), Rows.Num());
    Result->SetNumberField(TEXT("affectedBlueprintCount"), RowsByBlueprint.Num());
    Result->SetNumberField(TEXT("failedBlueprintCount"), FailedBlueprintCount);
    Result->SetBoolField(TEXT("dryRun"), Options.bDryRun);
    Result->SetArrayField(TEXT("affectedBlueprints"), AffectedBlueprints);
    if (!bAllSucceeded)
    {
        Result->SetStringField(TEXT("errorCode"), TEXT("call_site_refresh_incomplete"));
        Result->SetStringField(TEXT("message"), FString::Printf(
            TEXT("%d of %d affected Blueprint(s) did not complete; inspect affectedBlueprints[].error."),
            FailedBlueprintCount, RowsByBlueprint.Num()));
    }
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FRefreshBlueprintCallSitesTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;

    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("ownerBlueprint"), BuildStringProperty(TEXT("Object path of the Blueprint that owns the function whose signature changed.")));
    Properties->SetObjectField(TEXT("functionName"), BuildStringProperty(TEXT("Name of the function whose call sites should be refreshed.")));
    Properties->SetObjectField(TEXT("compileCallers"), BuildBoolProperty(TEXT("Compile each affected caller Blueprint after reconstruction. Defaults to true.")));
    Properties->SetObjectField(TEXT("refreshIndex"), BuildBoolProperty(TEXT("Reserved: only takes effect once an affected asset is saved and reindexed. Defaults to false.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Report which indexed call sites would be reconstructed without mutating any asset.")));
    Properties->SetObjectField(TEXT("operationId"), BuildStringProperty(TEXT("Optional client-assigned ID for mutation-request tracking via GetMutationRequestStatus.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(
        TEXT("required"),
        {
            MakeShared<FJsonValueString>(TEXT("ownerBlueprint")),
            MakeShared<FJsonValueString>(TEXT("functionName"))
        });
    return Schema;
}
