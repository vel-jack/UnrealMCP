#include "Tools/Data/UserDefinedStructMemberTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectIterator.h"
#include "ScopedTransaction.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace
{
    using EOperation = EUserStructMemberOperation;
    bool IsEnum(EOperation Operation)
    {
        return Operation == EOperation::InspectEnum || Operation == EOperation::AddEnum || Operation == EOperation::SetEnumDisplayName;
    }

    const TCHAR* ToolName(EOperation Operation)
    {
        switch (Operation)
        {
        case EOperation::Inspect: return TEXT("InspectUserDefinedStruct");
        case EOperation::Add: return TEXT("AddUserDefinedStructMember");
        case EOperation::Remove: return TEXT("RemoveUserDefinedStructMember");
        case EOperation::Rename: return TEXT("SetUserDefinedStructMemberName");
        case EOperation::SetDefault: return TEXT("SetUserDefinedStructMemberDefault");
        case EOperation::InspectEnum: return TEXT("InspectUserDefinedEnum");
        case EOperation::AddEnum: return TEXT("AddUserDefinedEnumEntry");
        case EOperation::SetEnumDisplayName: return TEXT("SetUserDefinedEnumEntryDisplayName");
        }
        return TEXT("");
    }

    TArray<TSharedPtr<FJsonValue>> FindAffectedPins(UBlueprint* Blueprint, const UObject* Asset)
    {
        TArray<TSharedPtr<FJsonValue>> Pins;
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        const auto Matches = [Asset](const FEdGraphPinType& Type)
        {
            return Type.PinSubCategoryObject.Get() == Asset || Type.PinValueType.TerminalSubCategoryObject.Get() == Asset;
        };
        for (const UEdGraph* Graph : Graphs)
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin || (!Matches(Pin->PinType) && (!Pin->ParentPin || !Matches(Pin->ParentPin->PinType)))) continue;
                    auto Item = MakeShared<FJsonObject>();
                    Item->SetStringField(TEXT("graphPath"), Graph->GetPathName(Blueprint));
                    Item->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString());
                    Item->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
                    Pins.Add(MakeShared<FJsonValueObject>(Item));
                }
            }
        return Pins;
    }
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
    : FMCPToolBase(ToolName(InOperation), IsEnum(InOperation)
        ? TEXT("Inspect or edit entries of a Blueprint enum by stable internal name. Supports append and display-name edits, preserving existing values and order.")
        : TEXT("Inspect or edit authored Blueprint struct members by GUID or name. Removal can discard stored member values.")), Operation(InOperation) {}

