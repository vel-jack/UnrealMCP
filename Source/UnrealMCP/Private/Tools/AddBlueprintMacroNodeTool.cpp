#include "Tools/AddBlueprintMacroNodeTool.h"

#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "K2Node_MacroInstance.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FAddBlueprintMacroNodeTool::FAddBlueprintMacroNodeTool()
    : FMCPToolBase(
        TEXT("AddBlueprintMacroNode"),
        TEXT("Adds a macro instance from a Blueprint macro library, including StandardMacros such as ForEachLoop."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintMacroNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath, GraphName, GraphGuid, MacroName, MacroLibraryPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("macroName"), MacroName))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("AddBlueprintMacroNode requires objectPath, macroName, and graphName or graphGuid."));
    }

    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    Request.Params->TryGetStringField(TEXT("macroLibraryPath"), MacroLibraryPath);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Graph selector is required."));
    }
    if (MacroLibraryPath.IsEmpty())
    {
        MacroLibraryPath = TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    UnrealMCP::BlueprintGraphEditToolUtils::FPlacement Placement;
    FString NodeGuid, SavedFilename, IndexError, ExecutionError;
    bool bIndexRefreshed = false;
    TArray<TSharedPtr<FJsonValue>> Pins;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;

        UEdGraph* TargetGraph = nullptr;
        if (!UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, TargetGraph, OutError)) return false;

        UBlueprint* MacroLibrary = LoadObject<UBlueprint>(nullptr, *MacroLibraryPath);
        if (MacroLibrary == nullptr)
        {
            OutError = FString::Printf(TEXT("Macro library was not found: %s"), *MacroLibraryPath);
            return false;
        }

        UEdGraph* MacroGraph = nullptr;
        for (UEdGraph* Candidate : MacroLibrary->MacroGraphs)
        {
            if (Candidate != nullptr && Candidate->GetName().Equals(MacroName, ESearchCase::IgnoreCase))
            {
                MacroGraph = Candidate;
                break;
            }
        }
        if (MacroGraph == nullptr)
        {
            OutError = FString::Printf(TEXT("Macro '%s' was not found in '%s'."), *MacroName, *MacroLibraryPath);
            return false;
        }

        Placement = UnrealMCP::BlueprintGraphEditToolUtils::ResolvePlacement(TargetGraph, Request.Params, OutError);
        if (!OutError.IsEmpty() || bDryRun) return OutError.IsEmpty();

        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "AddMacroNode", "UnrealMCP Add Macro Node"));
        Blueprint->Modify();
        TargetGraph->Modify();
        UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(TargetGraph);
        Node->SetMacroGraph(MacroGraph);
        UnrealMCP::BlueprintGraphEditToolUtils::PlaceNewNode(TargetGraph, Node, Placement);
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        NodeGuid = UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Node);
        Pins = UnrealMCP::BlueprintGraphEditToolUtils::SerializePins(Node);
        return UnrealMCP::BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(
            Blueprint, ObjectPath, bSave, SavedFilename, bIndexRefreshed, IndexError, OutError);
    }, ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("macroName"), MacroName);
    Result->SetStringField(TEXT("macroLibraryPath"), MacroLibraryPath);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("added"), !bDryRun);
    Result->SetNumberField(TEXT("positionX"), Placement.X);
    Result->SetNumberField(TEXT("positionY"), Placement.Y);
    Result->SetBoolField(TEXT("collisionAdjusted"), Placement.bCollisionAdjusted);
    Result->SetArrayField(TEXT("pins"), Pins);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintMacroNodeTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    for (const TCHAR* FieldName : {TEXT("objectPath"), TEXT("graphName"), TEXT("graphGuid"), TEXT("macroName"),
        TEXT("macroLibraryPath"), TEXT("relativeToNodeGuid")})
    {
        Properties->SetObjectField(FieldName, BuildStringProperty(TEXT("Blueprint graph, macro, or placement selector.")));
    }
    TSharedRef<FJsonObject> Integer = MakeShared<FJsonObject>();
    Integer->SetStringField(TEXT("type"), TEXT("integer"));
    Properties->SetObjectField(TEXT("positionX"), Integer);
    Properties->SetObjectField(TEXT("positionY"), Integer);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate and preview without mutation.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh after adding.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("macroName"))});
    return Schema;
}
