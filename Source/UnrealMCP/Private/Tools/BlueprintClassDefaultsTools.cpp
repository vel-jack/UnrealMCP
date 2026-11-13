#include "Tools/BlueprintClassDefaultsTools.h"

#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/UnrealType.h"

namespace
{
    namespace Edit = UnrealMCP::BlueprintEditToolUtils;

    FString ExportValue(const FProperty* Property, UObject* Object)
    {
        FString Value;
        const void* Address = Property->ContainerPtrToValuePtr<void>(Object);
        Property->ExportText_Direct(Value, Address, Address, Object, PPF_ExternalEditor);
        return Value;
    }

    FString PublicName(const FProperty* Property)
    {
        return Property->GetAuthoredName();
    }

    bool IsEditable(const FProperty* Property)
    {
        return Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)
            && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated);
    }

    FProperty* FindProperty(UClass* Class, const FString& Name)
    {
        for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            if (It->GetName().Equals(Name, ESearchCase::IgnoreCase)
                || PublicName(*It).Equals(Name, ESearchCase::IgnoreCase)) return *It;
        }
        return nullptr;
    }

    TSharedRef<FJsonObject> SerializeProperty(FProperty* Property, UClass* Class, UObject* CDO)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), PublicName(Property));
        Item->SetStringField(TEXT("declaringClass"), Property->GetOwnerClass()->GetPathName());
        FEdGraphPinType PinType;
        if (GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, PinType))
            Item->SetObjectField(TEXT("type"), UnrealMCP::BlueprintToolUtils::SerializePinType(PinType));
        else Item->SetStringField(TEXT("type"), Property->GetCPPType());
        Item->SetStringField(TEXT("value"), ExportValue(Property, CDO));
        FProperty* ParentProperty = Class->GetSuperClass() ? FindFProperty<FProperty>(Class->GetSuperClass(), Property->GetFName()) : nullptr;
        const FString ParentValue = ParentProperty ? ExportValue(ParentProperty, Class->GetSuperClass()->GetDefaultObject()) : FString();
        Item->SetStringField(TEXT("parentValue"), ParentValue);
        Item->SetBoolField(TEXT("isOverridden"), ParentProperty && Item->GetStringField(TEXT("value")) != ParentValue);
        Item->SetBoolField(TEXT("editable"), IsEditable(Property));
        return Item;
    }

    TSharedRef<FJsonObject> Schema(bool bSet)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("type"), TEXT("object"));
        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("objectPath"), Edit::BuildStringProperty(TEXT("Exact Blueprint object path.")));
        if (bSet)
        {
            TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
            Values->SetStringField(TEXT("type"), TEXT("object"));
            Values->SetStringField(TEXT("description"), TEXT("Property name to Unreal export-text value."));
            TSharedRef<FJsonObject> StringType = MakeShared<FJsonObject>();
            StringType->SetStringField(TEXT("type"), TEXT("string"));
            Values->SetObjectField(TEXT("additionalProperties"), StringType);
            Properties->SetObjectField(TEXT("values"), Values);
            Properties->SetObjectField(TEXT("dryRun"), Edit::BuildBoolProperty(TEXT("Preflight without mutation.")));
            Properties->SetObjectField(TEXT("confirm"), Edit::BuildBoolProperty(TEXT("Required for a real edit.")));
            Properties->SetObjectField(TEXT("compile"), Edit::BuildBoolProperty(TEXT("Compile after editing; default true.")));
            Properties->SetObjectField(TEXT("save"), Edit::BuildBoolProperty(TEXT("Save this Blueprint package; default false.")));
            Properties->SetObjectField(TEXT("allowDirty"), Edit::BuildBoolProperty(TEXT("Allow a pre-existing dirty Blueprint; default false.")));
            Result->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("values"))});
        }
        else
        {
            TSharedRef<FJsonObject> Names = MakeShared<FJsonObject>();
            Names->SetStringField(TEXT("type"), TEXT("array"));
            TSharedRef<FJsonObject> StringType = MakeShared<FJsonObject>();
            StringType->SetStringField(TEXT("type"), TEXT("string"));
            Names->SetObjectField(TEXT("items"), StringType);
            Properties->SetObjectField(TEXT("propertyNames"), Names);
            Properties->SetObjectField(TEXT("includeInherited"), Edit::BuildBoolProperty(TEXT("Include parent properties; default true.")));
            Properties->SetObjectField(TEXT("onlyOverridden"), Edit::BuildBoolProperty(TEXT("Return values different from the parent CDO.")));
            TSharedRef<FJsonObject> Integer = MakeShared<FJsonObject>();
            Integer->SetStringField(TEXT("type"), TEXT("integer"));
            Properties->SetObjectField(TEXT("offset"), Integer);
            Properties->SetObjectField(TEXT("limit"), Integer);
            Result->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath"))});
        }
        Result->SetObjectField(TEXT("properties"), Properties);
        return Result;
    }

    struct FEdit
    {
        FProperty* Property = nullptr;
        FString Name;
        FString Requested;
        FString Previous;
        FString NewValue;
        int32 VariableIndex = INDEX_NONE;
    };
}

