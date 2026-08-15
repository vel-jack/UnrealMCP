#include "Tools/AddBlueprintComponentsTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintComponentEditUtils.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FAddBlueprintComponentsTool::FAddBlueprintComponentsTool()
    : FMCPToolBase(
        TEXT("AddBlueprintComponents"),
        TEXT("Prevalidates and transactionally adds an ordered component set to one Blueprint, then optionally compiles, saves, and partially refreshes the index once."))
{
}

UnrealMCP::FMCPResponse FAddBlueprintComponentsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    using namespace UnrealMCP::BlueprintComponentEditUtils;

    FString ObjectPath;
    const TArray<TSharedPtr<FJsonValue>>* ComponentValues = nullptr;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || ObjectPath.IsEmpty()
        || !Request.Params->TryGetArrayField(TEXT("components"), ComponentValues)
        || ComponentValues == nullptr
        || ComponentValues->IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("AddBlueprintComponents requires objectPath and a non-empty components array."));
    }

    TArray<FComponentSpec> Specs;
    Specs.Reserve(ComponentValues->Num());
    for (int32 Index = 0; Index < ComponentValues->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> SpecObject = (*ComponentValues)[Index].IsValid()
            ? (*ComponentValues)[Index]->AsObject()
            : nullptr;
        FComponentSpec Spec;
        FString ParseError;
        if (!ParseComponentSpec(SpecObject, Spec, ParseError))
        {
            return BuildError(
                Request,
                EMCPErrorCode::InvalidParams,
                FString::Printf(TEXT("Invalid component at index %d: %s"), Index, *ParseError));
        }
        Specs.Add(MoveTemp(Spec));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bCompile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("compileAfterEdit"), true);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), true);
    bool bAnyAdded = false;
    bool bCompileSucceeded = false;
    bool bIndexRefreshed = false;
    FString SavedFilename;
    FString ExecutionError;
    FString IndexRefreshError;
    TArray<FComponentResult> ComponentResults;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)
            || !ResolveAndValidateSpecs(Blueprint, Specs, ComponentResults, OutError))
        {
            return false;
        }

        for (const FComponentResult& Result : ComponentResults)
        {
            bAnyAdded |= !Result.bAlreadyExists;
        }
        if (bDryRun || !bAnyAdded)
        {
            return true;
        }

        const FScopedTransaction Transaction(
            NSLOCTEXT("UnrealMCP", "AddBlueprintComponents", "UnrealMCP Add Blueprint Components"));
        Blueprint->Modify();
        Blueprint->SimpleConstructionScript->Modify();
        if (!AddValidatedSpecs(Blueprint, Specs, ComponentResults, OutError))
        {
            return false;
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(
                Blueprint,
                EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
            bCompileSucceeded = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
            if (!bCompileSucceeded)
            {
                OutError = TEXT("Components were added, but Blueprint compilation failed; use ValidateBlueprint for diagnostics.");
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

    TArray<TSharedPtr<FJsonValue>> ResultItems;
    int32 AddedCount = 0;
    int32 ExistingCount = 0;
    for (const FComponentResult& ComponentResult : ComponentResults)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("componentName"), ComponentResult.ComponentName);
        Item->SetStringField(TEXT("componentClassPath"), ComponentResult.ComponentClassPath);
        Item->SetStringField(TEXT("parentComponentName"), ComponentResult.ParentComponentName);
        Item->SetBoolField(TEXT("alreadyExists"), ComponentResult.bAlreadyExists);
        Item->SetBoolField(TEXT("added"), ComponentResult.bAdded);
        AddedCount += ComponentResult.bAdded ? 1 : 0;
        ExistingCount += ComponentResult.bAlreadyExists ? 1 : 0;
        TArray<TSharedPtr<FJsonValue>> AppliedProperties;
        for (const FString& PropertyName : ComponentResult.AppliedProperties)
        {
            AppliedProperties.Add(MakeShared<FJsonValueString>(PropertyName));
        }
        Item->SetArrayField(TEXT("appliedProperties"), AppliedProperties);
        ResultItems.Add(MakeShared<FJsonValueObject>(Item));
    }

    FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetNumberField(TEXT("requestedCount"), Specs.Num());
    Result->SetNumberField(TEXT("addedCount"), AddedCount);
    Result->SetNumberField(TEXT("existingCount"), ExistingCount);
    Result->SetArrayField(TEXT("components"), ResultItems);
    Result->SetBoolField(TEXT("compiled"), bCompile && bAnyAdded && !bDryRun);
    Result->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
    Result->SetBoolField(TEXT("saved"), bSave && bAnyAdded && !bDryRun);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddBlueprintComponentsTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target Blueprint object path.")));

    TSharedRef<FJsonObject> ComponentProperties = MakeShared<FJsonObject>();
    ComponentProperties->SetObjectField(TEXT("componentName"), BuildStringProperty(TEXT("Unique component variable name.")));
    ComponentProperties->SetObjectField(TEXT("componentClassPath"), BuildStringProperty(TEXT("UActorComponent-derived class path.")));
    ComponentProperties->SetObjectField(TEXT("parentComponentName"), BuildStringProperty(TEXT("Optional scene-component parent. A batch parent must appear earlier in the array.")));
    TSharedRef<FJsonObject> Defaults = MakeShared<FJsonObject>();
    Defaults->SetStringField(TEXT("type"), TEXT("object"));
    Defaults->SetStringField(TEXT("description"), TEXT("Optional editable template properties using JSON primitives or Unreal export-text strings."));
    Defaults->SetBoolField(TEXT("additionalProperties"), true);
    ComponentProperties->SetObjectField(TEXT("propertyDefaults"), Defaults);

    TSharedRef<FJsonObject> ComponentItem = MakeShared<FJsonObject>();
    ComponentItem->SetStringField(TEXT("type"), TEXT("object"));
    ComponentItem->SetObjectField(TEXT("properties"), ComponentProperties);
    TArray<TSharedPtr<FJsonValue>> ComponentRequired{
        MakeShared<FJsonValueString>(TEXT("componentName")),
        MakeShared<FJsonValueString>(TEXT("componentClassPath"))};
    ComponentItem->SetArrayField(TEXT("required"), ComponentRequired);

    TSharedRef<FJsonObject> Components = MakeShared<FJsonObject>();
    Components->SetStringField(TEXT("type"), TEXT("array"));
    Components->SetStringField(TEXT("description"), TEXT("Ordered components to add. The entire batch is validated before mutation."));
    Components->SetNumberField(TEXT("minItems"), 1);
    Components->SetObjectField(TEXT("items"), ComponentItem);
    Properties->SetObjectField(TEXT("components"), Components);
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate the complete batch without mutation.")));
    Properties->SetObjectField(TEXT("compileAfterEdit"), BuildBoolProperty(TEXT("Compile once after the batch. Defaults to true.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index once. Defaults to true.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("components"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
