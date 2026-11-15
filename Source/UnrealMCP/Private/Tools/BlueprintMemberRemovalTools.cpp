#include "Tools/BlueprintMemberRemovalTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Misc/ScopeExit.h"
#include "Editor/Transactor.h"
#include "Misc/Change.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    // Recompilation drops the deleted property from the CDO's serialized layout.
    // Record its export text separately so undo can restore it after the declaration.
    class FRemovedDefaultChange final : public FSwapChange
    {
    public:
        FRemovedDefaultChange(UBlueprint* InBlueprint, FName InVariable, FString InValue)
            : Blueprint(InBlueprint), Variable(InVariable), Value(MoveTemp(InValue)) {}
        virtual TUniquePtr<FChange> Execute(UObject*) override
        {
            UBlueprint* BP = Blueprint.Get();
            FString Previous;
            if (BP && BP->NewVariables.ContainsByPredicate([&](const FBPVariableDescription& V) { return V.VarName == Variable; }))
            {
                TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
                UnrealMCP::BlueprintEditToolUtils::CompileWithLocatedMessages(BP, Errors, Warnings);
                if (FProperty* Property = FindFProperty<FProperty>(BP->GeneratedClass, Variable))
                {
                    UObject* CDO = BP->GeneratedClass->GetDefaultObject();
                    Property->ExportText_InContainer(0, Previous, CDO, CDO, CDO, PPF_None);
                    Property->ImportText_Direct(*Value, Property->ContainerPtrToValuePtr<void>(CDO), CDO, PPF_None);
                }
            }
            return MakeUnique<FRemovedDefaultChange>(BP, Variable, Previous);
        }
        virtual FString ToString() const override { return TEXT("Restore removed Blueprint variable default"); }
    private:
        TWeakObjectPtr<UBlueprint> Blueprint;
        FName Variable;
        FString Value;
    };
}

FRemoveBlueprintMemberTool::FRemoveBlueprintMemberTool(bool bInFunction)
    : FMCPToolBase(bInFunction ? TEXT("RemoveBlueprintFunction") : TEXT("RemoveBlueprintVariable"),
        TEXT("Removes one locally declared Blueprint member variable or function graph. Dry-run reports usage and indexed referencer packages. Refuses used members; inherited members, component variables and interface implementations are excluded. Apply requires confirm=true and acknowledgement of external referencers. Compiles with rollback on errors, saves only when requested. External callers using reflection or string names cannot be detected.")), bFunction(bInFunction) {}

