#include "Tools/AddBlueprintFunctionCallNodeTool.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"

FAddBlueprintFunctionCallNodeTool::FAddBlueprintFunctionCallNodeTool()
    : FMCPToolBase(TEXT("AddBlueprintFunctionCallNode"), TEXT("Adds a function-call node from an exact owner class and function name with collision-aware placement, dry-run, and optional save.")) {}

UnrealMCP::FMCPResponse FAddBlueprintFunctionCallNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString O, G, GG, OwnerPath, FunctionName;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), O) || !Request.Params->TryGetStringField(TEXT("functionName"), FunctionName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AddBlueprintFunctionCallNode requires objectPath, functionName, and graphName or graphGuid."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), G);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GG);
    Request.Params->TryGetStringField(TEXT("ownerClassPath"), OwnerPath);
    if (G.IsEmpty() && GG.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Graph selector is required."));
    }

    bool Dry = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    bool Save = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);

    FString ResolvedOwner, NodeClass;
    bool Pure = false;
    bool RequiresExplicitTarget = false;

    auto Result = UnrealMCP::BlueprintGraphEditToolUtils::AddSimpleGraphNode(
        O, G, GG, Request.Params, Dry, Save,
        NSLOCTEXT("UnrealMCP", "AddFunctionCall", "UnrealMCP Add Function Call"),
        [&](UBlueprint* Blueprint, UEdGraph* Graph, bool bDetached, FString& OutError) -> UEdGraphNode*
        {
            UClass* Owner = Blueprint->GeneratedClass;
            if (!OwnerPath.IsEmpty() && !UnrealMCP::BlueprintEditToolUtils::ResolveClass(OwnerPath, Owner, OutError))
            {
                return nullptr;
            }

            if (!Owner)
            {
                OutError = TEXT("Function owner class is unavailable.");
                return nullptr;
            }

            UFunction* Function = Owner->FindFunctionByName(*FunctionName, EIncludeSuperFlag::IncludeSuper);
            if (!Function)
            {
                OutError = FString::Printf(TEXT("Function '%s' was not found on '%s'."), *FunctionName, *Owner->GetPathName());
                return nullptr;
            }

            if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
            {
                OutError = TEXT("Function is not BlueprintCallable or BlueprintPure.");
                return nullptr;
            }

            ResolvedOwner = Function->GetOwnerClass()->GetPathName();
            Pure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
            RequiresExplicitTarget = !Function->HasAnyFunctionFlags(FUNC_Static) && (!Blueprint->GeneratedClass || !Blueprint->GeneratedClass->IsChildOf(Function->GetOwnerClass()));

            UEdGraphNode* Node = UnrealMCP::BlueprintGraphEditToolUtils::CreateFunctionCallNode(Graph, Function, bDetached);
            if (!Node)
            {
                OutError = bDetached ? TEXT("Could not create the detached function-call preview.") : TEXT("Could not create the function-call node.");
                return nullptr;
            }

            NodeClass = Node->GetClass()->GetPathName();
            return Node;
        });

    if (!Result.bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, Result.ErrorMessage);
    }

    UnrealMCP::FMCPResponse R;
    R.Id = Request.Id;
    auto J = BuildBooleanResult(true);
    J->SetStringField(TEXT("functionName"), FunctionName);
    J->SetStringField(TEXT("ownerClassPath"), ResolvedOwner);
    J->SetStringField(TEXT("nodeGuid"), Result.NodeGuid);
    J->SetStringField(TEXT("nodeClass"), NodeClass);
    J->SetBoolField(TEXT("isPure"), Pure);
    J->SetBoolField(TEXT("requiresExplicitTarget"), RequiresExplicitTarget);
    J->SetBoolField(TEXT("pinsPredicted"), Dry);
    J->SetBoolField(TEXT("dryRun"), Dry);
    J->SetBoolField(TEXT("added"), !Dry);
    J->SetNumberField(TEXT("positionX"), Result.Placement.X);
    J->SetNumberField(TEXT("positionY"), Result.Placement.Y);
    J->SetBoolField(TEXT("collisionAdjusted"), Result.Placement.bCollisionAdjusted);
    J->SetArrayField(TEXT("pins"), Result.Pins);
    J->SetBoolField(TEXT("saved"), Result.bSaved);
    J->SetBoolField(TEXT("indexRefreshed"), Result.bIndexRefreshed);
    J->SetStringField(TEXT("indexRefreshError"), Result.IndexRefreshError);
    R.Result = J;
    return R;
}

TSharedPtr<FJsonObject> FAddBlueprintFunctionCallNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto S = MakeShared<FJsonObject>();
    S->SetStringField(TEXT("type"), TEXT("object"));
    auto P = MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint.")));
    P->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Stable graph name.")));
    P->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Exact graph GUID.")));
    P->SetObjectField(TEXT("functionName"), BuildStringProperty(TEXT("Exact reflected UFunction name.")));
    P->SetObjectField(TEXT("ownerClassPath"), BuildStringProperty(TEXT("Optional exact owner class; defaults to the target Blueprint generated class.")));
    P->SetObjectField(TEXT("relativeToNodeGuid"), BuildStringProperty(TEXT("Optional placement anchor.")));
    auto I = MakeShared<FJsonObject>();
    I->SetStringField(TEXT("type"), TEXT("integer"));
    P->SetObjectField(TEXT("positionX"), I);
    P->SetObjectField(TEXT("positionY"), I);
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate function and placement.")));
    P->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh.")));
    S->SetObjectField(TEXT("properties"), P);
    TArray<TSharedPtr<FJsonValue>> Q{ MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("functionName")) };
    S->SetArrayField(TEXT("required"), Q);
    return S;
}
