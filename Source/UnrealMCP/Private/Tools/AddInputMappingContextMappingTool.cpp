#include "Tools/AddInputMappingContextMappingTool.h"

#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "Tools/InputAssetToolUtils.h"

FAddInputMappingContextMappingTool::FAddInputMappingContextMappingTool()
    : FMCPToolBase(
        TEXT("AddInputMappingContextMapping"),
        TEXT("Adds one key-mapping row to an existing InputMappingContext. Idempotent if the identical Action+Key row already exists."))
{
}

UnrealMCP::FMCPResponse FAddInputMappingContextMappingTool::Execute(const UnrealMCP::FMCPRequest& Request) const
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
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("AddInputMappingContextMapping requires objectPath, inputActionPath, and key."));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    bool bAlreadyExists = false;
    bool bIndexRefreshed = false;
    int32 ResultIndex = INDEX_NONE;
    FString ResolvedActionPath;
    FString SavedFilename;
    FString IndexRefreshError;
    FString ExecutionError;
    TSharedPtr<FJsonObject> RowResult;

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

        TArray<int32> ExistingIndices;
        const int32 ExistingCount = InputAssetToolUtils::CountMatchingMappings(
            ContextObject, MappingsProperty, ActionObject, Key, &ExistingIndices);
        if (ExistingCount > 0)
        {
            bAlreadyExists = true;
            ResultIndex = ExistingIndices[0];
            UScriptStruct* MappingStruct = InputAssetToolUtils::GetMappingElementStruct(MappingsProperty);
            FScriptArrayHelper Helper(MappingsProperty, MappingsProperty->ContainerPtrToValuePtr<void>(ContextObject));
            RowResult = InputAssetToolUtils::SerializeMappingRow(Helper.GetRawPtr(ResultIndex), MappingStruct, ResultIndex);
            return true;
        }

        if (bDryRun)
        {
            return true;
        }

        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "AddInputMappingContextMapping", "UnrealMCP Add Input Mapping Context Mapping"));
        ContextObject->Modify();
        if (!InputAssetToolUtils::CallMappingContextFunction(ContextObject, TEXT("MapKey"), ActionObject, &Key, OutError))
        {
            return false;
        }

        TArray<int32> NewIndices;
        const int32 NewCount = InputAssetToolUtils::CountMatchingMappings(
            ContextObject, MappingsProperty, ActionObject, Key, &NewIndices);
        if (NewCount == 0)
        {
            OutError = TEXT("MapKey completed but the new mapping row could not be found afterward.");
            return false;
        }
        ResultIndex = NewIndices.Last();
        UScriptStruct* MappingStruct = InputAssetToolUtils::GetMappingElementStruct(MappingsProperty);
        FScriptArrayHelper Helper(MappingsProperty, MappingsProperty->ContainerPtrToValuePtr<void>(ContextObject));
        RowResult = InputAssetToolUtils::SerializeMappingRow(Helper.GetRawPtr(ResultIndex), MappingStruct, ResultIndex);

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
        return BuildError(Request, EMCPErrorCode::InternalError, ExecutionError);
    }

    FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("inputActionPath"), ResolvedActionPath);
    Result->SetStringField(TEXT("key"), KeyName);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("alreadyExists"), bAlreadyExists);
    Result->SetBoolField(TEXT("added"), !bDryRun && !bAlreadyExists);
    if (RowResult.IsValid())
    {
        Result->SetObjectField(TEXT("mapping"), RowResult);
    }
    Result->SetBoolField(TEXT("saved"), bSave && !bDryRun && !bAlreadyExists);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FAddInputMappingContextMappingTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Target InputMappingContext asset object path.")));
    Properties->SetObjectField(TEXT("inputActionPath"), BuildStringProperty(TEXT("Exact UInputAction asset object path to map.")));
    Properties->SetObjectField(TEXT("key"), BuildStringProperty(TEXT("FKey name, for example LeftMouseButton, MiddleMouseButton, W, Gamepad_FaceButton_Bottom.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate asset/action/key without adding the mapping.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save and partially refresh the index.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("objectPath")),
        MakeShared<FJsonValueString>(TEXT("inputActionPath")),
        MakeShared<FJsonValueString>(TEXT("key"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
