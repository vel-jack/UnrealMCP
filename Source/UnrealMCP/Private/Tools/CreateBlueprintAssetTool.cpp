#include "Tools/CreateBlueprintAssetTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FCreateBlueprintAssetTool::FCreateBlueprintAssetTool()
    : FMCPToolBase(TEXT("CreateBlueprintAsset"), TEXT("Creates a Blueprint asset with an explicit parent class. Rejects overwrite and supports dry-run and optional saving."))
{
}

UnrealMCP::FMCPResponse FCreateBlueprintAssetTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString PackagePath;
    FString ParentClassPath;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("packagePath"), PackagePath)
        || !Request.Params->TryGetStringField(TEXT("parentClassPath"), ParentClassPath))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("CreateBlueprintAsset requires packagePath and parentClassPath."));
    }

    PackagePath.RemoveFromEnd(TEXT(".uasset"));
    if (!FPackageName::IsValidLongPackageName(PackagePath) || !PackagePath.StartsWith(TEXT("/Game/")))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("packagePath must be a valid /Game/... long package path without .uasset."));
    }

    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSaveAfterEdit = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("saveAfterEdit"), false);
    FString ObjectPath;
    FString GeneratedClassPath;
    FString SavedFilename;
    FString IndexRefreshError;
    bool bIndexRefreshed = false;
    FString ExecutionError;
    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
        if (FindObject<UObject>(nullptr, *ObjectPath) != nullptr || FPackageName::DoesPackageExist(PackagePath))
        {
            OutError = FString::Printf(TEXT("Asset already exists at '%s'; overwrite is not allowed."), *ObjectPath);
            return false;
        }

        UClass* ParentClass = nullptr;
        if (!UnrealMCP::BlueprintEditToolUtils::ResolveClass(ParentClassPath, ParentClass, OutError)) return false;
        if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
        {
            OutError = FString::Printf(TEXT("Class '%s' cannot be used as a Blueprint parent."), *ParentClass->GetPathName());
            return false;
        }
        GeneratedClassPath = FString::Printf(TEXT("%s.%s_C"), *PackagePath, *AssetName);
        if (bDryRun) return true;

        const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "CreateBlueprintAsset", "UnrealMCP Create Blueprint Asset"));
        UPackage* Package = CreatePackage(*PackagePath);
        UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
            ParentClass, Package, *AssetName, BPTYPE_Normal,
            UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), FName(TEXT("UnrealMCP")));
        if (Blueprint == nullptr)
        {
            OutError = TEXT("Unreal failed to create the Blueprint asset.");
            return false;
        }
        FAssetRegistryModule::AssetCreated(Blueprint);
        Package->MarkPackageDirty();
        if (bSaveAfterEdit && !UnrealMCP::BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, OutError)) return false;
        if (bSaveAfterEdit) bIndexRefreshed = UnrealMCP::BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexRefreshError);
        return true;
    }, ExecutionError);

    if (!bSucceeded) return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetBoolField(TEXT("created"), !bDryRun);
    Result->SetBoolField(TEXT("saved"), !bDryRun && bSaveAfterEdit);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetStringField(TEXT("generatedClassPath"), GeneratedClassPath);
    Result->SetStringField(TEXT("parentClassPath"), ParentClassPath);
    Result->SetStringField(TEXT("savedFilename"), SavedFilename);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
    Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FCreateBlueprintAssetTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("packagePath"), BuildStringProperty(TEXT("New /Game/... package path without .uasset. Existing assets are never overwritten.")));
    Properties->SetObjectField(TEXT("parentClassPath"), BuildStringProperty(TEXT("Parent /Script/... or generated Blueprint class path.")));
    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate without creating the asset.")));
    Properties->SetObjectField(TEXT("saveAfterEdit"), BuildBoolProperty(TEXT("Save immediately after creation. Defaults to false.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> Required{MakeShared<FJsonValueString>(TEXT("packagePath")), MakeShared<FJsonValueString>(TEXT("parentClassPath"))};
    Schema->SetArrayField(TEXT("required"), Required); return Schema;
}
