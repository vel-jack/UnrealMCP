#include "Tools/AddBlueprintVariableSetNodeTool.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"

FAddBlueprintVariableSetNodeTool::FAddBlueprintVariableSetNodeTool()
    : FMCPToolBase(TEXT("AddBlueprintVariableSetNode"), TEXT("Adds a variable Set node for an exact Blueprint member with collision-aware placement, dry-run, and optional save.")) {}

UnrealMCP::FMCPResponse FAddBlueprintVariableSetNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString O, G, GG, VariableName;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), O) || !Request.Params->TryGetStringField(TEXT("variableName"), VariableName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AddBlueprintVariableSetNode requires objectPath, variableName, and graph selector."));
    }

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
        NSLOCTEXT("UnrealMCP", "AddVariableSet", "UnrealMCP Add Variable Set"),
        [&VariableName](UBlueprint* Blueprint, UEdGraph* Graph, bool bDetached, FString& OutError) -> UEdGraphNode*
        {
            const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, *VariableName);
            if (Index == INDEX_NONE)
            {
                OutError = TEXT("Variable was not found in the target Blueprint member variables.");
                return nullptr;
            }

            UEdGraphNode* Node = UnrealMCP::BlueprintGraphEditToolUtils::CreateVariableSetNode(Graph, *VariableName, Blueprint->NewVariables[Index].VarGuid, bDetached);
            if (!Node)
            {
                OutError = bDetached ? TEXT("Could not create the detached Variable Set preview.") : TEXT("Could not create the Variable Set node.");
            }

            return Node;
        });

    if (!Result.bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, Result.ErrorMessage);
    }

    UnrealMCP::FMCPResponse R;
    R.Id = Request.Id;
    auto J = BuildBooleanResult(true);
    J->SetStringField(TEXT("variableName"), VariableName);
    J->SetStringField(TEXT("nodeGuid"), Result.NodeGuid);
    J->SetStringField(TEXT("nodeType"), TEXT("variable_set"));
    J->SetBoolField(TEXT("dryRun"), Dry);
    J->SetBoolField(TEXT("pinsPredicted"), Dry);
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

TSharedPtr<FJsonObject> FAddBlueprintVariableSetNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto S = MakeShared<FJsonObject>();
    S->SetStringField(TEXT("type"), TEXT("object"));
    auto P = MakeShared<FJsonObject>();
    for (const TCHAR* N : { TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("variableName"), TEXT("relativeToNodeGuid") })
    {
        P->SetObjectField(N, BuildStringProperty(TEXT("Blueprint member, graph, or placement selector.")));
    }

    auto I = MakeShared<FJsonObject>();
    I->SetStringField(TEXT("type"), TEXT("integer"));
    P->SetObjectField(TEXT("positionX"), I);
    P->SetObjectField(TEXT("positionY"), I);
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate and preview.")));
    P->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh.")));
    S->SetObjectField(TEXT("properties"), P);
    TArray<TSharedPtr<FJsonValue>> Q{ MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("variableName")) };
    S->SetArrayField(TEXT("required"), Q);
    return S;
}