FGetBlueprintClassDefaultsTool::FGetBlueprintClassDefaultsTool()
    : FMCPToolBase(TEXT("GetBlueprintClassDefaults"), TEXT("Reads paged Blueprint class default values and parent overrides without modifying the asset.")) {}

UnrealMCP::FMCPResponse FGetBlueprintClassDefaultsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("GetBlueprintClassDefaults requires objectPath."));
    const bool bInherited = Edit::GetOptionalBool(Request.Params, TEXT("includeInherited"), true);
    const bool bOnlyOverridden = Edit::GetOptionalBool(Request.Params, TEXT("onlyOverridden"), false);
    double OffsetNumber = 0, LimitNumber = 50;
    Request.Params->TryGetNumberField(TEXT("offset"), OffsetNumber);
    Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber);
    if (OffsetNumber < 0 || LimitNumber < 1 || LimitNumber > 100 || FMath::FloorToDouble(OffsetNumber) != OffsetNumber || FMath::FloorToDouble(LimitNumber) != LimitNumber)
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("offset must be nonnegative and limit must be 1..100."));
    TArray<FString> Requested;
    const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
    if (Request.Params->TryGetArrayField(TEXT("propertyNames"), Names))
    {
        for (const TSharedPtr<FJsonValue>& NameValue : *Names)
        {
            if (!NameValue.IsValid() || NameValue->Type != EJson::String)
                return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("propertyNames must contain strings."));
            Requested.Add(NameValue->AsString());
        }
    }
    TArray<TSharedPtr<FJsonValue>> Items;
    int32 Total = 0;
    FString Error;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!Edit::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        UClass* Class = Blueprint->GeneratedClass;
        if (!Class) { OutError = TEXT("Blueprint has no generated class; compile it separately."); return false; }
        UObject* CDO = Class->GetDefaultObject(false);
        if (!CDO) { OutError = TEXT("Blueprint has no class default object."); return false; }
        for (TFieldIterator<FProperty> It(Class, bInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Requested.IsEmpty() && !Requested.ContainsByPredicate([&](const FString& RequestedName)
                { return Property->GetName().Equals(RequestedName, ESearchCase::IgnoreCase) || PublicName(Property).Equals(RequestedName, ESearchCase::IgnoreCase); })) continue;
            TSharedRef<FJsonObject> Item = SerializeProperty(Property, Class, CDO);
            if (bOnlyOverridden && !Item->GetBoolField(TEXT("isOverridden"))) continue;
            if (Total >= static_cast<int32>(OffsetNumber) && Items.Num() < static_cast<int32>(LimitNumber))
                Items.Add(MakeShared<FJsonValueObject>(Item));
            ++Total;
        }
        for (const FString& RequestedName : Requested)
            if (!FindProperty(Class, RequestedName)) { OutError = FString::Printf(TEXT("Property '%s' was not found."), *RequestedName); return false; }
        return true;
    }, Error);
    if (!bOk) return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, Error);
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetArrayField(TEXT("properties"), Items);
    Result->SetNumberField(TEXT("count"), Items.Num());
    Result->SetNumberField(TEXT("total"), Total);
    Result->SetNumberField(TEXT("offset"), OffsetNumber);
    Result->SetBoolField(TEXT("hasMore"), OffsetNumber + Items.Num() < Total);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetBlueprintClassDefaultsTool::BuildInputSchema() const { return Schema(false); }

