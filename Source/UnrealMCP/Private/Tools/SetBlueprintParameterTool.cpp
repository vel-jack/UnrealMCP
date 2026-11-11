#include "Tools/SetBlueprintParameterTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintGraphEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    // Collects nodes whose pins mirror the edited signature (call sites, macro instances, composite tunnels),
    // the same set the Details panel renames. FBasePinChangeHelper is exported by UnrealEd; the panel's own helper is private.
    struct FCollectSignatureUsers final : FBasePinChangeHelper
    {
        TSet<UK2Node*> Nodes;
        virtual void EditCompositeTunnelNode(UK2Node_Tunnel* TunnelNode) override { Nodes.Add(TunnelNode); }
        virtual void EditMacroInstance(UK2Node_MacroInstance* MacroInstance, UBlueprint*) override { Nodes.Add(MacroInstance); }
        virtual void EditCallSite(UK2Node_CallFunction* CallSite, UBlueprint*) override { Nodes.Add(CallSite); }
    };

    int32 CountLinks(UBlueprint* Blueprint)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        int32 Count = 0;
        for (const UEdGraph* Graph : Graphs)
        {
            if (Graph == nullptr) continue;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node == nullptr) continue;
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (Pin != nullptr && Pin->Direction == EGPD_Output) Count += Pin->LinkedTo.Num();
                }
            }
        }
        return Count;
    }

    // ReconstructNode keeps links by pin name even when the new types no longer match; find those.
    TArray<TPair<UEdGraphPin*, UEdGraphPin*>> FindIncompatibleLinks(UBlueprint* Blueprint)
    {
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        TArray<TPair<UEdGraphPin*, UEdGraphPin*>> Result;
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            if (Graph == nullptr) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node == nullptr) continue;
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (Pin == nullptr || Pin->Direction != EGPD_Output) continue;
                    for (UEdGraphPin* Other : Pin->LinkedTo)
                    {
                        if (Other != nullptr && !Schema->ArePinsCompatible(Pin, Other, Blueprint->SkeletonGeneratedClass))
                            Result.Emplace(Pin, Other);
                    }
                }
            }
        }
        return Result;
    }

    TArray<TSharedPtr<FJsonValue>> DescribeLinks(const UEdGraphNode* Node, const FName PinName)
    {
        TArray<TSharedPtr<FJsonValue>> Links;
        const UEdGraphPin* Pin = Node != nullptr ? Node->FindPin(PinName) : nullptr;
        if (Pin == nullptr) return Links;
        for (const UEdGraphPin* Other : Pin->LinkedTo)
        {
            if (Other == nullptr || Other->GetOwningNode() == nullptr) continue;
            TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
            Link->SetStringField(TEXT("nodeGuid"), UnrealMCP::BlueprintGraphEditToolUtils::GetNodeGuid(Other->GetOwningNode()));
            Link->SetStringField(TEXT("pinName"), Other->PinName.ToString());
            Links.Add(MakeShared<FJsonValueObject>(Link));
        }
        return Links;
    }

    UK2Node_EditablePinBase* FindEditableNode(UBlueprint* Blueprint, const FGuid& NodeGuid)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            if (Graph == nullptr) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node != nullptr && Node->NodeGuid == NodeGuid) return Cast<UK2Node_EditablePinBase>(Node);
            }
        }
        return nullptr;
    }

    TSharedPtr<FUserPinInfo> FindUserPin(UK2Node_EditablePinBase* Node, const FString& Name)
    {
        for (const TSharedPtr<FUserPinInfo>& UserPin : Node->UserDefinedPins)
        {
            if (UserPin.IsValid() && UserPin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase)) return UserPin;
        }
        return nullptr;
    }
}

FSetBlueprintParameterTool::FSetBlueprintParameterTool()
    : FMCPToolBase(
        TEXT("SetBlueprintParameter"),
        TEXT("Changes the type and/or name of one parameter on a custom event, function entry or function result node, updating call sites in one undoable transaction. Rolls back when links would break (unless confirm=true) or compilation fails."))
{
}

