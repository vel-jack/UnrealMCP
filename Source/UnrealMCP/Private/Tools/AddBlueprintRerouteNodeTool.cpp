#include "Tools/AddBlueprintRerouteNodeTool.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "K2Node_Knot.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"

FAddBlueprintRerouteNodeTool::FAddBlueprintRerouteNodeTool()
    : FMCPToolBase(TEXT("AddBlueprintRerouteNode"), TEXT("Adds a wildcard Reroute node with collision-aware placement. Its pin type specializes when connected.")) {}

UnrealMCP::FMCPResponse FAddBlueprintRerouteNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString O, G, GG;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), O))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AddBlueprintRerouteNode requires objectPath and graph selector."));
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
        NSLOCTEXT("UnrealMCP", "AddReroute", "UnrealMCP Add Reroute"),
        [](UBlueprint*, UEdGraph* Graph, bool bDetached, FString& OutError) -> UEdGraphNode*
        {
            UK2Node_Knot* Node = UnrealMCP::BlueprintGraphEditToolUtils::CreateRerouteNode(Graph, bDetached);
            if (!Node)
            {
                OutError = bDetached ? TEXT("Could not create the detached Reroute preview.") : TEXT("Could not create the Reroute node.");
            }
            return Node;
        });

    if (!Result.bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, Result.ErrorMessage);
    }

    UnrealMCP::FMCPResponse R;
    R.Id = Request.Id;
    auto J = BuildBooleanResult(true);
    J->SetStringField(TEXT("nodeGuid"), Result.NodeGuid);
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

TSharedPtr<FJsonObject> FAddBlueprintRerouteNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto S = MakeShared<FJsonObject>();
    S->SetStringField(TEXT("type"), TEXT("object"));
    auto P = MakeShared<FJsonObject>();
    for (const TCHAR* N : { TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("relativeToNodeGuid") })
    {
        P->SetObjectField(N, BuildStringProperty(TEXT("Blueprint graph or placement selector.")));
    }

    auto I = MakeShared<FJsonObject>();
    I->SetStringField(TEXT("type"), TEXT("integer"));
    P->SetObjectField(TEXT("positionX"), I);
    P->SetObjectField(TEXT("positionY"), I);
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Preview placement.")));
    P->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh.")));
    S->SetObjectField(TEXT("properties"), P);
    TArray<TSharedPtr<FJsonValue>> Q{ MakeShared<FJsonValueString>(TEXT("objectPath")) };
    S->SetArrayField(TEXT("required"), Q);
    return S;
}