FSetBlueprintClassDefaultsTool::FSetBlueprintClassDefaultsTool()
    : FMCPToolBase(TEXT("SetBlueprintClassDefaults"), TEXT("Preflights and transactionally sets Blueprint class defaults, including inherited properties.")) {}

UnrealMCP::FMCPResponse FSetBlueprintClassDefaultsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    const TSharedPtr<FJsonObject>* Values = nullptr;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty()
        || !Request.Params->TryGetObjectField(TEXT("values"), Values) || !Values || !Values->IsValid() || (*Values)->Values.IsEmpty())
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("SetBlueprintClassDefaults requires objectPath and a nonempty values object."));
    const bool bDryRun = Edit::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompile = Edit::GetOptionalBool(Request.Params, TEXT("compile"), true);
    const bool bSave = Edit::GetOptionalBool(Request.Params, TEXT("save"), false);
    const bool bAllowDirty = Edit::GetOptionalBool(Request.Params, TEXT("allowDirty"), false);
    if (!bDryRun && !Edit::GetOptionalBool(Request.Params, TEXT("confirm"), false))
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("SetBlueprintClassDefaults requires confirm=true unless dryRun=true."));
    if (bSave && !bCompile)
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("Saving class defaults requires compile=true."));

    TArray<FEdit> Edits;
    bool bChanged = false, bCompiled = false, bSaved = false;
    FString SavedFilename, Error;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!Edit::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        if (Blueprint->GetPackage()->IsDirty() && !bAllowDirty)
        {
            OutError = TEXT("Blueprint package has unsaved changes; pass allowDirty=true to include them in this edit.");
            return false;
        }
        UClass* Class = Blueprint->GeneratedClass;
        if (!Class) { OutError = TEXT("Blueprint has no generated class; compile it separately."); return false; }
        UObject* CDO = Class->GetDefaultObject(false);
        if (!CDO) { OutError = TEXT("Blueprint has no class default object."); return false; }
        UObject* Preview = DuplicateObject<UObject>(CDO, GetTransientPackage());
        if (!Preview) { OutError = TEXT("Could not create a transient class-default preview."); return false; }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
        {
            FProperty* Property = FindProperty(Class, Pair.Key);
            if (!Property || !IsEditable(Property))
            {
                OutError = FString::Printf(TEXT("Property '%s' was not found or is not editable."), *Pair.Key);
                return false;
            }
            if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
            {
                OutError = FString::Printf(TEXT("Value for '%s' must be an Unreal export-text string."), *Pair.Key);
                return false;
            }
            FEdit& EditItem = Edits.AddDefaulted_GetRef();
            EditItem.Property = Property;
            EditItem.Name = PublicName(Property);
            EditItem.Requested = Pair.Value->AsString();
            EditItem.Previous = ExportValue(Property, CDO);
            void* Address = Property->ContainerPtrToValuePtr<void>(Preview);
            const TCHAR* End = Property->ImportText_Direct(*EditItem.Requested, Address, Preview, PPF_None);
            if (!End || !FString(End).TrimStartAndEnd().IsEmpty())
            {
                OutError = FString::Printf(TEXT("Invalid export text for '%s' (%s): '%s'."), *Pair.Key, *Property->GetCPPType(), *EditItem.Requested);
                return false;
            }
            EditItem.NewValue = ExportValue(Property, Preview);
            if (Property->GetOwnerClass() == Class)
            {
                EditItem.VariableIndex = Blueprint->NewVariables.IndexOfByPredicate([&](const FBPVariableDescription& Variable)
                    { return Variable.VarName == Property->GetFName(); });
            }
            bChanged |= EditItem.Previous != EditItem.NewValue;
        }
        if (bDryRun || !bChanged) return true;

        const bool bWasDirty = Blueprint->GetPackage()->IsDirty();
        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "SetBlueprintClassDefaults", "UnrealMCP Set Blueprint Class Defaults"));
            Blueprint->Modify();
            CDO->Modify();
            for (const FEdit& EditItem : Edits)
            {
                void* Target = EditItem.Property->ContainerPtrToValuePtr<void>(CDO);
                EditItem.Property->CopyCompleteValue(Target, EditItem.Property->ContainerPtrToValuePtr<void>(Preview));
                if (EditItem.VariableIndex != INDEX_NONE)
                    Blueprint->NewVariables[EditItem.VariableIndex].DefaultValue = EditItem.NewValue;
            }
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        }
        if (bCompile)
        {
            FCompilerResultsLog Log;
            Log.bSilentMode = true;
            FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Log);
            bCompiled = Log.NumErrors == 0 && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
        }
        if (bCompiled)
        {
            UObject* RecompiledCDO = Blueprint->GeneratedClass->GetDefaultObject(false);
            for (const FEdit& EditItem : Edits)
            {
                FProperty* RecompiledProperty = FindProperty(Blueprint->GeneratedClass, EditItem.Name);
                if (!RecompiledProperty || !RecompiledCDO)
                {
                    bCompiled = false;
                    break;
                }
                void* Expected = RecompiledProperty->AllocateAndInitializeValue();
                const TCHAR* End = RecompiledProperty->ImportText_Direct(*EditItem.Requested, Expected, RecompiledCDO, PPF_None);
                const bool bMatches = End && FString(End).TrimStartAndEnd().IsEmpty()
                    && RecompiledProperty->Identical(Expected, RecompiledProperty->ContainerPtrToValuePtr<void>(RecompiledCDO), PPF_None);
                RecompiledProperty->DestroyAndFreeValue(Expected);
                if (!bMatches) { bCompiled = false; break; }
            }
        }
        if (bCompile && !bCompiled)
        {
            if (GEditor && GEditor->UndoTransaction())
            {
                FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
                Blueprint->GetPackage()->SetDirtyFlag(bWasDirty);
                OutError = TEXT("Compilation or default readback failed; the edit was rolled back.");
            }
            else OutError = TEXT("Compilation or default readback failed and transaction rollback failed; inspect the Blueprint before saving.");
            return false;
        }
        if (bSave && !Edit::SaveAsset(Blueprint, SavedFilename, OutError))
        {
            if (GEditor && GEditor->UndoTransaction())
            {
                FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
                Blueprint->GetPackage()->SetDirtyFlag(bWasDirty);
                OutError += TEXT(" The edit was rolled back.");
            }
            else OutError += TEXT(" Transaction rollback failed; inspect the Blueprint before retrying.");
            return false;
        }
        bSaved = bSave;
        return true;
    }, Error);
    if (!bOk) return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, Error);

    TArray<TSharedPtr<FJsonValue>> Results;
    for (const FEdit& EditItem : Edits)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), EditItem.Name);
        Item->SetStringField(TEXT("previousValue"), EditItem.Previous);
        Item->SetStringField(TEXT("value"), EditItem.NewValue);
        Item->SetBoolField(TEXT("changed"), EditItem.Previous != EditItem.NewValue);
        Results.Add(MakeShared<FJsonValueObject>(Item));
    }
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetArrayField(TEXT("properties"), Results);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("changed"), bChanged && !bDryRun);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FSetBlueprintClassDefaultsTool::BuildInputSchema() const { return Schema(true); }

