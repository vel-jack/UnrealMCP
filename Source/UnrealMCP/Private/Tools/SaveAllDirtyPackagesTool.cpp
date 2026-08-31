#include "Tools/SaveAllDirtyPackagesTool.h"
#include "Dom/JsonObject.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

FSaveAllDirtyPackagesTool::FSaveAllDirtyPackagesTool()
    : FMCPToolBase(TEXT("SaveAllDirtyPackages"), TEXT("Saves every dirty package without prompting, so a subsequent editor shutdown does not block on a save-confirmation dialog. Supports dry-run to report dirty packages without saving.")) {}

UnrealMCP::FMCPResponse FSaveAllDirtyPackagesTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const bool bDryRun = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);

    TArray<FString> DirtyPackageNames;
    TArray<FString> SavedPackageNames;
    TArray<FString> FailedPackageNames;
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
            const FString Extension = Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
            const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), Extension);
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            SaveArgs.SaveFlags = SAVE_NoError;
            if (UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs))
            {
                SavedPackageNames.Add(Package->GetName());
            }
            else
            {
                FailedPackageNames.Add(Package->GetName());
            }
        }

        return true;
    }, ExecutionError);

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
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
    TArray<TSharedPtr<FJsonValue>> FailedPackagesJson;
    for (const FString& PackageName : FailedPackageNames)
    {
        FailedPackagesJson.Add(MakeShared<FJsonValueString>(PackageName));
    }
    Result->SetArrayField(TEXT("failedPackages"), FailedPackagesJson);
    Result->SetBoolField(TEXT("saved"), !bDryRun && FailedPackageNames.Num() == 0);
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
