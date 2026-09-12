#include "Tools/SaveAllDirtyPackagesTool.h"
#include "Dom/JsonObject.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
    // UPackage::SavePackage only returns a bool, so name the cause the caller can actually act on
    // instead of reporting a bare failure. A read-only file is the usual cause under source control.
    FString DescribeSaveFailure(const FString& Filename)
    {
        IFileManager& FileManager = IFileManager::Get();
        if (!FileManager.FileExists(*Filename))
        {
            return TEXT("SavePackage failed and no file exists at the target path; check that the package name maps to a writable content directory.");
        }
        if (FileManager.IsReadOnly(*Filename))
        {
            return TEXT("The target file is read-only; check it out from source control or clear the read-only flag, then retry.");
        }
        return TEXT("SavePackage reported failure; see the Unreal log for the underlying save error.");
    }
}

FSaveAllDirtyPackagesTool::FSaveAllDirtyPackagesTool()
    : FMCPToolBase(TEXT("SaveAllDirtyPackages"), TEXT("Saves every dirty package without prompting, so a subsequent editor shutdown does not block on a save-confirmation dialog. Supports dry-run to report dirty packages without saving.")) {}

UnrealMCP::FMCPResponse FSaveAllDirtyPackagesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);

    TArray<FString> DirtyPackageNames;
    TArray<FString> SavedPackageNames;
    TArray<TSharedPtr<FJsonValue>> FailedPackagesJson;
    TArray<FString> StillDirtyPackageNames;
    int32 FailedPackageCount = 0;
    FString ExecutionError;

    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        TArray<UPackage*> DirtyPackages;
        FEditorFileUtils::GetDirtyPackages(DirtyPackages);
        for (const UPackage* Package : DirtyPackages)
        {
            DirtyPackageNames.Add(Package->GetName());
        }

        if (bDryRun)
        {
            return true;
        }

        // Deliberately bypass FEditorFileUtils::SaveDirtyPackages: even with bPromptUserToSave=false
        // it can still surface a "Save Content" picker for packages such as the currently open level,
        // which blocks an automated shutdown requested through the adapter. Saving each dirty package
        // directly via UPackage::SavePackage to its own deterministic on-disk path never shows any UI.
        for (UPackage* Package : DirtyPackages)
        {
            const FString PackageName = Package->GetName();
            const FString Extension = Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
            const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, Extension);
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            SaveArgs.SaveFlags = SAVE_NoError;

            if (UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs))
            {
                SavedPackageNames.Add(PackageName);
                // A package that reports a successful save but is still dirty has not actually been
                // written as asked; surface it rather than counting it as saved and moving on.
                if (Package->IsDirty())
                {
                    StillDirtyPackageNames.Add(PackageName);
                }
                continue;
            }

            ++FailedPackageCount;
            TSharedRef<FJsonObject> FailedItem = MakeShared<FJsonObject>();
            FailedItem->SetStringField(TEXT("packageName"), PackageName);
            FailedItem->SetStringField(TEXT("filename"), Filename);
            FailedItem->SetStringField(TEXT("reason"), DescribeSaveFailure(Filename));
            FailedPackagesJson.Add(MakeShared<FJsonValueObject>(FailedItem));
        }

        return true;
    }, ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    // A partial save is not a success: an agent keying off `success` would otherwise shut the editor
    // down believing everything reached disk.
    const bool bAllSaved = FailedPackageCount == 0 && StillDirtyPackageNames.IsEmpty();

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(bDryRun || bAllSaved);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetNumberField(TEXT("dirtyPackageCount"), DirtyPackageNames.Num());
    TArray<TSharedPtr<FJsonValue>> DirtyPackagesJson;
    for (const FString& PackageName : DirtyPackageNames)
    {
        DirtyPackagesJson.Add(MakeShared<FJsonValueString>(PackageName));
    }
    Result->SetArrayField(TEXT("dirtyPackages"), DirtyPackagesJson);
    TArray<TSharedPtr<FJsonValue>> SavedPackagesJson;
    for (const FString& PackageName : SavedPackageNames)
    {
        SavedPackagesJson.Add(MakeShared<FJsonValueString>(PackageName));
    }
    Result->SetArrayField(TEXT("savedPackages"), SavedPackagesJson);
    Result->SetArrayField(TEXT("failedPackages"), FailedPackagesJson);
    Result->SetNumberField(TEXT("failedPackageCount"), FailedPackageCount);
    Result->SetNumberField(TEXT("savedPackageCount"), SavedPackageNames.Num());

    TArray<TSharedPtr<FJsonValue>> StillDirtyJson;
    for (const FString& PackageName : StillDirtyPackageNames)
    {
        StillDirtyJson.Add(MakeShared<FJsonValueString>(PackageName));
    }
    Result->SetArrayField(TEXT("stillDirtyPackages"), StillDirtyJson);

    Result->SetBoolField(TEXT("saved"), !bDryRun && bAllSaved);
    if (!bDryRun && !bAllSaved)
    {
        Result->SetStringField(TEXT("errorCode"), TEXT("partial_save_failure"));
        Result->SetStringField(TEXT("message"), FString::Printf(
            TEXT("%d package(s) failed to save and %d reported success but are still dirty; %d saved. Do not close the editor expecting a clean state."),
            FailedPackageCount, StillDirtyPackageNames.Num(), SavedPackageNames.Num()));
    }
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FSaveAllDirtyPackagesTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("dryRun"), UnrealMCP::BlueprintEditToolUtils::BuildBoolProperty(TEXT("Report dirty packages without saving.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