FResetBlueprintClassDefaultsTool::FResetBlueprintClassDefaultsTool()
    : FMCPToolBase(TEXT("ResetBlueprintClassDefaults"), TEXT("Resets inherited Blueprint class defaults to their parent CDO values.")) {}

UnrealMCP::FMCPResponse FResetBlueprintClassDefaultsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty()
        || !Request.Params->TryGetArrayField(TEXT("propertyNames"), Names) || !Names || Names->IsEmpty())
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("ResetBlueprintClassDefaults requires objectPath and nonempty propertyNames."));
    TSharedRef<FJsonObject> ParentValues = MakeShared<FJsonObject>();
    FString Error;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!Edit::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        UClass* Class = Blueprint->GeneratedClass;
        UClass* Parent = Class ? Class->GetSuperClass() : nullptr;
        if (!Parent) { OutError = TEXT("Blueprint has no generated parent class."); return false; }
        for (const TSharedPtr<FJsonValue>& NameValue : *Names)
        {
            if (!NameValue.IsValid() || NameValue->Type != EJson::String)
            {
                OutError = TEXT("propertyNames must contain strings.");
                return false;
            }
            FProperty* ChildProperty = FindProperty(Class, NameValue->AsString());
            FProperty* ParentProperty = ChildProperty ? FindFProperty<FProperty>(Parent, ChildProperty->GetFName()) : nullptr;
            if (!ChildProperty || !ParentProperty || !IsEditable(ChildProperty))
            {
                OutError = FString::Printf(TEXT("Property '%s' is not an editable inherited property."), *NameValue->AsString());
                return false;
            }
            ParentValues->SetStringField(PublicName(ChildProperty), ExportValue(ParentProperty, Parent->GetDefaultObject()));
        }
        return true;
    }, Error);
    if (!bOk) return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, Error);

    TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("objectPath"), ObjectPath);
    Params->SetObjectField(TEXT("values"), ParentValues);
    for (const TCHAR* Flag : {TEXT("dryRun"), TEXT("confirm"), TEXT("compile"), TEXT("save"), TEXT("allowDirty")})
    {
        bool Value = false;
        if (Request.Params->TryGetBoolField(Flag, Value)) Params->SetBoolField(Flag, Value);
    }
    UnrealMCP::FMCPRequest SetRequest = Request;
    SetRequest.Params = Params;
    return FSetBlueprintClassDefaultsTool().Execute(SetRequest);
}

