#include "Tools/AddBlueprintVariableGetNodeTool.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintMemberToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"

FAddBlueprintVariableGetNodeTool::FAddBlueprintVariableGetNodeTool()
    : FMCPToolBase(TEXT("AddBlueprintVariableGetNode"), TEXT("Adds a variable Get node for a function local or Blueprint-visible member (local-first without ownerClassPath; explicit owner selects class members) with collision-aware placement, dry-run, and optional save.")) {}

UnrealMCP::FMCPResponse FAddBlueprintVariableGetNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString O, G, GG, VariableName, OwnerClassPath, DeclaredIn, VariableScope, ScopeName, VariableGuid;
    bool bSelfContext = true;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), O) || !Request.Params->TryGetStringField(TEXT("variableName"), VariableName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AddBlueprintVariableGetNode requires objectPath, variableName, and graph selector."));
    }

    Request.Params->TryGetStringField(TEXT("ownerClassPath"), OwnerClassPath);
    Request.Params->TryGetStringField(TEXT("graphName"), G);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GG);
    if (G.IsEmpty() && GG.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Graph selector is required."));
    }

    bool Dry = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    bool Save = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);

    auto Result = UnrealMCP::BlueprintGraphEditToolUtils::AddSimpleGraphNode(
        O, G, GG, Request.Params, Dry, Save,
        NSLOCTEXT("UnrealMCP", "AddVariableGet", "UnrealMCP Add Variable Get"),
        [&](UBlueprint* Blueprint, UEdGraph* Graph, bool bDetached, FString& OutError) -> UEdGraphNode*
        {
            UnrealMCP::BlueprintMemberToolUtils::FResolvedVariable Variable;
            if (!UnrealMCP::BlueprintMemberToolUtils::ResolveVariable(Blueprint, Graph, VariableName, OwnerClassPath, false, Variable, OutError)) return nullptr;
            bSelfContext = Variable.IsSelfContext(); DeclaredIn = Variable.DeclarationPath;
            VariableScope = Variable.IsLocalContext() ? TEXT("local") : TEXT("member"); ScopeName = Variable.LocalScope;
            VariableGuid = Variable.Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
            UEdGraphNode* Node = UnrealMCP::BlueprintMemberToolUtils::CreateVariableNode(Graph, Variable, false, bDetached);
            if (!Node)
            {
                OutError = bDetached ? TEXT("Could not create the detached Variable Get preview.") : TEXT("Could not create the Variable Get node.");
            }

            return Node;
        }, Save || UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), false));

    if (!Result.bSucceeded)
    {
        auto Evidence = BuildBooleanResult(false); Evidence->SetBoolField(TEXT("added"), Result.bAdded);
        Evidence->SetBoolField(TEXT("rolledBack"), Result.bRolledBack); Evidence->SetBoolField(TEXT("saved"), Result.bSaved);
        Evidence->SetArrayField(TEXT("errors"), Result.Errors); Evidence->SetArrayField(TEXT("warnings"), Result.Warnings);
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, Result.ErrorMessage, Evidence);
    }

    UnrealMCP::FMCPResponse R;
    R.Id = Request.Id;
    auto J = BuildBooleanResult(true);
    J->SetStringField(TEXT("variableName"), VariableName);
    J->SetStringField(TEXT("variableScope"), VariableScope); J->SetStringField(TEXT("scopeName"), ScopeName); J->SetStringField(TEXT("variableGuid"), VariableGuid);
    J->SetBoolField(TEXT("selfContext"), bSelfContext); J->SetStringField(TEXT("declaredIn"), DeclaredIn);
    J->SetStringField(TEXT("nodeGuid"), Result.NodeGuid);
    J->SetStringField(TEXT("nodeType"), TEXT("variable_get"));
    J->SetBoolField(TEXT("dryRun"), Dry);
    J->SetBoolField(TEXT("pinsPredicted"), Dry);
    J->SetBoolField(TEXT("added"), !Dry);
    J->SetNumberField(TEXT("positionX"), Result.Placement.X);
    J->SetNumberField(TEXT("positionY"), Result.Placement.Y);
    J->SetBoolField(TEXT("collisionAdjusted"), Result.Placement.bCollisionAdjusted);
    J->SetArrayField(TEXT("pins"), Result.Pins);
    J->SetArrayField(TEXT("errors"), Result.Errors); J->SetArrayField(TEXT("warnings"), Result.Warnings);
    J->SetBoolField(TEXT("compiled"), Result.bCompiled); J->SetBoolField(TEXT("compileSucceeded"), Result.bCompileSucceeded);
    J->SetBoolField(TEXT("rolledBack"), Result.bRolledBack);
    J->SetBoolField(TEXT("saved"), Result.bSaved);
    J->SetBoolField(TEXT("indexRefreshed"), Result.bIndexRefreshed);
    J->SetStringField(TEXT("indexRefreshError"), Result.IndexRefreshError);
    R.Result = J;
    return R;
}

TSharedPtr<FJsonObject> FAddBlueprintVariableGetNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto S = MakeShared<FJsonObject>();
    S->SetStringField(TEXT("type"), TEXT("object"));
    auto P = MakeShared<FJsonObject>();
    for (const TCHAR* N : { TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("variableName"), TEXT("ownerClassPath"), TEXT("relativeToNodeGuid") })
    {
        P->SetObjectField(N, BuildStringProperty(TEXT("Blueprint/graph/placement selector. ownerClassPath explicitly selects class members; omitted owner resolves a same-function local first.")));
    }

    auto I = MakeShared<FJsonObject>();
    I->SetStringField(TEXT("type"), TEXT("integer"));
    P->SetObjectField(TEXT("positionX"), I);
    P->SetObjectField(TEXT("positionY"), I);
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate and preview.")));
    P->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile after adding; defaults false. Save forces compile. Failure rolls back the addition.")));
    P->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh.")));
    S->SetObjectField(TEXT("properties"), P);
    TArray<TSharedPtr<FJsonValue>> Q{ MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("variableName")) };
    S->SetArrayField(TEXT("required"), Q);
    return S;
}
