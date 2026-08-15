#include "Tools/AddBlueprintOverrideEventNodeTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FAddBlueprintOverrideEventNodeTool::FAddBlueprintOverrideEventNodeTool()
    : FMCPToolBase(
          TEXT("AddBlueprintOverrideEventNode"),
          TEXT("Adds an overrideable parent-class Blueprint event to an exact Blueprint graph with collision-aware placement, dry-run, duplicate rejection, and optional save."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintOverrideEventNodeTool::Execute(
    const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    FString GraphGuid;
    FString FunctionName;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid)
        || !Request.Params->TryGetStringField(TEXT("functionName"), FunctionName)
        || ObjectPath.IsEmpty()
        || GraphGuid.IsEmpty()
        || FunctionName.IsEmpty())
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("AddBlueprintOverrideEventNode requires objectPath, graphGuid, and functionName."));
    }

    double NodeX = 0.0;
    double NodeY = 0.0;
    const bool bHasNodeX = Request.Params->TryGetNumberField(TEXT("nodeX"), NodeX);
    const bool bHasNodeY = Request.Params->TryGetNumberField(TEXT("nodeY"), NodeY);
    if (bHasNodeX != bHasNodeY)
    {
        return BuildError(
            Request,
            UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("nodeX and nodeY must be provided together."));
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(
        Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(
        Request.Params, TEXT("saveAfterEdit"), false);

    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement Placement;
    FString NodeGuid;
    FString ResolvedGraphGuid;
    FString FunctionOwnerClass;
    FString SavedFilename;
    FString IndexError;
    FString ExecutionError;
    bool bIndexRefreshed = false;
    TArray<TSharedPtr<FJsonValue>> Pins;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            UBlueprint* Blueprint = nullptr;
            if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(
                    ObjectPath, Blueprint, OutError))
            {
                return false;
            }
            if (Blueprint->ParentClass == nullptr)
            {
                OutError = TEXT("The Blueprint does not have a parent class.");
                return false;
            }

            UEdGraph* Graph = nullptr;
            if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(
                    Blueprint, FString(), GraphGuid, Graph, OutError))
            {
                return false;
            }
            if (Graph->GetSchema() == nullptr
                || Graph->GetSchema()->GetGraphType(Graph) != EGraphType::GT_Ubergraph)
            {
                OutError = TEXT("Override event nodes can only be added to an Ubergraph event graph.");
                return false;
            }
            ResolvedGraphGuid = Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);

            UFunction* Function = Blueprint->ParentClass->FindFunctionByName(*FunctionName);
            if (Function == nullptr)
            {
                OutError = FString::Printf(
                    TEXT("Function '%s' was not found on Blueprint parent class '%s'."),
                    *FunctionName,
                    *Blueprint->ParentClass->GetPathName());
                return false;
            }
            if (!Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
            {
                OutError = FString::Printf(
                    TEXT("Function '%s' is not a Blueprint event."), *FunctionName);
                return false;
            }
            if (Function->HasAnyFunctionFlags(FUNC_Final)
                || !UEdGraphSchema_K2::CanKismetOverrideFunction(Function)
                || !UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function))
            {
                OutError = FString::Printf(
                    TEXT("Function '%s' is not overrideable as a Blueprint event node."),
                    *FunctionName);
                return false;
            }

            const UClass* SignatureClass = Function->GetOwnerClass()->GetAuthoritativeClass();
            if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(
                    Blueprint, SignatureClass, Function->GetFName()))
            {
                OutError = FString::Printf(
                    TEXT("Override event '%s' already exists (nodeGuid=%s)."),
                    *FunctionName,
                    *UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Existing));
                return false;
            }

            FunctionOwnerClass = SignatureClass->GetPathName();
            TSharedRef<FJsonObject> PlacementParams = MakeShared<FJsonObject>();
            if (bHasNodeX)
            {
                PlacementParams->SetNumberField(TEXT("positionX"), NodeX);
                PlacementParams->SetNumberField(TEXT("positionY"), NodeY);
            }
            Placement = UnrealMCP::BlueprintGraphEditToolUtils::ResolvePlacement(
                Graph, PlacementParams, OutError);
            if (!OutError.IsEmpty() || bDryRun)
            {
                return OutError.IsEmpty();
            }

            const FScopedTransaction Transaction(
                NSLOCTEXT("UnrealMCP", "AddOverrideEvent", "UnrealMCP Add Override Event"));
            Blueprint->Modify();
            Graph->Modify();

            UK2Node_Event* Node = NewObject<UK2Node_Event>(Graph);
            Node->EventReference.SetFromField<UFunction>(Function, false);
            Node->bOverrideFunction = true;
            UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, Placement);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

            NodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node);
            Pins = UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(Node);
            return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
                Blueprint,
                ObjectPath,
                bSave,
                SavedFilename,
                bIndexRefreshed,
                IndexError,
                OutError);
        },
        ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("graphGuid"), ResolvedGraphGuid);
    Result->SetStringField(TEXT("functionName"), FunctionName);
    Result->SetStringField(TEXT("functionOwnerClass"), FunctionOwnerClass);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("added"), !bDryRun);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetStringField(TEXT("nodeType"), TEXT("overrideEvent"));
    Result->SetNumberField(TEXT("positionX"), Placement.X);
    Result->SetNumberField(TEXT("positionY"), Placement.Y);
    Result->SetBoolField(TEXT("autoPlaced"), Placement.bAutoPlaced);
    Result->SetBoolField(TEXT("collisionAdjusted"), Placement.bCollisionAdjusted);
    Result->SetArrayField(TEXT("pins"), Pins);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintOverrideEventNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;

    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(
        TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(
        TEXT("graphGuid"), BuildStringProperty(TEXT("Exact stable Ubergraph GUID.")));
    Properties->SetObjectField(
        TEXT("functionName"),
        BuildStringProperty(TEXT("Exact overrideable Blueprint event name on the parent class.")));

    TSharedRef<FJsonObject> NodeXProperty = MakeShared<FJsonObject>();
    NodeXProperty->SetStringField(TEXT("type"), TEXT("integer"));
    NodeXProperty->SetStringField(TEXT("description"), TEXT("Optional requested node X position."));
    Properties->SetObjectField(TEXT("nodeX"), NodeXProperty);

    TSharedRef<FJsonObject> NodeYProperty = MakeShared<FJsonObject>();
    NodeYProperty->SetStringField(TEXT("type"), TEXT("integer"));
    NodeYProperty->SetStringField(TEXT("description"), TEXT("Optional requested node Y position."));
    Properties->SetObjectField(TEXT("nodeY"), NodeYProperty);

    Properties->SetObjectField(
        TEXT("dryRun"),
        BuildBoolProperty(TEXT("Validate the event and preview placement without mutation.")));
    Properties->SetObjectField(
        TEXT("saveAfterEdit"),
        BuildBoolProperty(TEXT("Save and partially refresh this Blueprint after adding.")));
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("graphGuid")),
        MakeShared<FJsonValueString>(TEXT("functionName"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
