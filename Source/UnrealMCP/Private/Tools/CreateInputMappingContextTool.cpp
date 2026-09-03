#include "Tools/CreateInputMappingContextTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "Tools/InputAssetToolUtils.h"

FCreateInputMappingContextTool::FCreateInputMappingContextTool()
    : FMCPToolBase(
        TEXT("CreateInputMappingContext"),
        TEXT("Creates a new UInputMappingContext asset. Rejects overwrite and supports dry-run and optional saving."))
{
}

UnrealMCP::FMCPResponse FCreateInputMappingContextTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

    FString PackagePath;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("packagePath"), PackagePath))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("CreateInputMappingContext requires packagePath."));
    }

    PackagePath.RemoveFromEnd(TEXT(".uasset"));
    if (!FPackageName::IsValidLongPackageName(PackagePath) || !PackagePath.StartsWith(TEXT("/Game/")))
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("packagePath must be a valid /Game/... long package path without .uasset."));
    }

    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSaveAfterEdit = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    FString ObjectPath;
    FString SavedFilename;
    FString IndexRefreshError;
    bool bIndexRefreshed = false;
    FString ExecutionError;

    const bool bSucceeded = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UClass* ContextClass = nullptr;
        if (!InputAssetToolUtils::ResolveInputMappingContextClass(ContextClass, OutError))
        {
            return false;
        }

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

        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "CreateInputMappingContext", "UnrealMCP Create Input Mapping Context"));
        UPackage* Package = CreatePackage(*PackagePath);
        UObject* NewContext = NewObject<UObject>(Package, ContextClass, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
        if (NewContext == nullptr)
        {
            OutError = TEXT("Unreal failed to create the InputMappingContext asset.");
            return false;
        }

        FAssetRegistryModule::AssetCreated(NewContext);
        Package->MarkPackageDirty();
        if (bSaveAfterEdit && !BlueprintEditToolUtils::SaveAsset(NewContext, SavedFilename, OutError))
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
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FCreateInputMappingContextTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("packagePath"), BuildStringProperty(TEXT("New /Game/... package path without .uasset. Existing assets are never overwritten.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate without creating the asset.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save immediately after creation. Defaults to false.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{MakeShared<FJsonValueString>(TEXT("packagePath"))};
    Schema->SetArrayField(TEXT("required"), Required);
    return Schema;
}
