#include "Tools/AddBlueprintBranchNodeTool.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "K2Node_IfThenElse.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"

FAddBlueprintBranchNodeTool::FAddBlueprintBranchNodeTool()
    : FMCPToolBase(TEXT("AddBlueprintBranchNode"), TEXT("Adds a collision-aware Branch node to a stable Blueprint graph. Supports explicit or relative placement, dry-run, and optional save.")) {}

UnrealMCP::FMCPResponse FAddBlueprintBranchNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath, GraphName, GraphGuid;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AddBlueprintBranchNode requires objectPath and graphName or graphGuid."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("AddBlueprintBranchNode requires graphName or graphGuid."));
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);

    auto Result = UnrealMCP::BlueprintGraphEditToolUtils::AddSimpleGraphNode(
        ObjectPath, GraphName, GraphGuid, Request.Params, bDryRun, bSave,
        NSLOCTEXT("UnrealMCP", "AddBranchNode", "UnrealMCP Add Branch Node"),
        [](UBlueprint*, UEdGraph* Graph, bool bDetached, FString& OutError) -> UEdGraphNode*
        {
            UK2Node_IfThenElse* Node = UnrealMCP::BlueprintGraphEditToolUtils::CreateBranchNode(Graph, bDetached);
            if (!Node)
            {
                OutError = bDetached ? TEXT("Could not create the detached Branch preview.") : TEXT("Could not create the Branch node.");
            }
            return Node;
        });

    if (!Result.bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, Result.ErrorMessage);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> ResultJson = BuildBooleanResult(true);
    ResultJson->SetStringField(TEXT("objectPath"), ObjectPath);
    ResultJson->SetStringField(TEXT("graphName"), GraphName);
    ResultJson->SetStringField(TEXT("graphGuid"), GraphGuid);
    ResultJson->SetBoolField(TEXT("dryRun"), bDryRun);
    ResultJson->SetBoolField(TEXT("pinsPredicted"), bDryRun);
    ResultJson->SetBoolField(TEXT("added"), !bDryRun);
    ResultJson->SetStringField(TEXT("nodeGuid"), Result.NodeGuid);
    ResultJson->SetStringField(TEXT("nodeType"), TEXT("branch"));
    ResultJson->SetNumberField(TEXT("positionX"), Result.Placement.X);
    ResultJson->SetNumberField(TEXT("positionY"), Result.Placement.Y);
    ResultJson->SetBoolField(TEXT("autoPlaced"), Result.Placement.bAutoPlaced);
    ResultJson->SetBoolField(TEXT("collisionAdjusted"), Result.Placement.bCollisionAdjusted);
    ResultJson->SetArrayField(TEXT("pins"), Result.Pins);
    ResultJson->SetBoolField(TEXT("saved"), Result.bSaved);
    ResultJson->SetStringField(TEXT("savedFilename"), Result.SavedFilename);
    ResultJson->SetBoolField(TEXT("indexRefreshed"), Result.bIndexRefreshed);
    ResultJson->SetStringField(TEXT("indexRefreshError"), Result.IndexRefreshError);
    Response.Result = ResultJson;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintBranchNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
    S->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    P->SetObjectField(TEXT("graphName"), BuildStringProperty(TEXT("Stable graphName from ListBlueprintGraphs, or unique display name.")));
    P->SetObjectField(TEXT("graphGuid"), BuildStringProperty(TEXT("Optional exact graph GUID.")));
    P->SetObjectField(TEXT("relativeToNodeGuid"), BuildStringProperty(TEXT("Optional node GUID used as the placement anchor.")));
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Resolve graph and preview collision-aware placement without mutation.")));
    P->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh this Blueprint after adding.")));
    TSharedRef<FJsonObject> N = MakeShared<FJsonObject>();
    N->SetStringField(TEXT("type"), TEXT("integer"));
    P->SetObjectField(TEXT("positionX"), N);
    P->SetObjectField(TEXT("positionY"), N);
    P->SetObjectField(TEXT("horizontalSpacing"), N);
    P->SetObjectField(TEXT("verticalOffset"), N);
    S->SetObjectField(TEXT("properties"), P);
    TArray<TSharedPtr<FJsonValue>> R{ MakeShared<FJsonValueString>(TEXT("objectPath")) };
    S->SetArrayField(TEXT("required"), R);
    return S;
}
