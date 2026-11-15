#include "Tools/UserDefinedStructMemberTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Kismet2/StructureEditorUtils.h"
#include "ScopedTransaction.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace
{
    TArray<FName> DirtyNames()
    {
        TArray<UPackage*> Packages;
        FEditorFileUtils::GetDirtyContentPackages(Packages);
        FEditorFileUtils::GetDirtyWorldPackages(Packages);
        TArray<FName> Names;
        for (const UPackage* Package : Packages) if (Package) Names.AddUnique(Package->GetFName());
        return Names;
    }
    TArray<TSharedPtr<FJsonValue>> NamesJson(const TArray<FName>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (FName MemberName : Names) Values.Add(MakeShared<FJsonValueString>(MemberName.ToString()));
        return Values;
    }
}

FUserDefinedStructMemberTool::FUserDefinedStructMemberTool(EUserStructMemberOperation InOperation)
    : FMCPToolBase(InOperation == EUserStructMemberOperation::Inspect ? TEXT("InspectUserDefinedStruct") :
        InOperation == EUserStructMemberOperation::Add ? TEXT("AddUserDefinedStructMember") : TEXT("RemoveUserDefinedStructMember"),
        TEXT("Inspect, add or remove authored members of a user-defined struct using Unreal's structure editor. Inspection includes member GUIDs, authored/internal names, types and defaults. Mutations support dryRun, require confirm, compile the struct, never save dependents, and report newly dirtied packages. Referenced structs require allowReferenced=true; removal can discard stored member values. Native structs are excluded.")), Operation(InOperation) {}

