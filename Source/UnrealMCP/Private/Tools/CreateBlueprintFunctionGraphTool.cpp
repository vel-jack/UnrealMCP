#include "Tools/CreateBlueprintFunctionGraphTool.h"
#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FCreateBlueprintFunctionGraphTool::FCreateBlueprintFunctionGraphTool()
    : FMCPToolBase(TEXT("CreateBlueprintFunctionGraph"), TEXT("Creates a uniquely named user Blueprint function graph and returns stable graph, entry, and return identities. Supports dry-run and optional save.")) {}

UnrealMCP::FMCPResponse FCreateBlueprintFunctionGraphTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString O, FunctionName;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), O) || !Request.Params->TryGetStringField(TEXT("functionName"), FunctionName) || FunctionName.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("CreateBlueprintFunctionGraph requires objectPath and functionName."));
    }

    bool Dry = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    bool Save = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    bool Exists = false;

    FString GraphGuid, EntryGuid, ReturnGuid, File, IdxErr, ExecErr;
    bool Idx = false;

    bool Ok = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& E)
    {
        UBlueprint* B = nullptr;
        if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(O, B, E))
        {
            return false;
        }

        for (UEdGraph* Existing : B->FunctionGraphs)
        {
            if (Existing && Existing->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
            {
                Exists = true;
                GraphGuid = Existing->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
                return true;
            }
        }

        if (!FBlueprintEditorUtils::IsGraphNameUnique(B, *FunctionName))
        {
            E = TEXT("Function graph name conflicts with an existing Blueprint graph or member.");
            return false;
        }

        if (Dry)
        {
            return true;
        }

        const FScopedTransaction T(NSLOCTEXT("UnrealMCP", "CreateFunctionGraph", "UnrealMCP Create Function Graph"));
        B->Modify();
        UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(B, *FunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        if (!Graph)
        {
            E = TEXT("Unreal failed to create the function graph.");
            return false;
        }

        FBlueprintEditorUtils::AddFunctionGraph<UFunction>(B, Graph, true, nullptr);
        GraphGuid = Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Cast<UK2Node_FunctionEntry>(Node))
            {
                EntryGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node);
            }
            else if (Cast<UK2Node_FunctionResult>(Node))
            {
                ReturnGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node);
            }
        }

        return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(B, O, Save, File, Idx, IdxErr, E);
    }, ExecErr);

    if (!Ok)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecErr);
    }

    UnrealMCP::FMCPResponse R;
    R.Id = Request.Id;
    auto J = BuildBooleanResult(true);
    J->SetStringField(TEXT("functionName"), FunctionName);
    J->SetStringField(TEXT("graphGuid"), GraphGuid);
    J->SetStringField(TEXT("entryNodeGuid"), EntryGuid);
    J->SetStringField(TEXT("returnNodeGuid"), ReturnGuid);
    J->SetBoolField(TEXT("alreadyExists"), Exists);
    J->SetBoolField(TEXT("dryRun"), Dry);
    J->SetBoolField(TEXT("created"), !Dry && !Exists);
    J->SetBoolField(TEXT("saved"), Save && !Dry && !Exists);
    J->SetBoolField(TEXT("indexRefreshed"), Idx);
    J->SetStringField(TEXT("indexRefreshError"), IdxErr);
    R.Result = J;
    return R;
}

TSharedPtr<FJsonObject> FCreateBlueprintFunctionGraphTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto S = MakeShared<FJsonObject>();
    S->SetStringField(TEXT("type"), TEXT("object"));
    auto P = MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint.")));
    P->SetObjectField(TEXT("functionName"), BuildStringProperty(TEXT("Unique function graph name.")));
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate name without creation.")));
    P->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh.")));
    S->SetObjectField(TEXT("properties"), P);
    TArray<TSharedPtr<FJsonValue>> Q{ MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("functionName")) };
    S->SetArrayField(TEXT("required"), Q);
    return S;
}
