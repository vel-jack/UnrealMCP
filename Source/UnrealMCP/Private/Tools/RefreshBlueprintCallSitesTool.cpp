#include "Tools/RefreshBlueprintCallSitesTool.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MCP/MutationRequestTracker.h"
#include "ScopedTransaction.h"
#include "SQLitePreparedStatement.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "Tools/IndexedQueryToolUtils.h"

namespace
{
    using namespace UnrealMCP;

    struct FCallSiteRow
    {
        FString BlueprintObjectPath;
        FString GraphName;
        FString NodeGuid;
    };

    struct FPinSnapshot
    {
        FString PinId;
        FString PinName;
        EEdGraphPinDirection Direction = EGPD_Input;
    };

    // The index stores graph_name as "Name::GraphGuid::AssetPath:Name" (see BlueprintGraphEditToolUtils::ResolveGraph),
    // not a plain UEdGraph::GetName(), so resolution must go through the same shared helper the rest of the
    // plugin uses for index-sourced graph identifiers rather than a raw name comparison.
    UEdGraphNode* FindCallSiteNode(UBlueprint* Blueprint, const FString& GraphName, const FString& NodeGuid, UEdGraph*& OutGraph, FString& OutError)
    {
        OutGraph = nullptr;
        if (!BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, FString(), OutGraph, OutError))
        {
            return nullptr;
        }
        UEdGraphNode* Node = nullptr;
        if (!BlueprintGraphEditToolUtils::ResolveNode(OutGraph, NodeGuid, Node, OutError))
        {
            return nullptr;
        }
        return Node;
    }

    TArray<FPinSnapshot> SnapshotPins(const UK2Node_CallFunction* Node)
    {
        TArray<FPinSnapshot> Snapshot;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin == nullptr)
            {
                continue;
            }
            FPinSnapshot Item;
            Item.PinId = BlueprintGraphEditToolUtils::GetPinId(Pin);
            Item.PinName = Pin->PinName.ToString();
            Item.Direction = Pin->Direction;
            Snapshot.Add(MoveTemp(Item));
        }
        return Snapshot;
    }

    bool FindMatchingRow(const TArray<FPinSnapshot>& Rows, const FString& Name, EEdGraphPinDirection Direction, FPinSnapshot& OutRow)
    {
        for (const FPinSnapshot& Row : Rows)
        {
            if (Row.Direction == Direction && Row.PinName.Equals(Name, ESearchCase::IgnoreCase))
            {
                OutRow = Row;
                return true;
            }
        }
        return false;
    }

    // Reconstructs one caller node in-place: Unreal's UK2Node::ReconstructNode() re-derives pins from
    // SetFromFunction()/GetTargetFunction() and rewires old links onto matching-named new pins itself,
    // orphaning (not silently dropping) any pin whose name no longer exists on the new signature.
    TSharedRef<FJsonObject> ReconstructNode(UK2Node_CallFunction* Node, const FString& NodeGuid, const FString& GraphName)
    {
        const TArray<FPinSnapshot> OldPins = SnapshotPins(Node);
        Node->Modify();
        Node->ReconstructNode();
        const TArray<FPinSnapshot> NewPins = SnapshotPins(Node);

        TArray<TSharedPtr<FJsonValue>> Mapping;
        TArray<TSharedPtr<FJsonValue>> Orphaned;
        for (const FPinSnapshot& OldPin : OldPins)
        {
            FPinSnapshot NewMatch;
            if (FindMatchingRow(NewPins, OldPin.PinName, OldPin.Direction, NewMatch))
            {
                TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("pinName"), OldPin.PinName);
                Item->SetStringField(TEXT("direction"), OldPin.Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
                Item->SetStringField(TEXT("oldPinId"), OldPin.PinId);
                Item->SetStringField(TEXT("newPinId"), NewMatch.PinId);
                Mapping.Add(MakeShared<FJsonValueObject>(Item));
            }
            else
            {
                Orphaned.Add(MakeShared<FJsonValueString>(OldPin.PinName));
            }
        }

        TArray<TSharedPtr<FJsonValue>> OldPinIds, NewPinIds;
        for (const FPinSnapshot& Pin : OldPins) OldPinIds.Add(MakeShared<FJsonValueString>(Pin.PinId));
        for (const FPinSnapshot& Pin : NewPins) NewPinIds.Add(MakeShared<FJsonValueString>(Pin.PinId));

        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
        Result->SetStringField(TEXT("graphName"), GraphName);
        Result->SetArrayField(TEXT("oldPinIds"), OldPinIds);
        Result->SetArrayField(TEXT("newPinIds"), NewPinIds);
        Result->SetArrayField(TEXT("pinNameMapping"), Mapping);
        Result->SetArrayField(TEXT("orphanedPins"), Orphaned);
        Result->SetBoolField(TEXT("requiresManualAttention"), Orphaned.Num() > 0);
        return Result;
    }
}

FRefreshBlueprintCallSitesTool::FRefreshBlueprintCallSitesTool()
    : FMCPToolBase(
        TEXT("RefreshBlueprintCallSites"),
        TEXT("Finds every same-Blueprint and cross-Blueprint call site of a Blueprint function via the project index and reconstructs each call node to match the function's current signature, compiling callers without saving."))
{
}

