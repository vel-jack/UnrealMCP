#include "Tools/Blueprint/Graph/RefreshBlueprintNodesTool.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"

FRefreshBlueprintNodesTool::FRefreshBlueprintNodesTool()
    : FMCPToolBase(TEXT("RefreshBlueprintNodes"),
        TEXT("Runs Unreal Editor Refresh All Nodes on one Blueprint in an undo transaction. Requires confirm=true; does not save."))
{
}

UnrealMCP::FMCPResponse FRefreshBlueprintNodesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString Path, Error;
    if (!Request.Params || !Request.Params->TryGetStringField(TEXT("objectPath"), Path) || Path.IsEmpty()
        || !BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirm"), false))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide objectPath and confirm=true."));

    const bool bRefreshed = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(Path, Blueprint, OutError)) return false;
        if (!GEditor || GEditor->IsTransactionActive())
        {
            OutError = TEXT("Refresh requires an idle editor transaction system.");
            return false;
        }
        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "RefreshBlueprintNodes", "Refresh All Blueprint Nodes"));
        if (!GUndo)
        {
            OutError = TEXT("Undo recording is unavailable.");
            return false;
        }
        Blueprint->Modify();
        FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        return true;
    }, Error);
    if (!bRefreshed) return BuildError(Request, EMCPErrorCode::InvalidParams, Error);

    FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = BuildBooleanResult(true);
    Response.Result->SetStringField(TEXT("objectPath"), Path);
    Response.Result->SetBoolField(TEXT("saved"), false);
    return Response;
}

TSharedPtr<FJsonObject> FRefreshBlueprintNodesTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto Schema = MakeShared<FJsonObject>();
    auto Properties = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint asset path.")));
    Properties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Confirm reconstructing the Blueprint nodes.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("confirm"))});
    return Schema;
}
