#include "Tools/AddBlueprintCommentNodeTool.h"
#include "Dom/JsonObject.h"
#include "EdGraphNode_Comment.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FAddBlueprintCommentNodeTool::FAddBlueprintCommentNodeTool()
    : FMCPToolBase(TEXT("AddBlueprintCommentNode"), TEXT("Adds a bounded Blueprint graph comment with explicit text, collision-aware placement, size, dry-run, and optional save.")) {}

UnrealMCP::FMCPResponse FAddBlueprintCommentNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString O, G, GG, Text;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), O) || !Request.Params->TryGetStringField(TEXT("commentText"), Text))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AddBlueprintCommentNode requires objectPath, commentText, and graph selector."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), G);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GG);
    if (G.IsEmpty() && GG.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Graph selector is required."));
    }

    double WidthValue = 600, HeightValue = 300;
    if (Request.Params.IsValid())
    {
        Request.Params->TryGetNumberField(TEXT("width"), WidthValue);
        Request.Params->TryGetNumberField(TEXT("height"), HeightValue);
    }

    const TSharedPtr<FJsonObject>* RequestedColor = nullptr;
    if (Request.Params->TryGetObjectField(TEXT("color"), RequestedColor))
    {
        for (const TCHAR* ChannelName : {TEXT("r"), TEXT("g"), TEXT("b")})
        {
            double Value = 0;
            if (!(*RequestedColor)->TryGetNumberField(ChannelName, Value) || !FMath::IsFinite(Value) || Value < 0)
                return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Comment color requires finite nonnegative r/g/b."));
        }
    }
    const int32 Width = FMath::Clamp(FMath::RoundToInt(WidthValue), 200, 4000);
    const int32 Height = FMath::Clamp(FMath::RoundToInt(HeightValue), 100, 4000);
    bool Dry = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    bool Save = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);

    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement P;
    FString NG, File, IdxErr, ExecErr;
    bool Idx = false;

    bool Ok = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& E)
    {
        UBlueprint* B = nullptr;
        if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(O, B, E))
        {
            return false;
        }

        UEdGraph* Graph = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(B, G, GG, Graph, E))
        {
            return false;
        }

        P = UnrealMCP::BlueprintGraphEditToolUtils::ResolvePlacement(Graph, Request.Params, E);
        if (!E.IsEmpty() || Dry)
        {
            return E.IsEmpty();
        }

        const FScopedTransaction T(NSLOCTEXT("UnrealMCP", "AddComment", "UnrealMCP Add Graph Comment"));
        B->Modify();
        Graph->Modify();
        auto* Node = NewObject<UEdGraphNode_Comment>(Graph);
        Node->NodeWidth = Width;
        Node->NodeHeight = Height;
        UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, P);
        // PostPlacedNewNode initializes the default title, so apply authored properties afterwards.
        Node->NodeComment = Text;
        Node->bCommentBubbleVisible_InDetailsPanel = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("showBubbleWhenZoomed"), true);
        Node->bCommentBubbleVisible = Node->bCommentBubbleVisible_InDetailsPanel;
        Node->bColorCommentBubble = true;
        const TSharedPtr<FJsonObject>* Color = nullptr;
        if (Request.Params->TryGetObjectField(TEXT("color"), Color))
        {
            Node->CommentColor = FLinearColor((*Color)->GetNumberField(TEXT("r")), (*Color)->GetNumberField(TEXT("g")), (*Color)->GetNumberField(TEXT("b")), 1.f);
        }
        FBlueprintEditorUtils::MarkBlueprintAsModified(B);
        NG = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node);
        return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(B, O, Save, File, Idx, IdxErr, E);
    }, ExecErr);

    if (!Ok)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecErr);
    }

    UnrealMCP::FMCPResponse R;
    R.Id = Request.Id;
    auto J = BuildBooleanResult(true);
    J->SetStringField(TEXT("nodeGuid"), NG);
    J->SetStringField(TEXT("commentText"), Text);
    J->SetBoolField(TEXT("showBubbleWhenZoomed"), UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("showBubbleWhenZoomed"), true));
    J->SetNumberField(TEXT("positionX"), P.X);
    J->SetNumberField(TEXT("positionY"), P.Y);
    J->SetNumberField(TEXT("width"), Width);
    J->SetNumberField(TEXT("height"), Height);
    J->SetBoolField(TEXT("collisionAdjusted"), P.bCollisionAdjusted);
    J->SetBoolField(TEXT("dryRun"), Dry);
    J->SetBoolField(TEXT("added"), !Dry);
    J->SetBoolField(TEXT("saved"), Save && !Dry);
    J->SetBoolField(TEXT("indexRefreshed"), Idx);
    J->SetStringField(TEXT("indexRefreshError"), IdxErr);
    R.Result = J;
    return R;
}

TSharedPtr<FJsonObject> FAddBlueprintCommentNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto S = MakeShared<FJsonObject>();
    S->SetStringField(TEXT("type"), TEXT("object"));
    auto P = MakeShared<FJsonObject>();
    for (const TCHAR* N : { TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("commentText"), TEXT("relativeToNodeGuid") })
    {
        P->SetObjectField(N, BuildStringProperty(TEXT("Blueprint graph/comment/placement value.")));
    }

    auto I = MakeShared<FJsonObject>();
    I->SetStringField(TEXT("type"), TEXT("integer"));
    for (const TCHAR* N : { TEXT("positionX"), TEXT("positionY"), TEXT("width"), TEXT("height") })
    {
        P->SetObjectField(N, I);
    }

    auto Color = MakeShared<FJsonObject>(); Color->SetStringField(TEXT("type"), TEXT("object"));
    auto Channels = MakeShared<FJsonObject>(); auto Channel = MakeShared<FJsonObject>(); Channel->SetStringField(TEXT("type"), TEXT("number"));
    for (const TCHAR* ChannelName : {TEXT("r"), TEXT("g"), TEXT("b")}) Channels->SetObjectField(ChannelName, Channel);
    Color->SetObjectField(TEXT("properties"), Channels);
    Color->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("r")), MakeShared<FJsonValueString>(TEXT("g")), MakeShared<FJsonValueString>(TEXT("b"))});
    P->SetObjectField(TEXT("color"), Color);
    P->SetObjectField(TEXT("showBubbleWhenZoomed"), BuildBoolProperty(TEXT("Show bubble when zoomed; defaults true.")));
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Preview bounds.")));
    P->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh.")));
    S->SetObjectField(TEXT("properties"), P);
    TArray<TSharedPtr<FJsonValue>> Q{ MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("commentText")) };
    S->SetArrayField(TEXT("required"), Q);
    return S;
}