UnrealMCP::FMCPResponse FRefreshBlueprintCallSitesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

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
    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompileCallers = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileCallers"), true);
    const bool bRefreshIndex = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("refreshIndex"), false);

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

    for (const TPair<FString, TArray<FCallSiteRow>>& Entry : RowsByBlueprint)
    {
        const FString& CallerObjectPath = Entry.Key;
        const TArray<FCallSiteRow>& CallerRows = Entry.Value;

        TSharedRef<FJsonObject> AssetResult = MakeShared<FJsonObject>();
        AssetResult->SetStringField(TEXT("objectPath"), CallerObjectPath);
        TArray<TSharedPtr<FJsonValue>> NodeResults;
        bool bCompiled = false, bCompileSucceeded = true, bRolledBack = false, bDirty = false;
        int32 CompileErrors = 0, CompileWarnings = 0;
        FString AssetError;

        BlueprintToolUtils::ExecuteOnGameThreadSync(
            [&](FString& OutError)
            {
                UBlueprint* CallerBlueprint = nullptr;
                if (!BlueprintEditToolUtils::ResolveBlueprint(CallerObjectPath, CallerBlueprint, OutError))
                {
                    AssetError = OutError;
                    return false;
                }

                if (bDryRun)
                {
                    for (const FCallSiteRow& Row : CallerRows)
                    {
                        UEdGraph* Graph = nullptr;
                        FString ResolveError;
                        UEdGraphNode* Node = FindCallSiteNode(CallerBlueprint, Row.GraphName, Row.NodeGuid, Graph, ResolveError);
                        UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
                        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                        Item->SetStringField(TEXT("nodeGuid"), Row.NodeGuid);
                        Item->SetStringField(TEXT("graphName"), Row.GraphName);
                        Item->SetBoolField(TEXT("found"), CallNode != nullptr);
                        NodeResults.Add(MakeShared<FJsonValueObject>(Item));
                    }
                    return true;
                }

                const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "RefreshBlueprintCallSites", "UnrealMCP Refresh Blueprint Call Sites"));
                CallerBlueprint->Modify();

                FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Mutating,
                    FString::Printf(TEXT("Reconstructing %d call node(s) in %s."), CallerRows.Num(), *CallerObjectPath));

                bool bAnyReconstructed = false;
                for (const FCallSiteRow& Row : CallerRows)
                {
                    UEdGraph* Graph = nullptr;
                    FString ResolveError;
                    UEdGraphNode* Node = FindCallSiteNode(CallerBlueprint, Row.GraphName, Row.NodeGuid, Graph, ResolveError);
                    UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
                    if (CallNode == nullptr)
                    {
                        continue;
                    }
                    Graph->Modify();
                    NodeResults.Add(MakeShared<FJsonValueObject>(ReconstructNode(CallNode, Row.NodeGuid, Row.GraphName)));
                    bAnyReconstructed = true;
                }

                if (!bAnyReconstructed)
                {
                    return true;
                }

                bDirty = true;
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(CallerBlueprint);

                if (bCompileCallers)
                {
                    FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Compiling,
                        FString::Printf(TEXT("Compiling %s after call-site reconstruction."), *CallerObjectPath));
                    FCompilerResultsLog Log;
                    Log.bSilentMode = true;
                    FKismetEditorUtilities::CompileBlueprint(
                        CallerBlueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Log);
                    bCompiled = true;
                    CompileErrors = Log.NumErrors;
                    CompileWarnings = Log.NumWarnings;
                    bCompileSucceeded = CompileErrors == 0
                        && (CallerBlueprint->Status == BS_UpToDate || CallerBlueprint->Status == BS_UpToDateWithWarnings);
                    if (!bCompileSucceeded)
                    {
                        bRolledBack = GEditor != nullptr && GEditor->UndoTransaction();
                        if (bRolledBack)
                        {
                            FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::RolledBack,
                                FString::Printf(TEXT("Compilation failed after reconstruction; %s rolled back."), *CallerObjectPath));
                            FCompilerResultsLog RollbackLog;
                            RollbackLog.bSilentMode = true;
                            FKismetEditorUtilities::CompileBlueprint(
                                CallerBlueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &RollbackLog);
                        }
                        bDirty = !bRolledBack;
                        AssetError = FString::Printf(TEXT("Blueprint compilation failed with %d error(s); rolledBack=%s."),
                            CompileErrors, bRolledBack ? TEXT("true") : TEXT("false"));
                    }
                }

                // Never save here: saving affected callers is an explicit later step (Phase 4B.2).
                return true;
            },
            AssetError);

        AssetResult->SetArrayField(TEXT("nodesReconstructed"), NodeResults);
        AssetResult->SetBoolField(TEXT("compiled"), bCompiled);
        AssetResult->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
        AssetResult->SetNumberField(TEXT("compileErrors"), CompileErrors);
        AssetResult->SetNumberField(TEXT("compileWarnings"), CompileWarnings);
        AssetResult->SetBoolField(TEXT("rolledBack"), bRolledBack);
        AssetResult->SetBoolField(TEXT("dirty"), bDirty);
        AssetResult->SetStringField(TEXT("error"), AssetError);
        AssetResult->SetStringField(TEXT("indexRefreshNote"),
            bRefreshIndex ? TEXT("refreshIndex has no effect until this asset is saved; refresh the index after saving.") : FString());
        AffectedBlueprints.Add(MakeShared<FJsonValueObject>(AssetResult));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("ownerBlueprint"), OwnerBlueprint);
    Result->SetStringField(TEXT("functionName"), FunctionName);
    Result->SetNumberField(TEXT("totalCallSites"), Rows.Num());
    Result->SetNumberField(TEXT("affectedBlueprintCount"), RowsByBlueprint.Num());
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetArrayField(TEXT("affectedBlueprints"), AffectedBlueprints);
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