UnrealMCP::FMCPResponse FRemoveBlueprintMemberTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    namespace Edit = BlueprintEditToolUtils;
    FString Path, MemberName;
    const TCHAR* NameField = bFunction ? TEXT("functionName") : TEXT("variableName");
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), Path) || Path.IsEmpty()
        || !Request.Params->TryGetStringField(NameField, MemberName) || MemberName.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Requires objectPath and variableName/functionName."));
    const bool bDry = Edit::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = Edit::GetOptionalBool(Request.Params, TEXT("save"), false);
    if (!bDry && !Edit::GetOptionalBool(Request.Params, TEXT("confirm"), false))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Apply requires confirm=true; inspect dryRun first."));
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    FString Error;
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* BP = nullptr;
        if (!Edit::ResolveBlueprint(Path, BP, OutError)) return false;
        UEdGraph* Graph = nullptr;
        if (bFunction)
        {
            for (UEdGraph* Candidate : BP->FunctionGraphs)
                if (Candidate && Candidate->GetFName() == FName(*MemberName)) Graph = Candidate;
            if (!Graph) { OutError = TEXT("No locally declared function graph with this name (inherited/interface/event graphs are excluded)."); return false; }
            for (const FBPInterfaceDescription& Interface : BP->ImplementedInterfaces)
                if (Interface.Interface && Interface.Interface->FindFunctionByName(FName(*MemberName)))
                { OutError = TEXT("Cannot remove an interface implementation; remove the interface explicitly first."); return false; }
        }
        else if (!BP->NewVariables.ContainsByPredicate([&](const FBPVariableDescription& V) { return V.VarName == FName(*MemberName); }))
        { OutError = TEXT("No locally declared member variable with this name (inherited/component variables are excluded)."); return false; }
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        if (Registry.IsLoadingAssets()) { OutError = TEXT("Wait for asset discovery before removing members."); return false; }
        TArray<FName> Referencers;
        Registry.GetReferencers(BP->GetPackage()->GetFName(), Referencers);
        Referencers.Remove(BP->GetPackage()->GetFName());
        TArray<TSharedPtr<FJsonValue>> JsonRefs;
        for (const FName& Ref : Referencers) JsonRefs.Add(MakeShared<FJsonValueString>(Ref.ToString()));
        const bool bUsed = bFunction ? FBlueprintEditorUtils::IsFunctionUsed(BP, FName(*MemberName)) : FBlueprintEditorUtils::IsVariableUsed(BP, FName(*MemberName));
        Result->SetStringField(TEXT("objectPath"), BP->GetPathName());
        Result->SetStringField(NameField, MemberName);
        Result->SetBoolField(TEXT("dryRun"), bDry);
        Result->SetBoolField(TEXT("used"), bUsed);
        Result->SetArrayField(TEXT("referencerPackages"), JsonRefs);
        Result->SetStringField(TEXT("referenceCoverage"), TEXT("Unreal member usage search plus indexed package referencers; reflective/string references are not detectable."));
        if (bDry) return true;
        TArray<UPackage*> DirtyBefore;
        FEditorFileUtils::GetDirtyContentPackages(DirtyBefore);
        ON_SCOPE_EXIT
        {
            TArray<UPackage*> DirtyAfter;
            FEditorFileUtils::GetDirtyContentPackages(DirtyAfter);
            TArray<TSharedPtr<FJsonValue>> NewlyDirty;
            for (const UPackage* Package : DirtyAfter)
                if (!DirtyBefore.Contains(Package)) NewlyDirty.Add(MakeShared<FJsonValueString>(Package->GetName()));
            Result->SetArrayField(TEXT("newlyDirtiedPackages"), NewlyDirty);
        };
        if (bUsed) { OutError = TEXT("Member is used; remove or repoint usages before deleting it."); return false; }
        if (!Referencers.IsEmpty() && !Edit::GetOptionalBool(Request.Params, TEXT("acknowledgeExternalReferences"), false))
        { OutError = TEXT("External packages reference this Blueprint. Review referencerPackages and pass acknowledgeExternalReferences=true after checking dependent assets."); return false; }
        if (!GEditor || GEditor->IsTransactionActive()) { OutError = TEXT("An idle editor transaction system is required."); return false; }
        const bool bDirtyBefore = BP->GetPackage()->IsDirty();
        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "RemoveBlueprintMember", "UnrealMCP Remove Blueprint Member"));
            if (!GUndo) { OutError = TEXT("Undo recording is unavailable; no edit applied."); return false; }
            if (!bFunction && BP->GeneratedClass)
            {
                if (FProperty* Property = FindFProperty<FProperty>(BP->GeneratedClass, FName(*MemberName)))
                {
                    UObject* CDO = BP->GeneratedClass->GetDefaultObject();
                    FString Value;
                    Property->ExportText_InContainer(0, Value, CDO, CDO, CDO, PPF_None);
                    // A custom change cannot share the Blueprint serialization record.
                    UObject* UndoMarker = NewObject<UEnum>(GetTransientPackage(), NAME_None, RF_Transactional);
                    GUndo->StoreUndo(UndoMarker, MakeUnique<FRemovedDefaultChange>(BP, FName(*MemberName), Value));
                }
            }
            BP->Modify();
            if (Graph) { Graph->Modify(); FBlueprintEditorUtils::RemoveGraph(BP, Graph); }
            else FBlueprintEditorUtils::RemoveMemberVariable(BP, FName(*MemberName));
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        }
        TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
        const bool bCompiled = Edit::CompileWithLocatedMessages(BP, Errors, Warnings);
        Result->SetArrayField(TEXT("errors"), Errors);
        Result->SetArrayField(TEXT("warnings"), Warnings);
        Result->SetBoolField(TEXT("compiled"), bCompiled);
        if (!bCompiled)
        {
            const bool bRolledBack = GEditor->UndoTransaction();
            Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
            if (bRolledBack)
            {
                TArray<TSharedPtr<FJsonValue>> RestoreErrors, RestoreWarnings;
                Edit::CompileWithLocatedMessages(BP, RestoreErrors, RestoreWarnings);
                BP->GetPackage()->SetDirtyFlag(bDirtyBefore);
            }
            OutError = TEXT("Compile failed; removal was rolled back where indicated. Nothing saved.");
            return false;
        }
        Result->SetBoolField(TEXT("removed"), true);
        Result->SetBoolField(TEXT("saved"), false);
        if (bSave)
        {
            FString Filename, IndexError;
            if (!Edit::SaveAsset(BP, Filename, OutError)) return false;
            Result->SetBoolField(TEXT("saved"), true);
            Result->SetBoolField(TEXT("indexRefreshed"), Edit::RefreshAssetIndex(Path, IndexError));
        }
        Result->SetBoolField(TEXT("isDirty"), BP->GetPackage()->IsDirty());
        return true;
    }, Error);
    if (!bOk) { Result->SetBoolField(TEXT("success"), false); return BuildError(Request, EMCPErrorCode::InvalidParams, Error, Result); }
    FMCPResponse Response; Response.Id = Request.Id; Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FRemoveBlueprintMemberTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> S = MakeShared<FJsonObject>(), P = MakeShared<FJsonObject>();
    S->SetStringField(TEXT("type"), TEXT("object"));
    P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Blueprint asset path.")));
    const TCHAR* MemberName = bFunction ? TEXT("functionName") : TEXT("variableName");
    P->SetObjectField(MemberName, BuildStringProperty(TEXT("Name declared on this Blueprint.")));
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Report only; no changes.")));
    P->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required to remove.")));
    P->SetObjectField(TEXT("save"), BuildBoolProperty(TEXT("Save only after successful compilation. Default false.")));
    P->SetObjectField(TEXT("acknowledgeExternalReferences"), BuildBoolProperty(TEXT("Acknowledge indexed external packages after reviewing dryRun. Does not bypass used-member refusal.")));
    S->SetObjectField(TEXT("properties"), P);
    S->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(MemberName)});
    return S;
}
