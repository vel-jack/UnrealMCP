#include "Tools/AddBlueprintBranchNodeTool.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FAddBlueprintBranchNodeTool::FAddBlueprintBranchNodeTool()
    : FMCPToolBase(TEXT("AddBlueprintBranchNode"), TEXT("Adds a collision-aware Branch node to a stable Blueprint graph. Supports explicit or relative placement, dry-run, and optional save.")) {}

UnrealMCP::FMCPResponse FAddBlueprintBranchNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath,GraphName,GraphGuid;if(!Request.Params.IsValid()||!Request.Params->TryGetStringField(TEXT("objectPath"),ObjectPath))return BuildError(Request,UnrealMCP::EMCPErrorCode::InvalidParams,TEXT("AddBlueprintBranchNode requires objectPath and graphName or graphGuid."));
    Request.Params->TryGetStringField(TEXT("graphName"),GraphName);Request.Params->TryGetStringField(TEXT("graphGuid"),GraphGuid);if(GraphName.IsEmpty()&&GraphGuid.IsEmpty())return BuildError(Request,UnrealMCP::EMCPErrorCode::InvalidParams,TEXT("AddBlueprintBranchNode requires graphName or graphGuid."));
    const bool bDryRun=UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params,TEXT("dryRun"),false),bSave=UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params,TEXT("saveAfterEdit"),false);
    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement Placement;FString NodeGuid,SavedFilename,IndexError,ExecutionError;bool bIndexRefreshed=false;TArray<TSharedPtr<FJsonValue>>Pins;
    const bool bSucceeded=UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString&OutError)
    {
        UBlueprint*Blueprint=nullptr;if(!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath,Blueprint,OutError))return false;UEdGraph*Graph=nullptr;if(!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint,GraphName,GraphGuid,Graph,OutError))return false;
        Placement=UnrealMCP::BlueprintGraphEditToolUtils::ResolvePlacement(Graph,Request.Params,OutError);if(!OutError.IsEmpty())return false;if(bDryRun)return true;
        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP","AddBranchNode","UnrealMCP Add Branch Node"));Blueprint->Modify();Graph->Modify();UK2Node_IfThenElse*Node=NewObject<UK2Node_IfThenElse>(Graph);UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph,Node,Placement);FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);NodeGuid=UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node);Pins=UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(Node);
        return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(Blueprint,ObjectPath,bSave,SavedFilename,bIndexRefreshed,IndexError,OutError);
    },ExecutionError);if(!bSucceeded)return BuildError(Request,UnrealMCP::EMCPErrorCode::InternalError,ExecutionError);
    UnrealMCP::FMCPResponse Response;Response.Id=Request.Id;TSharedRef<FJsonObject>Result=BuildBooleanResult(true);Result->SetStringField(TEXT("objectPath"),ObjectPath);Result->SetStringField(TEXT("graphName"),GraphName);Result->SetStringField(TEXT("graphGuid"),GraphGuid);Result->SetBoolField(TEXT("dryRun"),bDryRun);Result->SetBoolField(TEXT("added"),!bDryRun);Result->SetStringField(TEXT("nodeGuid"),NodeGuid);Result->SetStringField(TEXT("nodeType"),TEXT("branch"));Result->SetNumberField(TEXT("positionX"),Placement.X);Result->SetNumberField(TEXT("positionY"),Placement.Y);Result->SetBoolField(TEXT("autoPlaced"),Placement.bAutoPlaced);Result->SetBoolField(TEXT("collisionAdjusted"),Placement.bCollisionAdjusted);Result->SetArrayField(TEXT("pins"),Pins);Result->SetBoolField(TEXT("saved"),bSave&&!bDryRun);Result->SetStringField(TEXT("savedFilename"),SavedFilename);Result->SetBoolField(TEXT("indexRefreshed"),bIndexRefreshed);Result->SetStringField(TEXT("indexRefreshError"),IndexError);Response.Result=Result;return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintBranchNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;TSharedRef<FJsonObject>S=MakeShared<FJsonObject>();S->SetStringField(TEXT("type"),TEXT("object"));TSharedRef<FJsonObject>P=MakeShared<FJsonObject>();P->SetObjectField(TEXT("objectPath"),BuildStringProperty(TEXT("Target Blueprint object path.")));P->SetObjectField(TEXT("graphName"),BuildStringProperty(TEXT("Stable graphName from ListBlueprintGraphs, or unique display name.")));P->SetObjectField(TEXT("graphGuid"),BuildStringProperty(TEXT("Optional exact graph GUID.")));P->SetObjectField(TEXT("relativeToNodeGuid"),BuildStringProperty(TEXT("Optional node GUID used as the placement anchor.")));P->SetObjectField(TEXT("dryRun"),BuildBoolProperty(TEXT("Resolve graph and preview collision-aware placement without mutation.")));P->SetObjectField(TEXT("saveAfterEdit"),BuildBoolProperty(TEXT("Save and refresh this Blueprint after adding.")));TSharedRef<FJsonObject>N=MakeShared<FJsonObject>();N->SetStringField(TEXT("type"),TEXT("integer"));P->SetObjectField(TEXT("positionX"),N);P->SetObjectField(TEXT("positionY"),N);P->SetObjectField(TEXT("horizontalSpacing"),N);P->SetObjectField(TEXT("verticalOffset"),N);S->SetObjectField(TEXT("properties"),P);TArray<TSharedPtr<FJsonValue>>R{MakeShared<FJsonValueString>(TEXT("objectPath"))};S->SetArrayField(TEXT("required"),R);return S;
}
