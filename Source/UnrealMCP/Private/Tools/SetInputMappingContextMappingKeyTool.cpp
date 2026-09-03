#include "Tools/SetInputMappingContextMappingKeyTool.h"

#include "Misc/DataValidation.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "Tools/InputAssetToolUtils.h"

FSetInputMappingContextMappingKeyTool::FSetInputMappingContextMappingKeyTool()
    : FMCPToolBase(TEXT("SetInputMappingContextMappingKey"),
        TEXT("Replaces the key of one exact Action+Key row, preserving its position, action, inline objects and metadata. Static asset editing only; no runtime mapping rebuild."))
{
}

UnrealMCP::FMCPResponse FSetInputMappingContextMappingKeyTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath, ActionPath, OldKeyName, NewKeyName;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("inputActionPath"), ActionPath)
        || !Request.Params->TryGetStringField(TEXT("key"), OldKeyName)
        || !Request.Params->TryGetStringField(TEXT("newKey"), NewKeyName))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams,
            TEXT("Requires objectPath, inputActionPath, key (expected current key), and newKey."));
    }
    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    bool bChanged = false, bSaved = false, bIndexRefreshed = false, bRolledBack = false, bValidated = false;
    FString Error, SaveError, SavedFilename, IndexError;
    TSharedPtr<FJsonObject> Before, After;
    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UClass* ContextClass = nullptr;
        UClass* ActionClass = nullptr;
        UObject* Context = nullptr;
        UObject* Action = nullptr;
        if (!InputAssetToolUtils::ResolveInputMappingContextClass(ContextClass, OutError)
            || !InputAssetToolUtils::ResolveInputActionClass(ActionClass, OutError)
            || !InputAssetToolUtils::ResolveExistingObjectOfClass(ObjectPath, ContextClass, Context, OutError)
            || !InputAssetToolUtils::ResolveExistingObjectOfClass(ActionPath, ActionClass, Action, OutError)) return false;
        if (Context->GetClass() != ContextClass)
        {
            OutError = TEXT("Custom InputMappingContext subclasses are not supported by this bounded key-edit operation.");
            return false;
        }
        const FKey OldKey{FName(*OldKeyName)}, NewKey{FName(*NewKeyName)};
        if (!OldKey.IsValid() || !NewKey.IsValid())
        {
            OutError = TEXT("key and newKey must both be recognized FKey names.");
            return false;
        }
        FArrayProperty* Mappings = InputAssetToolUtils::FindMappingsProperty(ContextClass, OutError);
        if (!Mappings) return false;
        UScriptStruct* RowStruct = InputAssetToolUtils::GetMappingElementStruct(Mappings);
        FStructProperty* KeyProperty = RowStruct ? FindFProperty<FStructProperty>(RowStruct, TEXT("Key")) : nullptr;
        if (!KeyProperty || KeyProperty->Struct != TBaseStructure<FKey>::Get())
        {
            OutError = TEXT("Cannot resolve mapping Key property as FKey.");
            return false;
        }
        TArray<int32> Indices;
        if (InputAssetToolUtils::CountMatchingMappings(Context, Mappings, Action, OldKey, &Indices) != 1)
        {
            OutError = TEXT("Expected exactly one current Action+Key row; missing or ambiguous rows cannot be replaced. Use the original operationId to replay an uncertain edit.");
            return false;
        }
        if (OldKey != NewKey && InputAssetToolUtils::CountMatchingMappings(Context, Mappings, Action, NewKey) != 0)
        {
            OutError = TEXT("The target Action+newKey row already exists; replacement would create a duplicate.");
            return false;
        }
        const int32 Index = Indices[0];
        FScriptArrayHelper Rows(Mappings, Mappings->ContainerPtrToValuePtr<void>(Context));
        void* Row = Rows.GetRawPtr(Index);
        Before = InputAssetToolUtils::SerializeMappingRow(Row, RowStruct, Index);
        FStructOnScope Original(RowStruct), Proposed(RowStruct);
        RowStruct->CopyScriptStruct(Original.GetStructMemory(), Row);
        RowStruct->CopyScriptStruct(Proposed.GetStructMemory(), Row);
        *KeyProperty->ContainerPtrToValuePtr<FKey>(Proposed.GetStructMemory()) = NewKey;
        After = InputAssetToolUtils::SerializeMappingRow(Proposed.GetStructMemory(), RowStruct, Index);

        // Validate a detached context first. Never temporarily change the real asset for a dry-run.
        UObject* Preview = DuplicateObject(Context, GetTransientPackage());
        if (!Preview)
        {
            OutError = TEXT("Could not create detached validation preview.");
            return false;
        }
        Preview->ClearFlags(RF_Public | RF_Standalone);
        FScriptArrayHelper PreviewRows(Mappings, Mappings->ContainerPtrToValuePtr<void>(Preview));
        *KeyProperty->ContainerPtrToValuePtr<FKey>(PreviewRows.GetRawPtr(Index)) = NewKey;
        FDataValidationContext PreviewValidation;
        if (static_cast<const UObject*>(Preview)->IsDataValid(PreviewValidation) == EDataValidationResult::Invalid)
        {
            OutError = TEXT("Proposed context failed Enhanced Input data validation; nothing changed.");
            return false;
        }
        bValidated = true;
        if (bDryRun || OldKey == NewKey) return true;
        if (!BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirm"), false))
        {
            OutError = TEXT("Inspect the dry-run replacement, then supply confirm=true.");
            return false;
        }

        UPackage* Package = Context->GetPackage();
        const bool bWasDirty = Package->IsDirty();
        const bool bWasTransactional = Context->HasAnyFlags(RF_Transactional);
        {
            FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "SetInputMappingKey", "UnrealMCP Set Input Mapping Key"));
            Context->SetFlags(RF_Transactional);
            Context->Modify();
            *KeyProperty->ContainerPtrToValuePtr<FKey>(Row) = NewKey;
            FDataValidationContext Validation;
            if (static_cast<const UObject*>(Context)->IsDataValid(Validation) == EDataValidationResult::Invalid
                || !RowStruct->CompareScriptStruct(Row, Proposed.GetStructMemory(), 0))
            {
                RowStruct->CopyScriptStruct(Row, Original.GetStructMemory());
                Transaction.Cancel();
                if (!bWasTransactional) Context->ClearFlags(RF_Transactional);
                Package->SetDirtyFlag(bWasDirty);
                bRolledBack = true;
                bValidated = false;
                OutError = TEXT("Final mapping validation failed; original row and dirty state restored.");
                return false;
            }
            Context->PostEditChange();
            Package->MarkPackageDirty();
            bChanged = true;
            After = InputAssetToolUtils::SerializeMappingRow(Row, RowStruct, Index);
        }
        // A failed disk save is not a failed in-memory edit and must never invite a blind retry.
        if (bSave)
        {
            bSaved = BlueprintEditToolUtils::SaveAsset(Context, SavedFilename, SaveError);
            if (bSaved) bIndexRefreshed = BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexError);
        }
        return true;
    }, Error);

    FMCPResponse Response;
    Response.Id = Request.Id;
    auto Result = BuildBooleanResult(bSucceeded && SaveError.IsEmpty());
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
    Result->SetBoolField(TEXT("dataValidated"), bValidated);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetStringField(TEXT("saveError"), SaveError);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexError);
    Result->SetBoolField(TEXT("runtimeMappingsRebuilt"), false);
    if (Before.IsValid()) Result->SetObjectField(TEXT("before"), Before);
    if (After.IsValid()) Result->SetObjectField(TEXT("proposedMapping"), After);
    if (!bSucceeded || !SaveError.IsEmpty())
    {
        Result->SetStringField(TEXT("errorCode"), !bSucceeded ? TEXT("input_mapping_edit_rejected") : TEXT("asset_save_failed"));
        Result->SetStringField(TEXT("message"), !bSucceeded ? Error : SaveError);
    }
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FSetInputMappingContextMappingKeyTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    auto Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Exact InputMappingContext object path.")));
    Properties->SetObjectField(TEXT("inputActionPath"), BuildStringProperty(TEXT("Exact mapped InputAction object path.")));
    Properties->SetObjectField(TEXT("key"), BuildStringProperty(TEXT("Expected current key; ambiguity or a stale key is rejected.")));
    Properties->SetObjectField(TEXT("newKey"), BuildStringProperty(TEXT("Replacement FKey name.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate on a detached preview and report before/proposed row without editing.")));
    Properties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required true for a real replacement.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Optional save and partial index refresh; default false. A no-op does not save.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required;
    for (const TCHAR* FieldName : {TEXT("objectPath"), TEXT("inputActionPath"), TEXT("key"), TEXT("newKey")})
        Required.Add(MakeShared<FJsonValueString>(FieldName));
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