UnrealMCP::FMCPResponse FSetBlueprintParameterTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, NodeGuidString, ParameterName, NewName, TypeName, TypeObjectPath, ContainerTypeName, ValueTypeName, ValueTypeObjectPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("nodeGuid"), NodeGuidString)
        || !Request.Params->TryGetStringField(TEXT("parameterName"), ParameterName)
        || ObjectPath.IsEmpty() || ParameterName.IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams,
            TEXT("SetBlueprintParameter requires objectPath, nodeGuid and parameterName, plus type and/or newName."));
    }
    Request.Params->TryGetStringField(TEXT("newName"), NewName);
    Request.Params->TryGetStringField(TEXT("type"), TypeName);
    Request.Params->TryGetStringField(TEXT("typeObjectPath"), TypeObjectPath);
    Request.Params->TryGetStringField(TEXT("containerType"), ContainerTypeName);
    Request.Params->TryGetStringField(TEXT("valueType"), ValueTypeName);
    Request.Params->TryGetStringField(TEXT("valueTypeObjectPath"), ValueTypeObjectPath);
    if (ContainerTypeName.IsEmpty()) ContainerTypeName = TEXT("none");
    const bool bChangeType = !TypeName.IsEmpty();
    const bool bRename = !NewName.IsEmpty() && !NewName.Equals(ParameterName, ESearchCase::CaseSensitive);
    if (!bChangeType && !bRename)
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Provide type (with typeObjectPath for object/class types) and/or a different newName."));
    }
    FGuid NodeGuid;
    if (!FGuid::Parse(NodeGuidString, NodeGuid))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("nodeGuid is not a valid GUID."));
    }
    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bConfirm = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirm"), false);
    const bool bCompile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compile"), true);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);

    TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
    FString ExecutionError;
    bool bApplied = false;
    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        UK2Node_EditablePinBase* TargetNode = FindEditableNode(Blueprint, NodeGuid);
        if (TargetNode == nullptr)
        {
            OutError = TEXT("nodeGuid does not identify a custom event, function entry or function result node in this Blueprint.");
            return false;
        }
        const TSharedPtr<FUserPinInfo> UserPin = FindUserPin(TargetNode, ParameterName);
        if (!UserPin.IsValid())
        {
            OutError = FString::Printf(TEXT("Node has no user-defined parameter named '%s'."), *ParameterName);
            return false;
        }
        const FName OldName = UserPin->PinName;
        const FName FinalName = bRename ? FName(*NewName) : OldName;

        FEdGraphPinType NewType = UserPin->PinType;
        if (bChangeType && !BlueprintEditToolUtils::BuildContainerPinType(
                TypeName, TypeObjectPath, ContainerTypeName, ValueTypeName, ValueTypeObjectPath, NewType, OutError))
        {
            return false;
        }

        // Function inputs and outputs share one namespace, so terminal set = entry + every result node of that function.
        TArray<UK2Node_EditablePinBase*> Terminals;
        if (TargetNode->IsA<UK2Node_FunctionEntry>() || TargetNode->IsA<UK2Node_FunctionResult>())
        {
            for (UEdGraphNode* Node : TargetNode->GetGraph()->Nodes)
            {
                if (Node && (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_FunctionResult>()))
                    Terminals.Add(CastChecked<UK2Node_EditablePinBase>(Node));
            }
        }
        else
        {
            Terminals.Add(TargetNode);
        }

        Evidence->SetStringField(TEXT("nodeClass"), TargetNode->GetClass()->GetName());
        Evidence->SetStringField(TEXT("graphName"), TargetNode->GetGraph()->GetName());
        Evidence->SetStringField(TEXT("graphGuid"), TargetNode->GetGraph()->GraphGuid.ToString());
        Evidence->SetObjectField(TEXT("typeBefore"), BlueprintToolUtils::SerializePinType(UserPin->PinType));
        Evidence->SetArrayField(TEXT("linksBefore"), DescribeLinks(TargetNode, OldName));
        const int32 LinkCountBefore = CountLinks(Blueprint);
        const EBlueprintStatus StatusBefore = Blueprint->Status;

        bool bApplyFailed = false;
        TArray<UK2Node*> RenamedNodes;
        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "SetBlueprintParameter", "UnrealMCP Set Blueprint Parameter"));
            Blueprint->Modify();
            if (bRename)
            {
                FCollectSignatureUsers Users;
                Users.Broadcast(Blueprint, TargetNode, TargetNode->GetGraph());
                RenamedNodes.Append(Terminals);
                RenamedNodes.Append(Users.Nodes.Array());
                for (UK2Node* Node : RenamedNodes)
                {
                    if (Node->RenameUserDefinedPin(OldName, FinalName, true) == ERenamePinResult::ERenamePinResult_NameCollision)
                    {
                        OutError = FString::Printf(TEXT("'%s' collides with an existing pin on %s."), *FinalName.ToString(), *Node->GetName());
                        bApplyFailed = true;
                        break;
                    }
                }
                if (!bApplyFailed)
                {
                    for (UK2Node* Node : RenamedNodes)
                    {
                        Node->Modify();
                        Node->RenameUserDefinedPin(OldName, FinalName, false);
                    }
                    for (UK2Node_EditablePinBase* Terminal : Terminals)
                    {
                        if (const TSharedPtr<FUserPinInfo> Pin = FindUserPin(Terminal, OldName.ToString())) Pin->PinName = FinalName;
                    }
                    // Function inputs are readable as local "parameter" variables inside the function graph.
                    if (TargetNode->IsA<UK2Node_FunctionEntry>())
                    {
                        for (UEdGraphNode* Node : TargetNode->GetGraph()->Nodes)
                        {
                            UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node);
                            if (Variable && Variable->VariableReference.IsLocalScope() && Variable->GetVarName() == OldName)
                            {
                                Variable->Modify();
                                Variable->VariableReference.SetLocalMember(FinalName,
                                    Variable->VariableReference.GetMemberScopeName(), Variable->VariableReference.GetMemberGuid());
                                Variable->ReconstructNode();
                            }
                        }
                    }
                }
            }
            if (!bApplyFailed && bChangeType)
            {
                for (UK2Node_EditablePinBase* Terminal : Terminals)
                {
                    const TSharedPtr<FUserPinInfo> Pin = FindUserPin(Terminal, FinalName.ToString());
                    if (!Pin.IsValid()) continue;
                    Terminal->Modify();
                    Pin->PinType = NewType;
                    // Same as the Details panel: const-ref events/array inputs, and the old default no longer fits.
                    if (!NewType.bIsConst && Terminal->ShouldUseConstRefParams())
                        Pin->PinType.bIsConst = NewType.IsArray() || NewType.bIsReference;
                    Pin->PinDefaultValue.Reset();
                }
            }
            if (!bApplyFailed)
            {
                for (UK2Node_EditablePinBase* Terminal : Terminals)
                {
                    const bool bOldOrphanSaving = Terminal->bDisableOrphanPinSaving;
                    Terminal->bDisableOrphanPinSaving = true;
                    Terminal->ReconstructNode();
                    Terminal->bDisableOrphanPinSaving = bOldOrphanSaving;
                }
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
                FParamsChangedHelper CallSites;
                CallSites.Broadcast(Blueprint, TargetNode, TargetNode->GetGraph());
                for (UBlueprint* Modified : CallSites.ModifiedBlueprints)
                {
                    if (Modified != Blueprint) FBlueprintEditorUtils::MarkBlueprintAsModified(Modified);
                }
                // Cut type-incompatible links so the result is consistent; unconfirmed edits are rolled back below anyway.
                for (const TPair<UEdGraphPin*, UEdGraphPin*>& Link : FindIncompatibleLinks(Blueprint))
                {
                    GetDefault<UEdGraphSchema_K2>()->BreakSinglePinLink(Link.Key, Link.Value);
                }
            }
        }

        const int32 LinkCountAfter = CountLinks(Blueprint);
        Evidence->SetObjectField(TEXT("typeAfter"), BlueprintToolUtils::SerializePinType(NewType));
        Evidence->SetArrayField(TEXT("linksAfter"), DescribeLinks(TargetNode, FinalName));
        Evidence->SetNumberField(TEXT("blueprintLinksBefore"), LinkCountBefore);
        Evidence->SetNumberField(TEXT("blueprintLinksAfter"), LinkCountAfter);
        Evidence->SetNumberField(TEXT("brokenLinkCount"), FMath::Max(0, LinkCountBefore - LinkCountAfter));
        Evidence->SetNumberField(TEXT("renamedNodeCount"), RenamedNodes.Num());

        auto Rollback = [&]()
        {
            const bool bRolledBack = GEditor != nullptr && GEditor->UndoTransaction();
            Evidence->SetBoolField(TEXT("rolledBack"), bRolledBack);
            return bRolledBack;
        };
        if (bApplyFailed) { Rollback(); return false; }
        if (bDryRun) { Rollback(); return true; }
        if (LinkCountAfter < LinkCountBefore && !bConfirm)
        {
            Rollback();
            OutError = FString::Printf(TEXT("This change breaks %d link(s); inspect brokenLinkCount/linksBefore in a dryRun, then pass confirm=true."),
                LinkCountBefore - LinkCountAfter);
            return false;
        }

        if (bCompile)
        {
            FCompilerResultsLog Log;
            Log.bSilentMode = true;
            FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Log);
            Evidence->SetNumberField(TEXT("compileErrors"), Log.NumErrors);
            Evidence->SetNumberField(TEXT("compileWarnings"), Log.NumWarnings);
            const bool bCompiled = Log.NumErrors == 0 && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
            Evidence->SetBoolField(TEXT("compiled"), bCompiled);
            // A Blueprint that was already broken is not this edit's failure; report, don't roll back.
            if (!bCompiled && StatusBefore != BS_Error)
            {
                Rollback();
                FCompilerResultsLog RollbackLog;
                RollbackLog.bSilentMode = true;
                FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &RollbackLog);
                OutError = FString::Printf(TEXT("Blueprint compilation failed with %d error(s); the change was rolled back."), Log.NumErrors);
                return false;
            }
        }

        bApplied = true;
        FString SavedFilename, IndexError;
        bool bIndexRefreshed = false;
        const bool bSaveOk = BlueprintGraphEditToolUtils::SaveAndRefreshIfRequested(Blueprint, ObjectPath, bSave, SavedFilename, bIndexRefreshed, IndexError, OutError);
        Evidence->SetBoolField(TEXT("saved"), bSave && bSaveOk);
        Evidence->SetStringField(TEXT("savedFilename"), SavedFilename);
        Evidence->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
        Evidence->SetStringField(TEXT("indexRefreshError"), IndexError);
        return bSaveOk;
    }, ExecutionError);

    Evidence->SetStringField(TEXT("objectPath"), ObjectPath);
    Evidence->SetStringField(TEXT("nodeGuid"), NodeGuidString);
    Evidence->SetStringField(TEXT("parameterName"), ParameterName);
    Evidence->SetStringField(TEXT("newName"), bRename ? NewName : ParameterName);
    Evidence->SetBoolField(TEXT("dryRun"), bDryRun);
    Evidence->SetBoolField(TEXT("applied"), bApplied);
    if (!bSucceeded)
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError, Evidence);
    }
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    for (const auto& Field : Evidence->Values) Result->SetField(Field.Key, Field.Value);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FSetBlueprintParameterTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    P->SetObjectField(TEXT("nodeGuid"), BuildStringProperty(TEXT("Custom event, function entry or function result node GUID (any graph, including collapsed).")));
    P->SetObjectField(TEXT("parameterName"), BuildStringProperty(TEXT("Current parameter name.")));
    P->SetObjectField(TEXT("newName"), BuildStringProperty(TEXT("Optional new parameter name; call sites are renamed too.")));
    P->SetObjectField(TEXT("type"), BuildStringProperty(TEXT("Optional new type (same vocabulary as AddBlueprintFunctionParameter), e.g. object.")));
    P->SetObjectField(TEXT("typeObjectPath"), BuildStringProperty(TEXT("Class/struct/enum path for object, class, struct or enum types, e.g. /Game/BP_Weapon.BP_Weapon_C.")));
    P->SetObjectField(TEXT("containerType"), BuildStringProperty(TEXT("none, array, set, or map. Default none.")));
    P->SetObjectField(TEXT("valueType"), BuildStringProperty(TEXT("Map value type.")));
    P->SetObjectField(TEXT("valueTypeObjectPath"), BuildStringProperty(TEXT("Class path for object/class Map values.")));
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Apply, measure broken links, then roll back.")));
    P->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required when the change breaks existing links.")));
    P->SetObjectField(TEXT("compile"), BuildBoolProperty(TEXT("Compile after the edit and roll back on new errors. Default true.")));
    P->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index after a successful edit.")));
    Schema->SetObjectField(TEXT("properties"), P);
    Schema->SetArrayField(TEXT("required"), {
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("nodeGuid")),
        MakeShared<FJsonValueString>(TEXT("parameterName")) });
    return Schema;
}
