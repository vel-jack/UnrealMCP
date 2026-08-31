#include "Tools/SetBlueprintPinDefaultValueTool.h"
#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FSetBlueprintPinDefaultValueTool::FSetBlueprintPinDefaultValueTool()
    : FMCPToolBase(TEXT("SetBlueprintPinDefaultValue"), TEXT("Sets a validated literal default on an unconnected Blueprint input pin using stable node/pin identity. Supports dry-run and optional save.")) {}

UnrealMCP::FMCPResponse FSetBlueprintPinDefaultValueTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString O, G, GG, N, PinIdValue, PinNameValue, RequestedValue;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), O) || !Request.Params->TryGetStringField(TEXT("nodeGuid"), N) || !Request.Params->TryGetStringField(TEXT("defaultValue"), RequestedValue))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("SetBlueprintPinDefaultValue requires objectPath, nodeGuid, defaultValue, graph selector, and pin selector."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), G);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GG);
    Request.Params->TryGetStringField(TEXT("pinId"), PinIdValue);
    Request.Params->TryGetStringField(TEXT("pinName"), PinNameValue);
    if (G.IsEmpty() && GG.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("SetBlueprintPinDefaultValue requires graphName or graphGuid."));
    }

    bool Dry = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    bool Save = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    FString Old, Applied, File, IdxErr, ExecErr;
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

        UEdGraphNode* Node = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveNode(Graph, N, Node, E))
        {
            return false;
        }

        UEdGraphPin* Pin = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolvePin(Node, PinIdValue, PinNameValue, TEXT("input"), Pin, E))
        {
            return false;
        }

        if (Pin->LinkedTo.Num() > 0)
        {
            E = TEXT("Cannot set a literal default on a connected input pin.");
            return false;
        }

        const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
        if (!Schema)
        {
            E = TEXT("Graph does not use K2 schema.");
            return false;
        }

        Old = Pin->DefaultValue;
        if (Dry)
        {
            Applied = RequestedValue;
            return true;
        }

        const FScopedTransaction Tx(NSLOCTEXT("UnrealMCP", "SetPinDefault", "UnrealMCP Set Blueprint Pin Default"));
        B->Modify();
        Graph->Modify();
        Node->Modify();
        Schema->TrySetDefaultValue(*Pin, RequestedValue);
        Applied = Pin->DefaultValue;
        if (Applied != RequestedValue)
        {
            E = FString::Printf(TEXT("Unreal rejected or normalized the requested default. Applied='%s'."), *Applied);
            return false;
        }

        FBlueprintEditorUtils::MarkBlueprintAsModified(B);
        return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(B, O, Save, File, Idx, IdxErr, E);
    }, ExecErr);

    if (!Ok)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecErr);
    }

    UnrealMCP::FMCPResponse R;
    R.Id = Request.Id;
    auto J = BuildBooleanResult(true);
    J->SetStringField(TEXT("nodeGuid"), N);
    J->SetStringField(TEXT("pinId"), PinIdValue);
    J->SetStringField(TEXT("pinName"), PinNameValue);
    J->SetStringField(TEXT("oldDefaultValue"), Old);
    J->SetStringField(TEXT("defaultValue"), Applied);
    J->SetBoolField(TEXT("dryRun"), Dry);
    J->SetBoolField(TEXT("changed"), !Dry && Old != Applied);
    J->SetBoolField(TEXT("saved"), Save && !Dry);
    J->SetBoolField(TEXT("indexRefreshed"), Idx);
    J->SetStringField(TEXT("indexRefreshError"), IdxErr);
    R.Result = J;
    return R;
}

TSharedPtr<FJsonObject> FSetBlueprintPinDefaultValueTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto S = MakeShared<FJsonObject>();
    S->SetStringField(TEXT("type"), TEXT("object"));
    auto P = MakeShared<FJsonObject>();
    for (const TCHAR* N : { TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("nodeGuid"), TEXT("pinId"), TEXT("pinName"), TEXT("defaultValue") })
    {
        P->SetObjectField(N, BuildStringProperty(TEXT("Blueprint graph/node/pin selector or literal value.")));
    }

    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate without mutation.")));
    P->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh after mutation.")));
    S->SetObjectField(TEXT("properties"), P);
    TArray<TSharedPtr<FJsonValue>> Q{ MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("nodeGuid")), MakeShared<FJsonValueString>(TEXT("defaultValue")) };
    S->SetArrayField(TEXT("required"), Q);
    return S;
}
