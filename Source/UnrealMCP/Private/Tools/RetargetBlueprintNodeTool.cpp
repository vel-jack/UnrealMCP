#include "Tools/RetargetBlueprintNodeTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_DynamicCast.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    // The cast's result pin is named after the target class, so after a retarget its old pin is orphaned
    // (a deleted class leaves it orphaned already). Find whichever pin currently carries the object result.
    UEdGraphPin* FindResultPin(const UK2Node_DynamicCast* Cast)
    {
        if (UEdGraphPin* Pin = Cast->GetCastResultPin())
        {
            return Pin;
        }
        for (UEdGraphPin* Pin : Cast->Pins)
        {
            if (Pin->Direction == EGPD_Output
                && (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object
                    || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface
                    || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class))
            {
                return Pin;
            }
        }
        return nullptr;
    }
}

FRetargetBlueprintNodeTool::FRetargetBlueprintNodeTool()
    : FMCPToolBase(TEXT("RetargetBlueprintNode"), TEXT("Retargets a cast node (including a 'Bad cast node' whose class was deleted) to newClassPath without needing the old class. Output links whose pins still exist survive; the old class-named result pin's links move to the new result pin. Requires confirm=true unless dryRun=true."))
{
}

UnrealMCP::FMCPResponse FRetargetBlueprintNodeTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    namespace Edit = UnrealMCP::BlueprintEditToolUtils;
    FString ObjectPath, GraphName, GraphGuid, NodeGuid, NewClassPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("nodeGuid"), NodeGuid)
        || !Request.Params->TryGetStringField(TEXT("newClassPath"), NewClassPath))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("RetargetBlueprintNode requires objectPath, nodeGuid, newClassPath and graphName or graphGuid."));
    }
    Request.Params->TryGetStringField(TEXT("graphName"), GraphName);
    Request.Params->TryGetStringField(TEXT("graphGuid"), GraphGuid);
    if (GraphName.IsEmpty() && GraphGuid.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("RetargetBlueprintNode requires graphName or graphGuid."));
    }

    const bool bDryRun = Edit::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bConfirm = Edit::GetOptionalBool(Request.Params, TEXT("confirm"), false);
    const bool bSave = Edit::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    if (!bDryRun && !bConfirm)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("RetargetBlueprintNode requires confirm=true unless dryRun=true."));
    }

    FString OldClassPath, SavedFilename, ExecutionError;
    bool bWasBroken = false;
    int32 ResultLinksBefore = 0;
    int32 ResultLinksKept = 0;
    TArray<FString> DroppedLinks;

    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        UEdGraph* Graph = nullptr;
        UEdGraphNode* Node = nullptr;
        UClass* NewClass = nullptr;
        if (!Edit::ResolveBlueprint(ObjectPath, Blueprint, OutError)
            || !UnrealMCP::BlueprintGraphEditToolUtils::ResolveGraph(Blueprint, GraphName, GraphGuid, Graph, OutError)
            || !UnrealMCP::BlueprintGraphEditToolUtils::ResolveNode(Graph, NodeGuid, Node, OutError)
            || !Edit::ResolveClass(NewClassPath, NewClass, OutError))
        {
            return false;
        }

        UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node);
        if (!CastNode)
        {
            OutError = TEXT("RetargetBlueprintNode currently supports cast nodes only (K2Node_DynamicCast and subclasses).");
            return false;
        }

        OldClassPath = CastNode->TargetType ? CastNode->TargetType->GetPathName() : FString();
        bWasBroken = CastNode->TargetType == nullptr;

        // Snapshot the result pin's links before reconstruction orphans it.
        TArray<UEdGraphPin*> ResultLinks;
        if (const UEdGraphPin* ResultPin = FindResultPin(CastNode))
        {
            ResultLinks = ResultPin->LinkedTo;
        }
        ResultLinksBefore = ResultLinks.Num();
        if (bDryRun)
        {
            ResultLinksKept = ResultLinksBefore;
            return true;
        }

        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "RetargetBlueprintNode", "UnrealMCP Retarget Blueprint Node"));
        Blueprint->Modify();
        Graph->Modify();
        CastNode->Modify();
        CastNode->TargetType = NewClass;
        CastNode->ReconstructNode();

        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        UEdGraphPin* NewResult = CastNode->GetCastResultPin();
        for (UEdGraphPin* Linked : ResultLinks)
        {
            if (!NewResult || !Linked)
            {
                continue;
            }
            if (NewResult->LinkedTo.Contains(Linked) || Schema->TryCreateConnection(NewResult, Linked))
            {
                ++ResultLinksKept;
            }
            else
            {
                DroppedLinks.Add(FString::Printf(TEXT("%s.%s"), *Linked->GetOwningNode()->GetName(), *Linked->PinName.ToString()));
            }
        }

        // Orphaned pins left by reconstruction (old class-named result pin) have served their purpose.
        TArray<UEdGraphPin*> Orphans;
        for (UEdGraphPin* Pin : CastNode->Pins)
        {
            if (Pin->bOrphanedPin)
            {
                Orphans.Add(Pin);
            }
        }
        for (UEdGraphPin* Orphan : Orphans)
        {
            CastNode->RemovePin(Orphan);
        }

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        if (bSave && !Edit::SaveAsset(Blueprint, SavedFilename, OutError))
        {
            return false;
        }
        return true;
    }, ExecutionError);

    if (!bOk)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("nodeGuid"), NodeGuid);
    Result->SetStringField(TEXT("oldClassPath"), OldClassPath);
    Result->SetStringField(TEXT("newClassPath"), NewClassPath);
    Result->SetBoolField(TEXT("wasBroken"), bWasBroken);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("retargeted"), !bDryRun);
    Result->SetNumberField(TEXT("resultLinksBefore"), ResultLinksBefore);
    Result->SetNumberField(TEXT("resultLinksKept"), ResultLinksKept);
    TArray<TSharedPtr<FJsonValue>> Dropped;
    for (const FString& Entry : DroppedLinks)
    {
        Dropped.Add(MakeShared<FJsonValueString>(Entry));
    }
    Result->SetArrayField(TEXT("droppedLinks"), Dropped);
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FRetargetBlueprintNodeTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    auto AddProp = [&Properties](const TCHAR* PropName, const TCHAR* PropType, const TCHAR* PropDesc)
    {
        TSharedRef<FJsonObject> Prop = MakeShared<FJsonObject>();
        Prop->SetStringField(TEXT("type"), PropType);
        Prop->SetStringField(TEXT("description"), PropDesc);
        Properties->SetObjectField(PropName, Prop);
    };
    AddProp(TEXT("objectPath"), TEXT("string"), TEXT("Blueprint asset object path."));
    AddProp(TEXT("graphName"), TEXT("string"), TEXT("Graph name from ListBlueprintGraphs."));
    AddProp(TEXT("graphGuid"), TEXT("string"), TEXT("Graph GUID; alternative to graphName."));
    AddProp(TEXT("nodeGuid"), TEXT("string"), TEXT("GUID of the cast node to retarget."));
    AddProp(TEXT("newClassPath"), TEXT("string"), TEXT("Class to cast to, e.g. /Game/BP_Vehicles.BP_Vehicles_C"));
    AddProp(TEXT("dryRun"), TEXT("boolean"), TEXT("Report what would be retargeted."));
    AddProp(TEXT("confirm"), TEXT("boolean"), TEXT("Required for a real retarget."));
    AddProp(TEXT("saveAfterEdit"), TEXT("boolean"), TEXT("Save after the edit (default false)."));
    Schema->SetObjectField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Required;
    for (const TCHAR* Required_ : {TEXT("objectPath"), TEXT("nodeGuid"), TEXT("newClassPath")})
    {
        Required.Add(MakeShared<FJsonValueString>(Required_));
    }
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
