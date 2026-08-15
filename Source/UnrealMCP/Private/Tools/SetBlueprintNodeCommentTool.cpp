#include "Tools/SetBlueprintNodeCommentTool.h"

#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FSetBlueprintNodeCommentTool::FSetBlueprintNodeCommentTool()
    : FMCPToolBase(
        TEXT("SetBlueprintNodeComment"),
        TEXT("Sets a Blueprint node comment and optional comment-bubble visibility state. Idempotent, transactional, and supports dry-run and optional save."))
{
}

UnrealMCP::FMCPResponse FSetBlueprintNodeCommentTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FString GraphName;
    FString GraphGuid;
    FString NodeGuid;
    FString RequestedComment;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("nodeGuid"), NodeGuid)
        || !Request.Params->TryGetStringField(TEXT("comment"), RequestedComment))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("SetBlueprintNodeComment requires objectPath, graphName or graphGuid, nodeGuid, and comment."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Graph selector is required."));
    }

    bool bRequestedBubbleVisible = false;
    bool bRequestedBubblePinned = false;
    const bool bHasBubbleVisible = Request.Params->TryGetBoolField(TEXT("commentBubbleVisible"), bRequestedBubbleVisible);
    const bool bHasBubblePinned = Request.Params->TryGetBoolField(TEXT("commentBubblePinned"), bRequestedBubblePinned);
    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);

    FString PreviousComment;
    bool bPreviousBubbleVisible = false;
    bool bPreviousBubblePinned = false;
    bool bAppliedBubbleVisible = false;
    bool bAppliedBubblePinned = false;
    bool bChanged = false;
    bool bIndexRefreshed = false;
    FString Filename;
    FString IndexError;
    FString ExecutionError;

    const bool bExecuted = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& Error)
        {
            UBlueprint* Blueprint = nullptr;
            if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, Error))
            {
                return false;
            }

            UEdGraph* Graph = nullptr;
            if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, Error))
            {
                return false;
            }

            UEdGraphNode* Node = nullptr;
            if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveNode(Graph, NodeGuid, Node, Error))
            {
                return false;
            }

            PreviousComment = Node->NodeComment;
            bPreviousBubbleVisible = Node->bCommentBubbleVisible;
            bPreviousBubblePinned = Node->bCommentBubblePinned;
            bAppliedBubbleVisible = bHasBubbleVisible ? bRequestedBubbleVisible : bPreviousBubbleVisible;
            bAppliedBubblePinned = bHasBubblePinned ? bRequestedBubblePinned : bPreviousBubblePinned;
            bChanged = PreviousComment != RequestedComment
                || bPreviousBubbleVisible != bAppliedBubbleVisible
                || bPreviousBubblePinned != bAppliedBubblePinned;

            if (bDryRun || !bChanged)
            {
                return true;
            }

            const FScopedTransaction Transaction(NSLOCTEXT(
                "UnrealMCP", "SetBlueprintNodeComment", "UnrealMCP Set Blueprint Node Comment"));
            Blueprint->Modify();
            Graph->Modify();
            Node->Modify();
            Node->NodeComment = RequestedComment;
            Node->bCommentBubbleVisible = bAppliedBubbleVisible;
            Node->bCommentBubblePinned = bAppliedBubblePinned;
            Node->bCommentBubbleMakeVisible = bAppliedBubbleVisible;
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
                Blueprint, ObjectPath, bSave, Filename, bIndexRefreshed, IndexError, Error);
        },
        ExecutionError);

    if (!bExecuted)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedPtr<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetStringField(TEXT("previousComment"), PreviousComment);
    Result->SetStringField(TEXT("comment"), RequestedComment);
    Result->SetBoolField(TEXT("previousCommentBubbleVisible"), bPreviousBubbleVisible);
    Result->SetBoolField(TEXT("commentBubbleVisible"), bAppliedBubbleVisible);
    Result->SetBoolField(TEXT("previousCommentBubblePinned"), bPreviousBubblePinned);
    Result->SetBoolField(TEXT("commentBubblePinned"), bAppliedBubblePinned);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("changed"), bChanged && !bDryRun);
    Result->SetBoolField(TEXT("wouldChange"), bChanged);
    Result->SetBoolField(TEXT("saved"), bSave && bChanged && !bDryRun);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FSetBlueprintNodeCommentTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;

    TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Stable or unique graph name.")));
    Properties->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Exact graph GUID.")));
    Properties->SetObjectField(TEXT("nodeGuid"), BuildStringProperty(TEXT("Exact node GUID.")));
    Properties->SetObjectField(TEXT("comment"), BuildStringProperty(TEXT("Replacement node comment; use an empty string to clear it.")));
    Properties->SetObjectField(TEXT("commentBubbleVisible"), BuildBoolProperty(TEXT("Optional persisted comment-bubble visibility.")));
    Properties->SetObjectField(TEXT("commentBubblePinned"), BuildBoolProperty(TEXT("Optional persisted comment-bubble pinned state.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Preview changes without mutation.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index after mutation.")));
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("objectPath")));
    Required.Add(MakeShared<FJsonValueString>(TEXT("nodeGuid")));
    Required.Add(MakeShared<FJsonValueString>(TEXT("comment")));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