TSharedPtr<FJsonObject> FResetBlueprintClassDefaultsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Result = Schema(false);
    TSharedPtr<FJsonObject> Properties = Result->GetObjectField(TEXT("properties"));
    Properties->SetObjectField(TEXT("dryRun"), Edit::BuildBoolProperty(TEXT("Preflight without mutation.")));
    Properties->SetObjectField(TEXT("confirm"), Edit::BuildBoolProperty(TEXT("Required for a real edit.")));
    Properties->SetObjectField(TEXT("compile"), Edit::BuildBoolProperty(TEXT("Compile after reset; default true.")));
    Properties->SetObjectField(TEXT("save"), Edit::BuildBoolProperty(TEXT("Save this Blueprint package; default false.")));
    Properties->SetObjectField(TEXT("allowDirty"), Edit::BuildBoolProperty(TEXT("Allow a pre-existing dirty Blueprint.")));
    Result->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("propertyNames"))});
    return Result;
}

FCompareBlueprintClassDefaultsTool::FCompareBlueprintClassDefaultsTool()
    : FMCPToolBase(TEXT("CompareBlueprintClassDefaults"), TEXT("Compares two Blueprint CDOs by property name, including properties present on one side only.")) {}

UnrealMCP::FMCPResponse FCompareBlueprintClassDefaultsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString PathA, PathB;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPathA"), PathA) || PathA.IsEmpty()
        || !Request.Params->TryGetStringField(TEXT("objectPathB"), PathB) || PathB.IsEmpty())
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("CompareBlueprintClassDefaults requires objectPathA and objectPathB."));
    const bool bOnlyDifferent = Edit::GetOptionalBool(Request.Params, TEXT("onlyDifferent"), true);
    double OffsetNumber = 0, LimitNumber = 100;
    Request.Params->TryGetNumberField(TEXT("offset"), OffsetNumber);
    Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber);
    if (OffsetNumber < 0 || LimitNumber < 1 || LimitNumber > 200 || FMath::FloorToDouble(OffsetNumber) != OffsetNumber || FMath::FloorToDouble(LimitNumber) != LimitNumber)
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("offset must be nonnegative and limit must be 1..200."));
    TArray<TSharedPtr<FJsonValue>> Items;
    int32 Total = 0;
    FString Error;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint *BlueprintA = nullptr, *BlueprintB = nullptr;
        if (!Edit::ResolveBlueprint(PathA, BlueprintA, OutError) || !Edit::ResolveBlueprint(PathB, BlueprintB, OutError)) return false;
        UClass *ClassA = BlueprintA->GeneratedClass, *ClassB = BlueprintB->GeneratedClass;
        if (!ClassA || !ClassB) { OutError = TEXT("Both Blueprints need generated classes; compile separately."); return false; }
        UObject *CDOA = ClassA->GetDefaultObject(false), *CDOB = ClassB->GetDefaultObject(false);
        if (!CDOA || !CDOB) { OutError = TEXT("Both Blueprints need class default objects."); return false; }
        TMap<FString, FProperty*> PropertiesA, PropertiesB;
        for (TFieldIterator<FProperty> It(ClassA, EFieldIteratorFlags::IncludeSuper); It; ++It)
            if (IsEditable(*It)) PropertiesA.FindOrAdd(PublicName(*It)) = *It;
        for (TFieldIterator<FProperty> It(ClassB, EFieldIteratorFlags::IncludeSuper); It; ++It)
            if (IsEditable(*It)) PropertiesB.FindOrAdd(PublicName(*It)) = *It;
        TArray<FString> Names;
        PropertiesA.GetKeys(Names);
        for (const TPair<FString, FProperty*>& Pair : PropertiesB) Names.AddUnique(Pair.Key);
        Names.Sort();
        for (const FString& PropertyName : Names)
        {
            FProperty* PropertyA = PropertiesA.FindRef(PropertyName);
            FProperty* PropertyB = PropertiesB.FindRef(PropertyName);
            const FString ValueA = PropertyA ? ExportValue(PropertyA, CDOA) : FString();
            const FString ValueB = PropertyB ? ExportValue(PropertyB, CDOB) : FString();
            const bool bDifferent = !PropertyA || !PropertyB || ValueA != ValueB || PropertyA->GetCPPType() != PropertyB->GetCPPType();
            if (bOnlyDifferent && !bDifferent) continue;
            if (Total >= static_cast<int32>(OffsetNumber) && Items.Num() < static_cast<int32>(LimitNumber))
            {
                TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("name"), PropertyName);
                Item->SetStringField(TEXT("valueA"), ValueA);
                Item->SetStringField(TEXT("valueB"), ValueB);
                Item->SetStringField(TEXT("typeA"), PropertyA ? PropertyA->GetCPPType() : FString());
                Item->SetStringField(TEXT("typeB"), PropertyB ? PropertyB->GetCPPType() : FString());
                Item->SetBoolField(TEXT("onlyInA"), PropertyA && !PropertyB);
                Item->SetBoolField(TEXT("onlyInB"), !PropertyA && PropertyB);
                Item->SetBoolField(TEXT("different"), bDifferent);
                Items.Add(MakeShared<FJsonValueObject>(Item));
            }
            ++Total;
        }
        return true;
    }, Error);
    if (!bOk) return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, Error);
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetArrayField(TEXT("properties"), Items);
    Result->SetNumberField(TEXT("count"), Items.Num());
    Result->SetNumberField(TEXT("total"), Total);
    Result->SetNumberField(TEXT("offset"), OffsetNumber);
    Result->SetBoolField(TEXT("hasMore"), OffsetNumber + Items.Num() < Total);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FCompareBlueprintClassDefaultsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPathA"), Edit::BuildStringProperty(TEXT("First Blueprint object path.")));
    Properties->SetObjectField(TEXT("objectPathB"), Edit::BuildStringProperty(TEXT("Second Blueprint object path.")));
    Properties->SetObjectField(TEXT("onlyDifferent"), Edit::BuildBoolProperty(TEXT("Return only differing or missing properties; default true.")));
    TSharedRef<FJsonObject> Integer = MakeShared<FJsonObject>();
    Integer->SetStringField(TEXT("type"), TEXT("integer"));
    Properties->SetObjectField(TEXT("offset"), Integer);
    Properties->SetObjectField(TEXT("limit"), Integer);
    Result->SetObjectField(TEXT("properties"), Properties);
    Result->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPathA")), MakeShared<FJsonValueString>(TEXT("objectPathB"))});
    return Result;
}
