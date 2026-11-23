#include "Tools/Blueprint/Members/SetBlueprintComponentDefaultsTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/Blueprint/Shared/BlueprintComponentEditUtils.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"

FSetBlueprintComponentDefaultsTool::FSetBlueprintComponentDefaultsTool()
    : FMCPToolBase(TEXT("SetBlueprintComponentDefaults"), TEXT("Transactionally validates and updates safely editable defaults on one existing Blueprint component template."))
{
}

UnrealMCP::FMCPResponse FSetBlueprintComponentDefaultsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, ComponentName, OverrideGuidText;
    FGuid OverrideGuid;
    const TSharedPtr<FJsonObject>* Defaults = nullptr;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("componentName"), ComponentName)
        || !Request.Params->TryGetObjectField(TEXT("propertyDefaults"), Defaults) || !Defaults->IsValid() || (*Defaults)->Values.IsEmpty())
    { return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("SetBlueprintComponentDefaults requires objectPath, componentName, and non-empty propertyDefaults.")); }
    if (Request.Params->HasField(TEXT("inheritedOverrideGuid"))
        && (!Request.Params->TryGetStringField(TEXT("inheritedOverrideGuid"), OverrideGuidText)
            || !FGuid::Parse(OverrideGuidText, OverrideGuid) || !OverrideGuid.IsValid()))
    { return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("inheritedOverrideGuid must be a valid component GUID.")); }
    bool bCreateOverride = false;
    if ((Request.Params->HasField(TEXT("createInheritedOverride"))
            && !Request.Params->TryGetBoolField(TEXT("createInheritedOverride"), bCreateOverride))
        || (bCreateOverride && !OverrideGuid.IsValid()))
    { return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("Creating an inherited override requires createInheritedOverride=true and inheritedOverrideGuid.")); }
    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), true);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), true);
    bool bChanged = false, bWouldChange = false, bCompileSucceeded = false, bIndexRefreshed = false;
    bool bWouldCreateOverride = false, bCreatedOverride = false;
    int32 CompileErrors = 0, CompileWarnings = 0;
    FString ComponentClassPath, SavedFilename, IndexError, Error;
    TArray<BlueprintComponentEditUtils::FPropertyValue> Values;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        USCS_Node* Node = nullptr;
        bool bFoundOverrideRecord = false;
        auto ResolveTemplate = [&]() -> UObject*
        {
            if (!OverrideGuid.IsValid())
            {
                Node = BlueprintComponentEditUtils::FindComponentNode(Blueprint, ComponentName);
                return Node ? Node->ComponentTemplate.Get() : nullptr;
            }
            UActorComponent* Match = nullptr;
            if (UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(false))
                for (auto It = Handler->CreateRecordIterator(); It; ++It)
                {
                    const FComponentKey& Key = It->ComponentKey;
                    if (Key.GetAssociatedGuid() != OverrideGuid) continue;
                    bFoundOverrideRecord = true;
                    if (Key.GetSCSVariableName() != FName(*ComponentName)) return nullptr;
                    UActorComponent* Candidate = It->ComponentTemplate;
                    const UClass* Owner = Key.GetComponentOwner();
                    if (Match || !Candidate || Candidate->GetOutermost() != Blueprint->GetOutermost()
                        || !Owner || !Blueprint->ParentClass
                        || !Blueprint->ParentClass->IsChildOf(Owner->GetAuthoritativeClass()))
                        return nullptr;
                    Match = Candidate;
                }
            return Match;
        };
        UObject* Template = ResolveTemplate();
        FComponentKey NewKey;
        UBlueprintGeneratedClass* ChildClass = nullptr;
        if (bCreateOverride)
        {
            if (bFoundOverrideRecord)
            { OutError = TEXT("An override record already exists for this GUID. Preview an edit without the creation flag."); return false; }
            ChildClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
            const USCS_Node* AncestorNode = nullptr;
            for (auto* Parent = Cast<UBlueprintGeneratedClass>(Blueprint->ParentClass); Parent;
                Parent = Cast<UBlueprintGeneratedClass>(Parent->GetSuperClass()))
            {
                if (!Parent->SimpleConstructionScript) continue;
                for (const USCS_Node* Candidate : Parent->SimpleConstructionScript->GetAllNodes())
                {
                    if (!Candidate || Candidate->VariableGuid != OverrideGuid) continue;
                    if (AncestorNode || Candidate->GetVariableName() != FName(*ComponentName))
                    { OutError = TEXT("Inherited component GUID and name do not identify a unique ancestor component."); return false; }
                    AncestorNode = Candidate;
                }
            }
            if (ChildClass && AncestorNode)
            {
                NewKey = FComponentKey(AncestorNode);
                Template = AncestorNode->GetActualComponentTemplate(ChildClass);
            }
        }
        if (!IsValid(Template)) { OutError = TEXT("No matching component template is owned by this Blueprint."); return false; }
        if (Node) ComponentName = Node->GetVariableName().ToString();
        ComponentClassPath = Template->GetClass()->GetPathName();
        // Inherited override templates can carry instancing relationships that cannot be duplicated into the transient package.
        UObject* Preview = NewObject<UObject>(GetTransientPackage(), Template->GetClass(), NAME_None, RF_Transient);
        if (!Preview) { OutError = TEXT("Could not create a component defaults preview."); return false; }
        for (const auto& Pair : (*Defaults)->Values)
            if (const FProperty* Property = FindFProperty<FProperty>(Template->GetClass(), *Pair.Key))
                Property->CopyCompleteValue_InContainer(Preview, Template);
        if (!BlueprintComponentEditUtils::ApplyPropertyDefaults(Preview, *Defaults, Values, OutError)) return false;
        bWouldChange = Values.ContainsByPredicate([](const auto& Value) { return Value.bChanged; });
        bWouldCreateOverride = bCreateOverride && bWouldChange;
        if (bDryRun || !bWouldChange) return true;
        if (!GEditor || GEditor->IsTransactionActive())
        { OutError = TEXT("Component default edits require an idle editor transaction system."); return false; }
        TArray<FProperty*> Properties;
        for (const auto& Value : Values)
        {
            FProperty* Property = FindFProperty<FProperty>(Template->GetClass(), FName(*Value.Name));
            if (!Property) { OutError = TEXT("A validated component property is no longer available."); return false; }
            Properties.Add(Property);
        }
        const bool bWasDirty = Blueprint->GetOutermost()->IsDirty();
        {
            const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "SetBlueprintComponentDefaults", "UnrealMCP Set Blueprint Component Defaults"));
            Blueprint->Modify();
            if (Node) Node->Modify();
            UObject* Target = Template;
            if (bWouldCreateOverride)
            {
                // The handler owns the override records. Undoing a generated class can restore stale compiled fields.
                UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(true);
                Target = nullptr;
                if (Handler)
                {
                    Handler->SetFlags(RF_Transactional);
                    Handler->Modify();
                    Target = Handler->CreateOverridenComponentTemplate(NewKey);
                    if (Target && (Target->GetOutermost() != Blueprint->GetOutermost() || Target->GetClass() != Template->GetClass()))
                        Target = nullptr;
                }
                bCreatedOverride = Target != nullptr;
            }
            if (Target)
            {
                Target->SetFlags(RF_Transactional);
                Target->Modify();
                for (const FProperty* Property : Properties)
                    Property->CopyCompleteValue_InContainer(Target, Preview);
                FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            }
        }
        bChanged = true;
        auto Rollback = [&]()
        {
            if (GEditor->UndoTransaction())
            {
                FCompilerResultsLog RollbackLog;
                RollbackLog.bSilentMode = true;
                FKismetEditorUtilities::CompileBlueprint(Blueprint,
                    EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &RollbackLog);
                Blueprint->GetOutermost()->SetDirtyFlag(bWasDirty);
                OutError += TEXT(" Component defaults transaction rolled back.");
            }
            else OutError += TEXT(" Component defaults rollback failed.");
            return false;
        };
        if (bWouldCreateOverride && !bCreatedOverride)
        { OutError = TEXT("Unreal could not create the child override."); return Rollback(); }
        if (bCompile)
        {
            FCompilerResultsLog CompilerLog; CompilerLog.bSilentMode = true;
            FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &CompilerLog);
            CompileErrors = CompilerLog.NumErrors; CompileWarnings = CompilerLog.NumWarnings;
            bCompileSucceeded = CompileErrors == 0 && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
            if (!bCompileSucceeded) { OutError = TEXT("Blueprint compilation failed."); return Rollback(); }
            UObject* After = ResolveTemplate();
            TArray<FString> Names;
            for (const auto& Value : Values) Names.Add(Value.Name);
            TArray<BlueprintComponentEditUtils::FPropertyValue> Readback;
            if (!After || !BlueprintComponentEditUtils::GetEditablePropertyValues(After, Names, Names.Num(), Readback, OutError)
                || Values.ContainsByPredicate([&](const auto& Value)
                {
                    return !Readback.ContainsByPredicate([&](const auto& Actual)
                    { return Actual.Name.Equals(Value.Name, ESearchCase::IgnoreCase) && Actual.Value == Value.Value; });
                }))
            { OutError = TEXT("Component defaults did not survive compilation."); return Rollback(); }
        }
        if (bSave && !BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, OutError)) return Rollback();
        if (bSave) bIndexRefreshed = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexError);
        return true;
    }, Error);
    if (!bSucceeded) return BuildError(Request, EMCPErrorCode::InternalError, Error);

    TArray<TSharedPtr<FJsonValue>> PropertyResults;
    for (const BlueprintComponentEditUtils::FPropertyValue& Value : Values)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("name"), Value.Name); Item->SetStringField(TEXT("type"), Value.Type);
        Item->SetStringField(TEXT("previousValue"), Value.PreviousValue); Item->SetStringField(TEXT("value"), Value.Value); Item->SetBoolField(TEXT("changed"), Value.bChanged);
        PropertyResults.Add(MakeShared<FJsonValueObject>(Item));
    }
    FMCPResponse Response; Response.Id = Request.Id; TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath); Result->SetStringField(TEXT("componentName"), ComponentName); Result->SetStringField(TEXT("componentClassPath"), ComponentClassPath);
    if (OverrideGuid.IsValid()) Result->SetStringField(TEXT("inheritedOverrideGuid"), OverrideGuid.ToString());
    if (bCreateOverride)
    {
        Result->SetBoolField(TEXT("wouldCreateOverride"), bWouldCreateOverride);
        Result->SetBoolField(TEXT("createdOverride"), bCreatedOverride);
    }
    Result->SetBoolField(TEXT("dryRun"), bDryRun); Result->SetBoolField(TEXT("wouldChange"), bWouldChange); Result->SetBoolField(TEXT("changed"), bChanged); Result->SetArrayField(TEXT("properties"), PropertyResults);
    Result->SetBoolField(TEXT("compiled"), bCompile && bChanged && !bDryRun); Result->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
    Result->SetNumberField(TEXT("compileErrorCount"), CompileErrors); Result->SetNumberField(TEXT("compileWarningCount"), CompileWarnings);
    Result->SetBoolField(TEXT("saved"), bSave && bChanged && !bDryRun); Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed); Result->SetStringField(TEXT("indexRefreshError"), IndexError); Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FSetBlueprintComponentDefaultsTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object")); TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("inheritedOverrideGuid"), BuildStringProperty(TEXT("Optional GUID of an existing inherited SCS override owned by the child Blueprint. componentName must also match.")));
    Properties->SetObjectField(TEXT("createInheritedOverride"), BuildBoolProperty(TEXT("Create a missing child override for inheritedOverrideGuid if values differ from the effective inherited template. Defaults false.")));
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path."))); Properties->SetObjectField(TEXT("componentName"), BuildStringProperty(TEXT("Exact existing SCS component variable name.")));
    TSharedRef<FJsonObject> Defaults = MakeShared<FJsonObject>(); Defaults->SetStringField(TEXT("type"), TEXT("object")); Defaults->SetBoolField(TEXT("additionalProperties"), true); Defaults->SetStringField(TEXT("description"), TEXT("Properties expressed as JSON primitives or Unreal export-text strings.")); Properties->SetObjectField(TEXT("propertyDefaults"), Defaults);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate and return normalized values without mutation."))); Properties->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile after a real change; defaults true."))); Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and refresh the index after a real change; defaults true.")));
    Schema->SetObjectField(TEXT("properties"), Properties); Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath")), MakeShared<FJsonValueString>(TEXT("componentName")), MakeShared<FJsonValueString>(TEXT("propertyDefaults"))}); return Schema;
}
