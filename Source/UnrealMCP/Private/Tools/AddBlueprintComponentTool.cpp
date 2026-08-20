#include "Tools/AddBlueprintComponentTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintComponentEditUtils.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FAddBlueprintComponentTool::FAddBlueprintComponentTool()
    : FMCPToolBase(
        TEXT("AddBlueprintComponent"),
        TEXT("Transactionally adds one component to a Blueprint SCS with optional editable template property defaults. Idempotent by component name."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintComponentTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    using namespace UnrealMCP::BlueprintComponentEditUtils;

    FString ObjectPath;
    FComponentSpec Spec;
    FString ParseError;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || ObjectPath.IsEmpty()
        || !ParseComponentSpec(Request.Params, Spec, ParseError))
    {
        return BuildError(
            Request,
            EMCPErrorCode::InvalidParams,
            ParseError.IsEmpty()
                ? TEXT("AddBlueprintComponent requires objectPath, componentName, and componentClassPath.")
                : ParseError);
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bUpdateExistingDefaults = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("updateExistingDefaults"), false);
    const bool bCompile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), false);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    bool bCompileSucceeded = false;
    bool bIndexRefreshed = false;
    FString SavedFilename;
    FString ExecutionError;
    FString IndexRefreshError;
    TArray<FComponentResult> ComponentResults;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError))
        {
            return false;
        }

        TArray<FComponentSpec> Specs{Spec};
        if (!ResolveAndValidateSpecs(Blueprint, Specs, ComponentResults, OutError))
        {
            return false;
        }
        Spec = Specs[0];
        if (ComponentResults[0].bAlreadyExists && bUpdateExistingDefaults && Spec.PropertyDefaults.IsValid())
        {
            USCS_Node* ExistingNode = FindComponentNode(Blueprint, Spec.ComponentName);
            if (ExistingNode == nullptr || ExistingNode->ComponentTemplate == nullptr)
            {
                OutError = TEXT("The existing component template could not be resolved.");
                return false;
            }
            if (!ValidatePropertyDefaults(ExistingNode->ComponentTemplate, Spec.PropertyDefaults, OutError)) return false;
            if (bDryRun) return true;

            const FScopedTransaction Transaction(
                NSLOCTEXT("UnrealMCP", "UpdateBlueprintComponentDefaults", "UnrealMCP Update Blueprint Component Defaults"));
            Blueprint->Modify(); ExistingNode->Modify(); ExistingNode->ComponentTemplate->Modify();
            TArray<FPropertyValue> Values;
            if (!ApplyPropertyDefaults(ExistingNode->ComponentTemplate, Spec.PropertyDefaults, Values, OutError)) return false;
            for (const FPropertyValue& Value : Values)
            {
                ComponentResults[0].AppliedProperties.Add(Value.Name);
                ComponentResults[0].bChanged |= Value.bChanged;
            }
            ComponentResults[0].bUpdatedExisting = ComponentResults[0].bChanged;
            if (!ComponentResults[0].bChanged) return true;
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        else if (bDryRun || ComponentResults[0].bAlreadyExists)
        {
            return true;
        }
        else
        {
            const FScopedTransaction Transaction(
                NSLOCTEXT("UnrealMCP", "AddBlueprintComponent", "UnrealMCP Add Blueprint Component"));
            Blueprint->Modify();
            Blueprint->SimpleConstructionScript->Modify();
            if (!AddValidatedSpecs(Blueprint, Specs, ComponentResults, OutError)) return false;
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }

        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(
                Blueprint,
                EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
            bCompileSucceeded = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
            if (!bCompileSucceeded)
            {
                OutError = TEXT("Component was added, but Blueprint compilation failed; use ValidateBlueprint for diagnostics.");
                return false;
            }
        }
        if (bSave && !BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, OutError))
        {
            return false;
        }
        if (bSave)
        {
            bIndexRefreshed = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexRefreshError);
        }
        return true;
    }, ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, EMCPErrorCode::InternalError, ExecutionError);
    }

    const FComponentResult& ComponentResult = ComponentResults[0];
    FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("componentName"), ComponentResult.ComponentName);
    Result->SetStringField(TEXT("componentClassPath"), ComponentResult.ComponentClassPath);
    Result->SetStringField(TEXT("parentComponentName"), ComponentResult.ParentComponentName);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("alreadyExists"), ComponentResult.bAlreadyExists);
    Result->SetBoolField(TEXT("added"), ComponentResult.bAdded);
    Result->SetBoolField(TEXT("updateExistingDefaults"), bUpdateExistingDefaults);
    Result->SetBoolField(TEXT("updatedExisting"), ComponentResult.bUpdatedExisting);
    Result->SetBoolField(TEXT("changed"), ComponentResult.bChanged);
    TArray<TSharedPtr<FJsonValue>> AppliedProperties;
    for (const FString& PropertyName : ComponentResult.AppliedProperties)
    {
        AppliedProperties.Add(MakeShared<FJsonValueString>(PropertyName));
    }
    Result->SetArrayField(TEXT("appliedProperties"), AppliedProperties);
    Result->SetBoolField(TEXT("compiled"), bCompile && ComponentResult.bChanged);
    Result->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
    Result->SetBoolField(TEXT("saved"), bSave && ComponentResult.bChanged);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintComponentTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));
    Properties->SetObjectField(TEXT("componentName"), BuildStringProperty(TEXT("Unique component variable name.")));
    Properties->SetObjectField(TEXT("componentClassPath"), BuildStringProperty(TEXT("UActorComponent-derived /Script/... or generated Blueprint class path.")));
    Properties->SetObjectField(TEXT("parentComponentName"), BuildStringProperty(TEXT("Optional existing scene-component parent name.")));
    TSharedRef<FJsonObject> Defaults = MakeShared<FJsonObject>();
    Defaults->SetStringField(TEXT("type"), TEXT("object"));
    Defaults->SetStringField(TEXT("description"), TEXT("Optional editable component-template properties. Values must be JSON primitives or Unreal export-text strings."));
    Defaults->SetBoolField(TEXT("additionalProperties"), true);
    Properties->SetObjectField(TEXT("propertyDefaults"), Defaults);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate without mutation.")));
    Properties->SetObjectField(TEXT("updateExistingDefaults"), BuildBoolProperty(TEXT("If the named component already exists, explicitly update supplied propertyDefaults. Defaults false.")));
    Properties->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile only this Blueprint after mutation.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save after mutation.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("componentName")),
        MakeShared<FJsonValueString>(TEXT("componentClassPath"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
