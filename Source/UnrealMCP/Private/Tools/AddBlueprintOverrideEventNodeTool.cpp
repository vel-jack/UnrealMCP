#include "Tools/AddBlueprintOverrideEventNodeTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "K2Node_Event.h"
#include "K2Node_CallParentFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintMemberToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FAddBlueprintOverrideEventNodeTool::FAddBlueprintOverrideEventNodeTool()
    : FMCPToolBase(
          TEXT("AddBlueprintOverrideEventNode"),
          TEXT("Adds an overrideable parent-class Blueprint event to an exact Blueprint graph with collision-aware placement, dry-run, duplicate rejection, optional wired parent call, and optional compile/save. Save implies compile; wiring/compile failures roll back the complete addition."))
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

    const bool bAddParentCall = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(
        Request.Params, TEXT("addParentCall"), false);
    const bool bCompile = bSave || UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(
        Request.Params, TEXT("compileAfterEdit"), false);

    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement Placement;
    FString NodeGuid;
    FString ResolvedGraphGuid;
    FString FunctionOwnerClass;
    FString SavedFilename;
    FString IndexError;
    FString ExecutionError;
    bool bIndexRefreshed = false;
    bool bAdded = false, bCompiled = false, bSaved = false;
    bool bRollbackAttempted = false, bRolledBack = false;
    FString ParentCallGuid;
    TArray<TSharedPtr<FJsonValue>> Pins, ParentCallPins, CompileErrors, CompileWarnings;

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
            if (!OutError.IsEmpty()) return false;
            UFunction* ParentFunction = nullptr;
            if (bAddParentCall && !UnrealMCP::BlueprintMemberToolUtils::ResolveParentFunction(
                Blueprint, FunctionName, ParentFunction, OutError)) return false;
            if (bDryRun)
            {
                if (bAddParentCall)
                {
                    // K2 connection notifications must not reach the real Blueprint during preflight.
                    UBlueprint* PreviewBlueprint = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
                    PreviewBlueprint->ParentClass = Blueprint->ParentClass;
                    PreviewBlueprint->GeneratedClass = Blueprint->GeneratedClass;
                    PreviewBlueprint->SkeletonGeneratedClass = Blueprint->SkeletonGeneratedClass;
                    PreviewBlueprint->BlueprintType = Blueprint->BlueprintType;
                    PreviewBlueprint->Status = BS_BeingCreated;
                    UEdGraph* PreviewGraph = DuplicateObject<UEdGraph>(Graph, PreviewBlueprint, Graph->GetFName());
                    if (!PreviewGraph) { OutError = TEXT("Could not create the event preflight graph."); return false; }
                    PreviewGraph->SetFlags(RF_Transient);
                    PreviewBlueprint->UbergraphPages.Add(PreviewGraph);
                    UK2Node_Event* PreviewEvent = NewObject<UK2Node_Event>(PreviewGraph, NAME_None, RF_Transient);
                    PreviewEvent->EventReference.SetFromField<UFunction>(Function, false);
                    PreviewEvent->bOverrideFunction = true;
                    PreviewEvent->AllocateDefaultPins();
                    auto* PreviewParent = UnrealMCP::BlueprintMemberToolUtils::CreateParentCall(PreviewGraph, ParentFunction, true);
                    if (!UnrealMCP::BlueprintMemberToolUtils::WireParentCall(PreviewEvent, PreviewParent, OutError)) return false;
                    Pins = UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(PreviewEvent);
                    ParentCallPins = UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(PreviewParent);
                }
                return true;
            }
            if (!GEditor || GEditor->IsTransactionActive())
            {
                OutError = TEXT("An idle editor transaction system is required.");
                return false;
            }
            const bool bWasDirty = Blueprint->GetPackage()->IsDirty();
            bool bWired = true;
            {
                const FScopedTransaction Transaction(
                    NSLOCTEXT("UnrealMCP", "AddOverrideEvent", "UnrealMCP Add Override Event"));
                if (!GUndo) { OutError = TEXT("Undo recording is unavailable; no event added."); return false; }
                Blueprint->Modify();
                Graph->Modify();
                UK2Node_Event* Node = NewObject<UK2Node_Event>(Graph);
                Node->EventReference.SetFromField<UFunction>(Function, false);
                Node->bOverrideFunction = true;
                UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Node, Placement);
                NodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node);
                if (bAddParentCall)
                {
                    auto ParentParams = MakeShared<FJsonObject>();
                    ParentParams->SetNumberField(TEXT("positionX"), static_cast<double>(Placement.X) + 400.0);
                    ParentParams->SetNumberField(TEXT("positionY"), Placement.Y);
                    const auto ParentPlacement = UnrealMCP::BlueprintGraphEditToolUtils::ResolvePlacement(Graph, ParentParams, OutError);
                    bWired = OutError.IsEmpty();
                    if (bWired)
                    {
                        auto* Parent = UnrealMCP::BlueprintMemberToolUtils::CreateParentCall(Graph, ParentFunction, false);
                        UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(Graph, Parent, ParentPlacement);
                        ParentCallGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Parent);
                        bWired = UnrealMCP::BlueprintMemberToolUtils::WireParentCall(Node, Parent, OutError);
                        ParentCallPins = UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(Parent);
                    }
                }
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
                Pins = UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(Node);
                bAdded = true;
            }
            if (bWired && bCompile)
                bCompiled = UnrealMCP::BlueprintEditToolUtils::CompileWithLocatedMessages(Blueprint, CompileErrors, CompileWarnings);
            if (!bWired || (bCompile && !bCompiled))
            {
                bRollbackAttempted = true;
                bRolledBack = GEditor->UndoTransaction();
                if (bRolledBack)
                {
                    TArray<TSharedPtr<FJsonValue>> RestoreErrors, RestoreWarnings;
                    UnrealMCP::BlueprintEditToolUtils::CompileWithLocatedMessages(Blueprint, RestoreErrors, RestoreWarnings);
                    Blueprint->GetPackage()->SetDirtyFlag(bWasDirty);
                    bAdded = false;
                }
                if (OutError.IsEmpty()) OutError = TEXT("Event addition failed compilation; rollback result is reported. Nothing saved.");
                return false;
            }
            if (!UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
                Blueprint, ObjectPath, bSave, SavedFilename, bIndexRefreshed, IndexError, OutError)) return false;
            bSaved = bSave;
            return true;
        },
        ExecutionError);

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(bSucceeded);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("graphGuid"), ResolvedGraphGuid);
    Result->SetStringField(TEXT("functionName"), FunctionName);
    Result->SetStringField(TEXT("functionOwnerClass"), FunctionOwnerClass);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("added"), bAdded);
    Result->SetBoolField(TEXT("addParentCall"), bAddParentCall);
    Result->SetBoolField(TEXT("pinsPredicted"), bDryRun && bAddParentCall);
    Result->SetBoolField(TEXT("parentCallAdded"), bAddParentCall && bAdded);
    Result->SetStringField(TEXT("parentCallNodeGuid"), ParentCallGuid);
    Result->SetArrayField(TEXT("parentCallPins"), ParentCallPins);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetArrayField(TEXT("errors"), CompileErrors);
    Result->SetArrayField(TEXT("warnings"), CompileWarnings);
    Result->SetBoolField(TEXT("rollbackAttempted"), bRollbackAttempted);
    Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetStringField(TEXT("nodeType"), TEXT("overrideEvent"));
    Result->SetNumberField(TEXT("positionX"), Placement.X);
    Result->SetNumberField(TEXT("positionY"), Placement.Y);
    Result->SetBoolField(TEXT("autoPlaced"), Placement.bAutoPlaced);
    Result->SetBoolField(TEXT("collisionAdjusted"), Placement.bCollisionAdjusted);
    Result->SetArrayField(TEXT("pins"), Pins);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    if (!bSucceeded) return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError, Result);
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
    Properties->SetObjectField(TEXT("addParentCall"),
        BuildBoolProperty(TEXT("Add and wire Event X to Parent: X, passing matching parameters. Default false.")));
    Properties->SetObjectField(TEXT("compileAfterEdit"),
        BuildBoolProperty(TEXT("Compile and roll back addition on compile failure. Default false; saving always compiles.")));
    Properties->SetObjectField(
        TEXT("saveAfterEdit"),
        BuildBoolProperty(TEXT("Compile, save and partially refresh this Blueprint after adding.")));
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("graphGuid")),
        MakeShared<FJsonValueString>(TEXT("functionName"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
