#include "Tools/CreateInputActionTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "Tools/InputAssetToolUtils.h"

FCreateInputActionTool::FCreateInputActionTool()
    : FMCPToolBase(
        TEXT("CreateInputAction"),
        TEXT("Creates a new UInputAction asset with an explicit value type. Rejects overwrite and supports dry-run and optional saving."))
{
}

UnrealMCP::FMCPResponse FCreateInputActionTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

    FString PackagePath;
    FString ValueTypeName;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("packagePath"), PackagePath)
        || !Request.Params->TryGetStringField(TEXT("valueType"), ValueTypeName))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("CreateInputAction requires packagePath and valueType."));
    }

    PackagePath.RemoveFromEnd(TEXT(".uasset"));
    if (!FPackageName::IsValidLongPackageName(PackagePath) || !PackagePath.StartsWith(TEXT("/Game/")))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("packagePath must be a valid /Game/... long package path without .uasset."));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSaveAfterEdit = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    FString ObjectPath;
    FString ResolvedValueType;
    FString SavedFilename;
    FString IndexRefreshError;
    bool bIndexRefreshed = false;
    FString ExecutionError;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UClass* InputActionClass = nullptr;
        if (!InputAssetToolUtils::ResolveInputActionClass(InputActionClass, OutError))
        {
            return false;
        }

        FEnumProperty* ValueTypeProperty = nullptr;
        int64 ValueTypeValue = 0;
        if (!InputAssetToolUtils::ParseInputActionValueType(InputActionClass, ValueTypeName, ValueTypeProperty, ValueTypeValue, OutError))
        {
            return false;
        }
        ResolvedValueType = ValueTypeProperty->GetEnum()->GetNameStringByValue(ValueTypeValue);

        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
        if (FindObject<UObject>(nullptr, *ObjectPath) != nullptr || FPackageName::DoesPackageExist(PackagePath))
        {
            OutError = FString::Printf(TEXT("Asset already exists at '%s'; overwrite is not allowed."), *ObjectPath);
            return false;
        }

        if (bDryRun)
        {
            return true;
        }

        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "CreateInputAction", "UnrealMCP Create Input Action"));
        UPackage* Package = CreatePackage(*PackagePath);
        UObject* NewAction = NewObject<UObject>(Package, InputActionClass, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
        if (NewAction == nullptr)
        {
            OutError = TEXT("Unreal failed to create the InputAction asset.");
            return false;
        }

        ValueTypeProperty->GetUnderlyingProperty()->SetIntPropertyValue(
            ValueTypeProperty->ContainerPtrToValuePtr<void>(NewAction), ValueTypeValue);

        FAssetRegistryModule::AssetCreated(NewAction);
        Package->MarkPackageDirty();
        if (bSaveAfterEdit && !BlueprintEditToolUtils::SaveAsset(NewAction, SavedFilename, OutError))
        {
            return false;
        }
        if (bSaveAfterEdit)
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
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("created"), !bDryRun);
    Result->SetBoolField(TEXT("saved"), !bDryRun && bSaveAfterEdit);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("valueType"), ResolvedValueType);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FCreateInputActionTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("packagePath"), BuildStringProperty(TEXT("New /Game/... package path without .uasset. Existing assets are never overwritten.")));
    Properties->SetObjectField(TEXT("valueType"), BuildStringProperty(TEXT("Boolean, Axis1D, Axis2D, or Axis3D (matches EInputActionValueType).")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate without creating the asset.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save immediately after creation. Defaults to false.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{
        MakeShared<FJsonValueString>(TEXT("packagePath")),
        MakeShared<FJsonValueString>(TEXT("valueType"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