UnrealMCP::FMCPResponse FUserDefinedStructMemberTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    namespace Edit = BlueprintEditToolUtils;
    const bool bInspect = Operation == EUserStructMemberOperation::Inspect;
    const bool bAdd = Operation == EUserStructMemberOperation::Add;
    FString Path, MemberName, GuidText;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), Path) || Path.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("objectPath is required."));
    Request.Params->TryGetStringField(TEXT("memberName"), MemberName);
    Request.Params->TryGetStringField(TEXT("memberGuid"), GuidText);
    if (!bInspect && ((bAdd && MemberName.IsEmpty()) || (!bAdd && MemberName.IsEmpty() && GuidText.IsEmpty())))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Add requires memberName; remove requires memberGuid or memberName."));
    const bool bDry = Edit::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = Edit::GetOptionalBool(Request.Params, TEXT("save"), false);
    if (!bInspect && !bDry && !Edit::GetOptionalBool(Request.Params, TEXT("confirm"), false))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Apply requires confirm=true."));
    auto Result = BuildBooleanResult(true);
    FString Error;
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UUserDefinedStruct* Struct = LoadObject<UUserDefinedStruct>(nullptr, *Path);
        if (!Struct) { OutError = TEXT("Path does not resolve to a user-defined struct asset."); return false; }
        Result->SetStringField(TEXT("objectPath"), Struct->GetPathName());
        auto Describe = [&]()
        {
            TArray<TSharedPtr<FJsonValue>> Members;
            for (const FStructVariableDescription& V : FStructureEditorUtils::GetVarDesc(Struct))
            {
                auto Member = MakeShared<FJsonObject>();
                Member->SetStringField(TEXT("memberGuid"), V.VarGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
                Member->SetStringField(TEXT("memberName"), V.FriendlyName);
                Member->SetStringField(TEXT("internalName"), V.VarName.ToString());
                Member->SetStringField(TEXT("defaultValue"), V.DefaultValue);
                Member->SetObjectField(TEXT("type"), BlueprintToolUtils::SerializePinType(V.ToPinType()));
                Members.Add(MakeShared<FJsonValueObject>(Member));
            }
            Result->SetArrayField(TEXT("members"), Members);
            Result->SetBoolField(TEXT("isDirty"), Struct->GetPackage()->IsDirty());
        };
        if (bInspect) { Describe(); return true; }
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        if (Registry.IsLoadingAssets()) { OutError = TEXT("Wait for asset discovery before struct edits."); return false; }
        TArray<FName> Referencers;
        Registry.GetReferencers(Struct->GetPackage()->GetFName(), Referencers);
        Referencers.Remove(Struct->GetPackage()->GetFName());
        Result->SetArrayField(TEXT("referencerPackages"), NamesJson(Referencers));
        Result->SetBoolField(TEXT("dryRun"), bDry);
        FGuid Guid;
        FEdGraphPinType Type;
        if (bAdd)
        {
            if (!FStructureEditorUtils::IsUniqueVariableFriendlyName(Struct, MemberName))
            { OutError = TEXT("Member name is invalid or already exists."); return false; }
            FString TypeName, TypePath, Container, ValueType, ValuePath;
            if (!Request.Params->TryGetStringField(TEXT("type"), TypeName)) { OutError = TEXT("type is required."); return false; }
            Request.Params->TryGetStringField(TEXT("typeObjectPath"), TypePath);
            Request.Params->TryGetStringField(TEXT("containerType"), Container);
            Request.Params->TryGetStringField(TEXT("valueType"), ValueType);
            Request.Params->TryGetStringField(TEXT("valueTypeObjectPath"), ValuePath);
            if (Container.IsEmpty()) Container = TEXT("none");
            if (!Edit::BuildContainerPinType(TypeName, TypePath, Container, ValueType, ValuePath, Type, OutError)
                || !FStructureEditorUtils::CanHaveAMemberVariableOfType(Struct, Type, &OutError)) return false;
        }
        else
        {
            if (!GuidText.IsEmpty() && !FGuid::Parse(GuidText, Guid)) { OutError = TEXT("Invalid memberGuid."); return false; }
            const auto* Member = FStructureEditorUtils::GetVarDesc(Struct).FindByPredicate([&](const FStructVariableDescription& V)
            { return GuidText.IsEmpty() ? (V.FriendlyName == MemberName || V.VarName == FName(*MemberName)) : V.VarGuid == Guid; });
            if (!Member) { OutError = TEXT("Member not found."); return false; }
            if (!MemberName.IsEmpty() && !GuidText.IsEmpty() && Member->FriendlyName != MemberName && Member->VarName != FName(*MemberName))
            { OutError = TEXT("memberName does not match memberGuid."); return false; }
            Guid = Member->VarGuid;
            if (FStructureEditorUtils::GetVarDesc(Struct).Num() <= 1) { OutError = TEXT("Cannot remove the last member of a user-defined struct."); return false; }
        }
        Describe();
        if (bDry) return true;
        if (!Referencers.IsEmpty() && !Edit::GetOptionalBool(Request.Params, TEXT("allowReferenced"), false))
        { OutError = TEXT("Struct has referencers. Review dryRun and pass allowReferenced=true; removing a member discards stored values and may break dependent Blueprints."); return false; }
        if (!GEditor || GEditor->IsTransactionActive()) { OutError = TEXT("An idle editor transaction system is required."); return false; }
        const auto Before = DirtyNames();
        const bool bDirtyBefore = Struct->GetPackage()->IsDirty();
        bool bChanged = false;
        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "EditStructMember", "UnrealMCP Edit Struct Member"));
            // StructureEditorUtils transacts editor data; transacting compiled fields makes undo unsafe.
            if (bAdd)
            {
                bChanged = FStructureEditorUtils::AddVariable(Struct, Type);
                if (bChanged)
                {
                    Guid = FStructureEditorUtils::GetVarDesc(Struct).Last().VarGuid;
                    bChanged = FStructureEditorUtils::RenameVariable(Struct, Guid, MemberName);
                }
            }
            else bChanged = FStructureEditorUtils::RemoveVariable(Struct, Guid);
        }
        const bool bCompiled = Struct->Status == UDSS_UpToDate;
        Result->SetBoolField(TEXT("compiled"), bCompiled);
        if (!bChanged || !bCompiled)
        {
            const bool bRolledBack = GEditor->UndoTransaction();
            Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
            if (bRolledBack) { FStructureEditorUtils::CompileStructure(Struct); Struct->GetPackage()->SetDirtyFlag(bDirtyBefore); }
            OutError = TEXT("Struct edit/compile failed; transaction rollback attempted. Nothing saved.");
        }
        else
        {
            Result->SetBoolField(TEXT("changed"), true);
            Result->SetStringField(TEXT("memberGuid"), Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
            Result->SetBoolField(TEXT("saved"), false);
            if (bSave)
            {
                FString Filename, IndexError;
                if (Edit::SaveAsset(Struct, Filename, OutError))
                {
                    Result->SetBoolField(TEXT("saved"), true);
                    Result->SetBoolField(TEXT("indexRefreshed"), Edit::RefreshAssetIndex(Path, IndexError));
                }
            }
        }
        TArray<FName> NewlyDirty;
        for (FName Package : DirtyNames()) if (!Before.Contains(Package)) NewlyDirty.Add(Package);
        Result->SetArrayField(TEXT("newlyDirtiedPackages"), NamesJson(NewlyDirty));
        Describe();
        return bChanged && bCompiled && OutError.IsEmpty();
    }, Error);
    if (!bOk) { Result->SetBoolField(TEXT("success"), false); return BuildError(Request, EMCPErrorCode::InvalidParams, Error, Result); }
    FMCPResponse Response; Response.Id = Request.Id; Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FUserDefinedStructMemberTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto S = MakeShared<FJsonObject>(), P = MakeShared<FJsonObject>();
    S->SetStringField(TEXT("type"), TEXT("object"));
    P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("User-defined struct asset path; native structs are not supported.")));
    TArray<TSharedPtr<FJsonValue>> Required = {MakeShared<FJsonValueString>(TEXT("objectPath"))};
    if (Operation != EUserStructMemberOperation::Inspect)
    {
        P->SetObjectField(TEXT("memberName"), BuildStringProperty(TEXT("Authored display name; remove also accepts internal name.")));
        if (Operation == EUserStructMemberOperation::Remove)
            P->SetObjectField(TEXT("memberGuid"), BuildStringProperty(TEXT("Stable GUID from InspectUserDefinedStruct; preferred for removal.")));
        else
        {
            Required.Add(MakeShared<FJsonValueString>(TEXT("memberName")));
            Required.Add(MakeShared<FJsonValueString>(TEXT("type")));
            for (const TCHAR* Key : {TEXT("type"), TEXT("typeObjectPath"), TEXT("containerType"), TEXT("valueType"), TEXT("valueTypeObjectPath")})
                P->SetObjectField(Key, BuildStringProperty(TEXT("Type argument as in AddBlueprintVariable; containerType is none, array, set or map.")));
        }
        P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate and report only.")));
        P->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required to apply.")));
        P->SetObjectField(TEXT("allowReferenced"), BuildBoolProperty(TEXT("Allow dependent assets to be reconstructed/dirtied, and potential loss of removed-member data. Default false.")));
        P->SetObjectField(TEXT("save"), BuildBoolProperty(TEXT("Save the edited struct after compilation; never saves dependent packages. Default false.")));
    }
    S->SetObjectField(TEXT("properties"), P); S->SetArrayField(TEXT("required"), Required); return S;
}
