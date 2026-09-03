#include "Tools/RemoveInputMappingContextMappingTool.h"

#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "Tools/InputAssetToolUtils.h"

FRemoveInputMappingContextMappingTool::FRemoveInputMappingContextMappingTool()
    : FMCPToolBase(
        TEXT("RemoveInputMappingContextMapping"),
        TEXT("Removes one exact existing Action+Key mapping row from an InputMappingContext. Fails clearly rather than guessing when zero or more than one row matches."))
{
}

UnrealMCP::FMCPResponse FRemoveInputMappingContextMappingTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

    FString ObjectPath;
    FString InputActionPath;
    FString KeyName;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath)
        || !Request.Params->TryGetStringField(TEXT("inputActionPath"), InputActionPath)
        || !Request.Params->TryGetStringField(TEXT("key"), KeyName))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("RemoveInputMappingContextMapping requires objectPath, inputActionPath, and key."));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    bool bIndexRefreshed = false;
    FString ResolvedActionPath;
    FString SavedFilename;
    FString IndexRefreshError;
    FString ExecutionError;
    TSharedPtr<FJsonObject> RemovedRow;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UClass* ContextClass = nullptr;
        if (!InputAssetToolUtils::ResolveInputMappingContextClass(ContextClass, OutError))
        {
            return false;
        }
        UClass* ActionClass = nullptr;
        if (!InputAssetToolUtils::ResolveInputActionClass(ActionClass, OutError))
        {
            return false;
        }

        UObject* ContextObject = nullptr;
        if (!InputAssetToolUtils::ResolveExistingObjectOfClass(ObjectPath, ContextClass, ContextObject, OutError))
        {
            return false;
        }
        UObject* ActionObject = nullptr;
        if (!InputAssetToolUtils::ResolveExistingObjectOfClass(InputActionPath, ActionClass, ActionObject, OutError))
        {
            return false;
        }
        ResolvedActionPath = ActionObject->GetPathName();

        const FKey Key = FKey(FName(*KeyName));
        if (!Key.IsValid())
        {
            OutError = FString::Printf(TEXT("'%s' is not a recognized FKey name."), *KeyName);
            return false;
        }

        FArrayProperty* MappingsProperty = InputAssetToolUtils::FindMappingsProperty(ContextClass, OutError);
        if (MappingsProperty == nullptr)
        {
            return false;
        }

        TArray<int32> MatchingIndices;
        const int32 MatchCount = InputAssetToolUtils::CountMatchingMappings(
            ContextObject, MappingsProperty, ActionObject, Key, &MatchingIndices);
        if (MatchCount == 0)
        {
            OutError = FString::Printf(
                TEXT("No mapping row found for action '%s' and key '%s'."), *ResolvedActionPath, *KeyName);
            return false;
        }
        if (MatchCount > 1)
        {
            OutError = FString::Printf(
                TEXT("%d identical mapping rows found for action '%s' and key '%s'; refusing to guess which to remove."),
                MatchCount, *ResolvedActionPath, *KeyName);
            return false;
        }

        UScriptStruct* MappingStruct = InputAssetToolUtils::GetMappingElementStruct(MappingsProperty);
        {
            FScriptArrayHelper Helper(MappingsProperty, MappingsProperty->ContainerPtrToValuePtr<void>(ContextObject));
            RemovedRow = InputAssetToolUtils::SerializeMappingRow(Helper.GetRawPtr(MatchingIndices[0]), MappingStruct, MatchingIndices[0]);
        }

        if (bDryRun)
        {
            return true;
        }

        if (!BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("confirm"), false))
        {
            OutError = TEXT("Inspect the exact mapping with dryRun=true, then supply confirm=true to remove it.");
            return false;
        }

        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "RemoveInputMappingContextMapping", "UnrealMCP Remove Input Mapping Context Mapping"));
        ContextObject->Modify();
        if (!InputAssetToolUtils::CallMappingContextFunction(ContextObject, TEXT("UnmapKey"), ActionObject, &Key, OutError))
        {
            return false;
        }

        const int32 RemainingCount = InputAssetToolUtils::CountMatchingMappings(ContextObject, MappingsProperty, ActionObject, Key);
        if (RemainingCount != 0)
        {
            OutError = TEXT("UnmapKey completed but a matching mapping row still remains afterward.");
            return false;
        }

        ContextObject->GetPackage()->MarkPackageDirty();
        if (bSave && !BlueprintEditToolUtils::SaveAsset(ContextObject, SavedFilename, OutError))
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
        return BuildError(Request, EMCPErrorCode::InvalidParams, ExecutionError);
    }

    FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("inputActionPath"), ResolvedActionPath);
    Result->SetStringField(TEXT("key"), KeyName);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("removed"), !bDryRun);
    if (RemovedRow.IsValid())
    {
        Result->SetObjectField(TEXT("removedMapping"), RemovedRow);
    }
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FRemoveInputMappingContextMappingTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target InputMappingContext asset object path.")));
    Properties->SetObjectField(TEXT("inputActionPath"), BuildStringProperty(TEXT("Exact UInputAction asset object path currently mapped.")));
    Properties->SetObjectField(TEXT("key"), BuildStringProperty(TEXT("FKey name of the exact row to remove, for example LeftMouseButton.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate and return the row that would be removed without removing it.")));
    Properties->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required true for a real removal after inspecting the dry-run row.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("inputActionPath")),
        MakeShared<FJsonValueString>(TEXT("key"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