UnrealMCP::FMCPResponse FUserDefinedStructMemberTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    namespace Edit = BlueprintEditToolUtils;
    const bool bEnum = IsEnum(Operation);
    const bool bInspect = Operation == EOperation::Inspect || Operation == EOperation::InspectEnum;
    const bool bAdd = Operation == EUserStructMemberOperation::Add;
    FString Path, MemberName, GuidText, NewValue, EntryName;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), Path) || Path.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("objectPath is required."));
    Request.Params->TryGetStringField(TEXT("memberName"), MemberName);
    Request.Params->TryGetStringField(TEXT("memberGuid"), GuidText);
    if (!bInspect && !bEnum && ((bAdd && MemberName.IsEmpty()) || (!bAdd && MemberName.IsEmpty() && GuidText.IsEmpty())))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Add requires memberName; remove requires memberGuid or memberName."));
    const TCHAR* ValueField = bEnum ? TEXT("displayName") : Operation == EOperation::Rename ? TEXT("newName") : TEXT("defaultValue");
    if ((bEnum && !bInspect) || Operation == EOperation::Rename || Operation == EOperation::SetDefault)
        if (!Request.Params->TryGetStringField(ValueField, NewValue)
            || (Operation != EOperation::SetDefault && NewValue.TrimStartAndEnd().IsEmpty()))
            return BuildError(Request, EMCPErrorCode::InvalidParams, FString::Printf(TEXT("%s is required."), ValueField));
    if (Operation == EOperation::SetEnumDisplayName
        && (!Request.Params->TryGetStringField(TEXT("entryName"), EntryName) || EntryName.IsEmpty()))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("entryName is required."));
    const bool bDry = Edit::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = Edit::GetOptionalBool(Request.Params, TEXT("save"), false);
    if (!bInspect && !bDry && !Edit::GetOptionalBool(Request.Params, TEXT("confirm"), false))
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Apply requires confirm=true."));
    auto Result = BuildBooleanResult(true);
    FString Error;
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UUserDefinedStruct* Struct = bEnum ? nullptr : LoadObject<UUserDefinedStruct>(nullptr, *Path);
        UUserDefinedEnum* Enum = bEnum ? LoadObject<UUserDefinedEnum>(nullptr, *Path) : nullptr;
        UObject* Asset = bEnum ? static_cast<UObject*>(Enum) : Struct;
        if (!Asset) { OutError = TEXT("Path does not resolve to the requested user-defined asset type."); return false; }
        Result->SetStringField(TEXT("objectPath"), Asset->GetPathName());
        auto Describe = [&]()
        {
            TArray<TSharedPtr<FJsonValue>> Members;
            if (Enum)
            {
                for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
                {
                    auto Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("entryName"), Enum->GetNameByIndex(Index).ToString());
                    Entry->SetStringField(TEXT("displayName"), Enum->GetDisplayNameTextByIndex(Index).ToString());
                    Entry->SetNumberField(TEXT("value"), Enum->GetValueByIndex(Index));
                    Members.Add(MakeShared<FJsonValueObject>(Entry));
                }
                Result->SetArrayField(TEXT("entries"), Members);
                Result->SetBoolField(TEXT("isDirty"), Asset->GetPackage()->IsDirty());
                return;
            }
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
        Registry.GetReferencers(Asset->GetPackage()->GetFName(), Referencers);
        Referencers.Remove(Asset->GetPackage()->GetFName());
        Result->SetArrayField(TEXT("referencerPackages"), NamesJson(Referencers));
        Result->SetBoolField(TEXT("dryRun"), bDry);
        FGuid Guid;
        FEdGraphPinType Type;
        int32 EnumIndex = INDEX_NONE;
        bool bNoChange = false;
        if (Enum)
        {
            const int32 EntryCount = Enum->NumEnums() - 1;
            if (Operation == EOperation::AddEnum)
            {
                EnumIndex = EntryCount;
                for (int32 Index = 0; Index < EntryCount; ++Index)
                    if (Enum->GetValueByIndex(Index) != Index)
                    { OutError = TEXT("Appending would renumber existing values."); return false; }
                if (EntryCount >= 255) { OutError = TEXT("Enum has no remaining byte values."); return false; }
            }
            else
            {
                EnumIndex = Enum->GetIndexByName(FName(*EntryName));
                if (EnumIndex < 0 || EnumIndex >= EntryCount) { OutError = TEXT("Enum entry not found."); return false; }
                bNoChange = Enum->GetDisplayNameTextByIndex(EnumIndex).ToString() == NewValue;
            }
            if (!FEnumEditorUtils::IsEnumeratorDisplayNameValid(Enum, EnumIndex, FText::FromString(NewValue)))
            { OutError = TEXT("Invalid or duplicate enum display name."); return false; }
        }
        else if (bAdd)
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
            if (Operation == EOperation::Remove && FStructureEditorUtils::GetVarDesc(Struct).Num() <= 1)
            { OutError = TEXT("Cannot remove the last member of a user-defined struct."); return false; }
            if (Operation == EOperation::Rename)
            {
                bNoChange = Member->FriendlyName == NewValue;
                if (!bNoChange && !FStructureEditorUtils::IsUniqueVariableFriendlyName(Struct, NewValue))
                { OutError = TEXT("New member name is invalid or already exists."); return false; }
            }
            if (Operation == EOperation::SetDefault)
            {
                bNoChange = Member->DefaultValue == NewValue;
                FStructOnScope Scratch(Struct);
                const FProperty* Property = FindFProperty<FProperty>(Struct, Member->VarName);
                if (!NewValue.IsEmpty() && (!Property || !Scratch.IsValid()
                    || !FBlueprintEditorUtils::PropertyValueFromString(Property, NewValue, Scratch.GetStructMemory(), Struct)))
                { OutError = TEXT("Default value does not match the member type."); return false; }
            }
        }
        Describe();
        TMap<UBlueprint*, TArray<TSharedPtr<FJsonValue>>> Dependents;
        const bool bCompileDependents = Edit::GetOptionalBool(Request.Params, TEXT("compileDependents"), false);
        for (TObjectIterator<UBlueprint> It; It; ++It)
        {
            if (!It->IsAsset()) continue;
            auto Pins = FindAffectedPins(*It, Asset);
            const bool bMember = It->NewVariables.ContainsByPredicate([Asset](const FBPVariableDescription& V)
            {
                return V.VarType.PinSubCategoryObject.Get() == Asset || V.VarType.PinValueType.TerminalSubCategoryObject.Get() == Asset;
            });
            if (!Pins.IsEmpty() || bMember) Referencers.AddUnique(It->GetPackage()->GetFName());
            if (Referencers.Contains(It->GetPackage()->GetFName())) Dependents.Add(*It, MoveTemp(Pins));
        }
        Result->SetArrayField(TEXT("referencerPackages"), NamesJson(Referencers));
        if (bCompileDependents || bDry)
            for (FName Package : Referencers)
            {
                TArray<FAssetData> Assets;
                Registry.GetAssetsByPackageName(Package, Assets);
                for (const FAssetData& Data : Assets)
                    if (UBlueprint* Blueprint = Cast<UBlueprint>(Data.GetAsset()))
                        if (!Dependents.Contains(Blueprint)) Dependents.Add(Blueprint, FindAffectedPins(Blueprint, Asset));
            }
        auto ReportDependents = [&](bool bCompile)
        {
            TArray<TSharedPtr<FJsonValue>> Items;
            bool bAllPassed = true;
            for (const auto& Pair : Dependents)
            {
                UBlueprint* Blueprint = Pair.Key;
                auto Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("objectPath"), Blueprint->GetPathName());
                Item->SetArrayField(TEXT("affectedPinsBefore"), Pair.Value);
                if (bCompile)
                {
                    FCompilerResultsLog Log;
                    Log.bSilentMode = true;
                    FKismetEditorUtilities::CompileBlueprint(Blueprint,
                        EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Log);
                    TArray<TSharedPtr<FJsonValue>> Diagnostics;
                    for (const auto& Message : Log.Messages)
                        Diagnostics.Add(MakeShared<FJsonValueObject>(Edit::BuildLocatedCompileMessage(*Message)));
                    Item->SetArrayField(TEXT("diagnostics"), Diagnostics);
                    Item->SetArrayField(TEXT("affectedPinsAfter"), FindAffectedPins(Blueprint, Asset));
                    Item->SetNumberField(TEXT("errorCount"), Log.NumErrors);
                    bAllPassed &= Log.NumErrors == 0 && Blueprint->Status != BS_Error;
                }
                Items.Add(MakeShared<FJsonValueObject>(Item));
            }
            Result->SetArrayField(TEXT("dependentBlueprints"), Items);
            return bAllPassed;
        };
        ReportDependents(false);
        Result->SetBoolField(TEXT("changed"), false);
        if (bNoChange) return true;
        if (bDry) return true;
        if (!Referencers.IsEmpty() && !Edit::GetOptionalBool(Request.Params, TEXT("allowReferenced"), false))
        { OutError = TEXT("Asset has referencers. Review dryRun and pass allowReferenced=true. Struct removal can discard stored values."); return false; }
        if (!GEditor || GEditor->IsTransactionActive()) { OutError = TEXT("An idle editor transaction system is required."); return false; }
        const auto Before = DirtyNames();
        const bool bDirtyBefore = Asset->GetPackage()->IsDirty();
        bool bChanged = false;
        bool bTouched = false;
        {
            FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "EditUserSchema", "UnrealMCP Edit User-defined Schema"));
            // StructureEditorUtils transacts editor data; transacting compiled fields makes undo unsafe.
            if (Enum)
            {
                if (Operation == EOperation::AddEnum)
                {
                    FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
                    bTouched = Enum->NumEnums() - 1 > EnumIndex;
                }
                if (Operation != EOperation::AddEnum || bTouched)
                    bChanged = FEnumEditorUtils::SetEnumeratorDisplayName(Enum, EnumIndex, FText::FromString(NewValue));
            }
            else if (bAdd)
            {
                bChanged = FStructureEditorUtils::AddVariable(Struct, Type);
                bTouched = bChanged;
                if (bChanged)
                {
                    Guid = FStructureEditorUtils::GetVarDesc(Struct).Last().VarGuid;
                    bChanged = FStructureEditorUtils::RenameVariable(Struct, Guid, MemberName);
                }
            }
            else if (Operation == EOperation::Rename) bChanged = FStructureEditorUtils::RenameVariable(Struct, Guid, NewValue);
            else if (Operation == EOperation::SetDefault) bChanged = FStructureEditorUtils::ChangeVariableDefaultValue(Struct, Guid, NewValue);
            else bChanged = FStructureEditorUtils::RemoveVariable(Struct, Guid);
            bTouched |= bChanged;
            if (!bTouched) Transaction.Cancel();
        }
        const bool bCompiled = !Struct || Struct->Status == UDSS_UpToDate;
        Result->SetBoolField(TEXT("compiled"), bCompiled);
        if (!bChanged || !bCompiled)
        {
            const bool bRolledBack = bTouched && GEditor->UndoTransaction();
            Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
            if (bRolledBack)
            {
                if (Struct) FStructureEditorUtils::CompileStructure(Struct);
                Asset->GetPackage()->SetDirtyFlag(bDirtyBefore);
            }
            OutError = TEXT("The engine rejected the schema edit or compilation failed. No packages were saved.");
        }
        else
        {
            Result->SetBoolField(TEXT("changed"), true);
            if (Enum) Result->SetStringField(TEXT("entryName"), Enum->GetNameByIndex(EnumIndex).ToString());
            else Result->SetStringField(TEXT("memberGuid"), Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
            Result->SetBoolField(TEXT("saved"), false);
            if (bCompileDependents && !ReportDependents(true))
                OutError = TEXT("Dependent Blueprint compilation failed. The schema edit is in memory and no packages were saved.");
            if (bSave && OutError.IsEmpty())
            {
                FString Filename, IndexError;
                if (Edit::SaveAsset(Asset, Filename, OutError))
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
    const bool bEnum = IsEnum(Operation);
    P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("User-defined struct or enum asset path for this tool.")));
    TArray<TSharedPtr<FJsonValue>> Required = {MakeShared<FJsonValueString>(TEXT("objectPath"))};
    if (Operation != EOperation::Inspect && Operation != EOperation::InspectEnum)
    {
        auto RequireString = [&](const TCHAR* Field, const TCHAR* FieldDescription)
        {
            P->SetObjectField(Field, BuildStringProperty(FieldDescription));
            Required.Add(MakeShared<FJsonValueString>(Field));
        };
        if (bEnum)
        {
            RequireString(TEXT("displayName"), TEXT("Unique authored display name."));
            if (Operation == EOperation::SetEnumDisplayName)
                RequireString(TEXT("entryName"), TEXT("Exact internal name from InspectUserDefinedEnum."));
        }
        else
        {
            P->SetObjectField(TEXT("memberName"), BuildStringProperty(TEXT("Authored member name or existing internal name.")));
            if (Operation != EOperation::Add)
                P->SetObjectField(TEXT("memberGuid"), BuildStringProperty(TEXT("Stable GUID from InspectUserDefinedStruct.")));
            if (Operation == EOperation::Rename) RequireString(TEXT("newName"), TEXT("New authored member name."));
            if (Operation == EOperation::SetDefault) RequireString(TEXT("defaultValue"), TEXT("Unreal property export text; empty resets the default."));
        }
        if (Operation == EOperation::Add)
        {
            Required.Add(MakeShared<FJsonValueString>(TEXT("memberName")));
            Required.Add(MakeShared<FJsonValueString>(TEXT("type")));
            for (const TCHAR* Key : {TEXT("type"), TEXT("typeObjectPath"), TEXT("containerType"), TEXT("valueType"), TEXT("valueTypeObjectPath")})
                P->SetObjectField(Key, BuildStringProperty(TEXT("Type argument as in AddBlueprintVariable; containerType is none, array, set or map.")));
        }
        P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate and report only.")));
        P->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required to apply.")));
        P->SetObjectField(TEXT("allowReferenced"), BuildBoolProperty(TEXT("Allow dependent assets to be reconstructed/dirtied, and potential loss of removed-member data. Default false.")));
        P->SetObjectField(TEXT("compileDependents"), BuildBoolProperty(TEXT("Compile discovered dependent Blueprints and return pin and compiler evidence. No dependent packages are saved.")));
        P->SetObjectField(TEXT("save"), BuildBoolProperty(TEXT("Save only the edited schema asset. Default false.")));
    }
    S->SetObjectField(TEXT("properties"), P); S->SetArrayField(TEXT("required"), Required); return S;
}
