#include "Tools/Assets/AssetManagementTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/ObjectRedirector.h"
#include "Tools/Blueprint/Shared/BlueprintEditToolUtils.h"
#include "Tools/Blueprint/Shared/BlueprintToolUtils.h"

namespace
{
    using UnrealMCP::BlueprintEditToolUtils::BuildBoolProperty;
    using UnrealMCP::BlueprintEditToolUtils::BuildStringProperty;
    using UnrealMCP::BlueprintEditToolUtils::GetOptionalBool;

    IAssetRegistry& Registry() { return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get(); }
    IAssetTools& AssetTools() { return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get(); }

    TArray<TSharedPtr<FJsonValue>> ToJson(const TArray<FName>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FName& Name : Names) Values.Add(MakeShared<FJsonValueString>(Name.ToString()));
        return Values;
    }

    TArray<FName> ExternalReferencers(const FName Package)
    {
        TArray<FName> Referencers;
        Registry().GetReferencers(Package, Referencers);
        Referencers.Remove(Package);
        Referencers.Sort(FNameLexicalLess());
        return Referencers;
    }

    TArray<FName> DirtyPackageNames()
    {
        TArray<UPackage*> Dirty;
        FEditorFileUtils::GetDirtyContentPackages(Dirty);
        TArray<UPackage*> DirtyWorlds;
        FEditorFileUtils::GetDirtyWorldPackages(DirtyWorlds);
        Dirty.Append(DirtyWorlds);
        TArray<FName> Names;
        for (const UPackage* Package : Dirty) if (Package) Names.AddUnique(Package->GetFName());
        return Names;
    }

    // Packages that became dirty during an operation, so callers see every side effect.
    TArray<FName> NewlyDirty(const TArray<FName>& Before)
    {
        TArray<FName> Result;
        for (const FName& Name : DirtyPackageNames()) if (!Before.Contains(Name)) Result.Add(Name);
        Result.Sort(FNameLexicalLess());
        return Result;
    }

    TArray<FName> AlreadyDirty(const TArray<FName>& Packages)
    {
        TArray<FName> Result;
        for (const FName& Name : Packages)
        {
            if (const UPackage* Package = FindPackage(nullptr, *Name.ToString()); Package && Package->IsDirty()) Result.Add(Name);
        }
        return Result;
    }

    bool LoadAsset(const FString& ObjectPath, UObject*& OutAsset, FString& OutError)
    {
        OutAsset = LoadObject<UObject>(nullptr, *ObjectPath);
        if (OutAsset == nullptr || OutAsset->IsA<UObjectRedirector>())
        {
            OutError = FString::Printf(TEXT("Could not load an asset at %s."), *ObjectPath);
            return false;
        }
        return true;
    }

    bool ValidateDestination(const FString& PackagePath, const FString& Name, FString& OutObjectPath, FString& OutError)
    {
        const FString LongPackage = PackagePath / Name;
        FText Reason;
        if (!FPackageName::IsValidLongPackageName(LongPackage, false, &Reason) || !FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS))
        {
            OutError = FString::Printf(TEXT("Invalid destination %s: %s"), *LongPackage, *Reason.ToString());
            return false;
        }
        OutObjectPath = LongPackage + TEXT(".") + Name;
        if (Registry().GetAssetByObjectPath(FSoftObjectPath(OutObjectPath)).IsValid() || FindObject<UObject>(nullptr, *OutObjectPath))
        {
            OutError = FString::Printf(TEXT("An asset already exists at %s."), *OutObjectPath);
            return false;
        }
        return true;
    }

    // Repoints every referencer of the redirectors, resaves them and deletes the redirectors (editor "Fix Up Redirectors").
    // Refuses to resave a referencer that already had unsaved edits unless explicitly allowed.
    bool FixupAndReport(const TArray<UObjectRedirector*>& Redirectors, bool bAllowDirtyReferencers, TSharedRef<FJsonObject> Result, FString& OutError, const TArray<FName>* DirtyBeforeOperation = nullptr)
    {
        if (Registry().IsLoadingAssets())
        {
            OutError = TEXT("Asset discovery is still running; wait before redirector fixup.");
            return false;
        }
        TArray<FName> Referencers;
        TArray<FString> RedirectorPaths;
        for (const UObjectRedirector* Redirector : Redirectors)
        {
            RedirectorPaths.Add(Redirector->GetPathName());
            for (const FName& Name : ExternalReferencers(Redirector->GetPackage()->GetFName())) Referencers.AddUnique(Name);
        }
        TArray<FName> Dirty;
        if (DirtyBeforeOperation)
        {
            // Rename dirties referencers itself; only earlier edits require explicit save permission.
            for (const FName& Package : Referencers) if (DirtyBeforeOperation->Contains(Package)) Dirty.Add(Package);
        }
        else Dirty = AlreadyDirty(Referencers);
        Result->SetArrayField(TEXT("redirectorReferencers"), ToJson(Referencers));
        Result->SetArrayField(TEXT("dirtyReferencers"), ToJson(Dirty));
        if (!Dirty.IsEmpty() && !bAllowDirtyReferencers)
        {
            OutError = TEXT("Some referencers have unsaved edits that fixup would save; save or revert them first, or pass allowSavingDirtyReferencers=true.");
            return false;
        }
        // AssetTools::FixupReferencers opens a modal report in UE 5.8, even with
        // DeleteFixedUpRedirectors. Resave resolved imports directly for MCP.
        TArray<FName> SavedPackages, SavedReferencers, NotSaved;
        TArray<FString> SavedFiles;
        for (const FName& Name : Referencers)
        {
            UPackage* Package = LoadPackage(nullptr, *Name.ToString(), LOAD_None);
            const FString Filename = FPackageName::LongPackageNameToFilename(Name.ToString(),
                Package && Package->HasAnyPackageFlags(PKG_ContainsMap) ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            SaveArgs.SaveFlags = SAVE_NoError;
            if (Package && UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs))
            {
                SavedPackages.Add(Name);
                SavedReferencers.Add(Name);
                SavedFiles.Add(Filename);
            }
            else NotSaved.Add(Name);
        }
        if (!SavedFiles.IsEmpty()) Registry().ScanModifiedAssetFiles(SavedFiles);
        TArray<UObject*> SafeToDelete;
        for (UObjectRedirector* Redirector : Redirectors)
        {
            if (ExternalReferencers(Redirector->GetPackage()->GetFName()).IsEmpty())
                SafeToDelete.Add(Redirector);
        }
        if (!SafeToDelete.IsEmpty()) ObjectTools::DeleteObjectsUnchecked(SafeToDelete);
        Result->SetArrayField(TEXT("referencersResaved"), ToJson(SavedReferencers));
        Result->SetArrayField(TEXT("packagesSaved"), ToJson(SavedPackages));

        TArray<TSharedPtr<FJsonValue>> Remaining;
        for (const FString& Path : RedirectorPaths)
        {
            const UObjectRedirector* Left = FindObject<UObjectRedirector>(nullptr, *Path);
            const FString Filename = FPackageName::LongPackageNameToFilename(
                FPackageName::ObjectPathToPackageName(Path), FPackageName::GetAssetPackageExtension());
            if ((Left && IsValid(Left)) || IFileManager::Get().FileExists(*Filename))
                Remaining.Add(MakeShared<FJsonValueString>(Path));
        }
        Result->SetArrayField(TEXT("redirectorsRemoved"), [&]
        {
            TArray<TSharedPtr<FJsonValue>> Removed;
            for (const FString& Path : RedirectorPaths)
                if (!Remaining.ContainsByPredicate([&](const TSharedPtr<FJsonValue>& V) { return V->AsString() == Path; }))
                    Removed.Add(MakeShared<FJsonValueString>(Path));
            return Removed;
        }());
        Result->SetArrayField(TEXT("redirectorsRemaining"), Remaining);
        Result->SetArrayField(TEXT("referencersNotSaved"), ToJson(NotSaved));
        const bool bComplete = Remaining.IsEmpty() && NotSaved.IsEmpty();
        Result->SetBoolField(TEXT("success"), bComplete);
        Result->SetBoolField(TEXT("partialSuccess"), !bComplete);
        if (!bComplete) OutError = TEXT("Redirector fixup was incomplete; inspect redirectorsRemaining and referencersNotSaved (checkout, load or save may have failed). Changes already applied were not rolled back.");
        return bComplete;
    }

    UnrealMCP::FMCPResponse Respond(const UnrealMCP::FMCPRequest& Request, const TSharedRef<FJsonObject>& Result)
    {
        UnrealMCP::FMCPResponse Response;
        Response.Id = Request.Id;
        Response.Result = Result;
        return Response;
    }

    TSharedRef<FJsonObject> Schema(TSharedRef<FJsonObject> Properties, std::initializer_list<const TCHAR*> Required)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("type"), TEXT("object"));
        Root->SetObjectField(TEXT("properties"), Properties);
        TArray<TSharedPtr<FJsonValue>> RequiredValues;
        for (const TCHAR* Name : Required) RequiredValues.Add(MakeShared<FJsonValueString>(Name));
        Root->SetArrayField(TEXT("required"), RequiredValues);
        return Root;
    }
}

// ---------------------------------------------------------------- MoveAsset

FMoveAssetTool::FMoveAssetTool()
    : FMCPToolBase(TEXT("MoveAsset"),
        TEXT("Moves and/or renames an asset (IAssetTools::RenameAssets), then by default fixes up the redirector so referencers are resaved at the new path and no redirector is left. Dry-run lists referencers and changes nothing. Reports newly dirtied packages and referencers that could not be saved."))
{
}

UnrealMCP::FMCPResponse FMoveAssetTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString SourcePath, DestinationPath, NewName;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("sourceObjectPath"), SourcePath) || SourcePath.IsEmpty()
        || !Request.Params->TryGetStringField(TEXT("destinationPackagePath"), DestinationPath) || DestinationPath.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("MoveAsset requires sourceObjectPath and destinationPackagePath."));
    }
    Request.Params->TryGetStringField(TEXT("newName"), NewName);
    DestinationPath.RemoveFromEnd(TEXT("/"));
    const bool bDryRun = GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bConfirm = GetOptionalBool(Request.Params, TEXT("confirm"), false);
    const bool bFixup = GetOptionalBool(Request.Params, TEXT("fixupRedirectors"), true);
    const bool bSave = GetOptionalBool(Request.Params, TEXT("save"), false);
    const bool bAllowDirty = GetOptionalBool(Request.Params, TEXT("allowSavingDirtyReferencers"), false);
    if (!bDryRun && !bConfirm)
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("MoveAsset requires confirm=true unless dryRun=true."));

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    FString ExecutionError;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UObject* Asset = nullptr;
        if (!LoadAsset(SourcePath, Asset, OutError)) return false;
        const FString OldObjectPath = Asset->GetPathName();
        const FString FinalName = NewName.IsEmpty() ? Asset->GetName() : NewName;
        FString NewObjectPath;
        if (!ValidateDestination(DestinationPath, FinalName, NewObjectPath, OutError)) return false;

        const TArray<FName> Referencers = ExternalReferencers(Asset->GetPackage()->GetFName());
        Result->SetStringField(TEXT("sourceObjectPath"), OldObjectPath);
        Result->SetStringField(TEXT("newObjectPath"), NewObjectPath);
        Result->SetArrayField(TEXT("referencers"), ToJson(Referencers));
        Result->SetArrayField(TEXT("dirtyReferencers"), ToJson(AlreadyDirty(Referencers)));
        Result->SetBoolField(TEXT("dryRun"), bDryRun);
        if (bDryRun) return true;
        if (bFixup && !bAllowDirty && !AlreadyDirty(Referencers).IsEmpty())
        {
            OutError = TEXT("Some referencers have unsaved edits that redirector fixup would save; save or revert them first, or pass allowSavingDirtyReferencers=true.");
            return false;
        }

        if (bFixup && Registry().IsLoadingAssets())
        {
            OutError = TEXT("Asset discovery is still running; wait before moving with redirector fixup.");
            return false;
        }
        const TArray<FName> DirtyBefore = DirtyPackageNames();
        if (!AssetTools().RenameAssets({FAssetRenameData(Asset, DestinationPath, FinalName)}))
        {
            OutError = TEXT("IAssetTools::RenameAssets failed; see the Unreal log for the reason (locked file, read-only package, or name clash).");
            return false;
        }
        UObject* Moved = FindObject<UObject>(nullptr, *NewObjectPath);
        Result->SetBoolField(TEXT("moved"), Moved != nullptr);
        UObjectRedirector* Redirector = FindObject<UObjectRedirector>(nullptr, *OldObjectPath);
        Result->SetBoolField(TEXT("redirectorCreated"), Redirector != nullptr);
        if (bFixup && Redirector && !FixupAndReport({Redirector}, bAllowDirty, Result, OutError, &DirtyBefore))
        {
            Result->SetArrayField(TEXT("newlyDirtiedPackages"), ToJson(NewlyDirty(DirtyBefore)));
            return false;
        }

        if (bSave && Moved)
        {
            FString Filename, IndexError;
            if (!UnrealMCP::BlueprintEditToolUtils::SaveAsset(Moved, Filename, OutError)) return false;
            Result->SetStringField(TEXT("savedFilename"), Filename);
            UnrealMCP::BlueprintEditToolUtils::RefreshAssetIndex(NewObjectPath, IndexError);
        }
        Result->SetBoolField(TEXT("saved"), bSave && Moved);
        Result->SetArrayField(TEXT("newlyDirtiedPackages"), ToJson(NewlyDirty(DirtyBefore)));
        Result->SetBoolField(TEXT("success"), Moved != nullptr);
        return true;
    }, ExecutionError);
    return bOk ? Respond(Request, Result) : BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecutionError, Result);
}

TSharedPtr<FJsonObject> FMoveAssetTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("sourceObjectPath"), BuildStringProperty(TEXT("Asset to move, e.g. /Game/A/BP_X.BP_X.")));
    P->SetObjectField(TEXT("destinationPackagePath"), BuildStringProperty(TEXT("Destination folder, e.g. /Game/B/Weapons.")));
    P->SetObjectField(TEXT("newName"), BuildStringProperty(TEXT("Optional new asset name; keeps the current name when empty.")));
    P->SetObjectField(TEXT("fixupRedirectors"), BuildBoolProperty(TEXT("Resave referencers and delete the redirector. Default true.")));
    P->SetObjectField(TEXT("allowSavingDirtyReferencers"), BuildBoolProperty(TEXT("Allow fixup to save referencers that already had unsaved edits.")));
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("List referencers and validate the destination only.")));
    P->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required to apply.")));
    P->SetObjectField(TEXT("save"), BuildBoolProperty(TEXT("Save the moved asset.")));
    return Schema(P, {TEXT("sourceObjectPath"), TEXT("destinationPackagePath")});
}

// ---------------------------------------------------------------- FixupRedirectors

FFixupRedirectorsTool::FFixupRedirectorsTool()
    : FMCPToolBase(TEXT("FixupRedirectors"),
        TEXT("Editor 'Fix Up Redirectors in Folder': finds redirectors under packagePath (recursive by default), resaves their referencers and deletes them. Dry-run lists redirectors and referencers only."))
{
}

UnrealMCP::FMCPResponse FFixupRedirectorsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString PackagePath;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("packagePath"), PackagePath) || PackagePath.IsEmpty())
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FixupRedirectors requires packagePath."));
    PackagePath.RemoveFromEnd(TEXT("/"));
    const bool bDryRun = GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bConfirm = GetOptionalBool(Request.Params, TEXT("confirm"), false);
    const bool bRecursive = GetOptionalBool(Request.Params, TEXT("recursive"), true);
    const bool bAllowDirty = GetOptionalBool(Request.Params, TEXT("allowSavingDirtyReferencers"), false);
    if (!bDryRun && !bConfirm)
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("FixupRedirectors requires confirm=true unless dryRun=true."));

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    FString ExecutionError;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*PackagePath));
        Filter.bRecursivePaths = bRecursive;
        Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
        TArray<FAssetData> Found;
        Registry().GetAssets(Filter, Found);

        TArray<TSharedPtr<FJsonValue>> Listed;
        TArray<FName> Referencers;
        for (const FAssetData& Data : Found)
        {
            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("redirector"), Data.GetObjectPathString());
            Entry->SetArrayField(TEXT("referencers"), ToJson(ExternalReferencers(Data.PackageName)));
            Listed.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Result->SetStringField(TEXT("packagePath"), PackagePath);
        Result->SetArrayField(TEXT("redirectors"), Listed);
        Result->SetBoolField(TEXT("dryRun"), bDryRun);
        if (bDryRun || Found.IsEmpty()) return true;

        TArray<UObjectRedirector*> Redirectors;
        for (const FAssetData& Data : Found)
            if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Data.GetAsset())) Redirectors.Add(Redirector);
        const TArray<FName> DirtyBefore = DirtyPackageNames();
        const bool bFixed = FixupAndReport(Redirectors, bAllowDirty, Result, OutError);
        Result->SetArrayField(TEXT("newlyDirtiedPackages"), ToJson(NewlyDirty(DirtyBefore)));
        if (!bFixed) return false;
        Result->SetArrayField(TEXT("newlyDirtiedPackages"), ToJson(NewlyDirty(DirtyBefore)));
        return true;
    }, ExecutionError);
    return bOk ? Respond(Request, Result) : BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecutionError, Result);
}

TSharedPtr<FJsonObject> FFixupRedirectorsTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("packagePath"), BuildStringProperty(TEXT("Folder to scan, e.g. /Game/MarkerBP.")));
    P->SetObjectField(TEXT("recursive"), BuildBoolProperty(TEXT("Include subfolders. Default true.")));
    P->SetObjectField(TEXT("allowSavingDirtyReferencers"), BuildBoolProperty(TEXT("Allow saving referencers that already had unsaved edits.")));
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("List redirectors and referencers only.")));
    P->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required to apply.")));
    return Schema(P, {TEXT("packagePath")});
}

// ---------------------------------------------------------------- DuplicateAsset

FDuplicateAssetTool::FDuplicateAssetTool()
    : FMCPToolBase(TEXT("DuplicateAsset"),
        TEXT("Duplicates an asset to destinationPackagePath/newName (IAssetTools::DuplicateAsset), e.g. to keep the original for comparison. Refuses to overwrite."))
{
}

UnrealMCP::FMCPResponse FDuplicateAssetTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString SourcePath, DestinationPath, NewName;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("sourceObjectPath"), SourcePath) || SourcePath.IsEmpty()
        || !Request.Params->TryGetStringField(TEXT("destinationPackagePath"), DestinationPath) || DestinationPath.IsEmpty()
        || !Request.Params->TryGetStringField(TEXT("newName"), NewName) || NewName.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("DuplicateAsset requires sourceObjectPath, destinationPackagePath and newName."));
    }
    DestinationPath.RemoveFromEnd(TEXT("/"));
    const bool bDryRun = GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bSave = GetOptionalBool(Request.Params, TEXT("save"), false);

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    FString ExecutionError;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UObject* Source = nullptr;
        FString NewObjectPath;
        if (!LoadAsset(SourcePath, Source, OutError) || !ValidateDestination(DestinationPath, NewName, NewObjectPath, OutError)) return false;
        Result->SetStringField(TEXT("sourceObjectPath"), Source->GetPathName());
        Result->SetStringField(TEXT("newObjectPath"), NewObjectPath);
        Result->SetBoolField(TEXT("dryRun"), bDryRun);
        if (bDryRun) return true;

        const TArray<FName> DirtyBefore = DirtyPackageNames();
        UObject* Copy = AssetTools().DuplicateAsset(NewName, DestinationPath, Source);
        if (Copy == nullptr)
        {
            OutError = TEXT("IAssetTools::DuplicateAsset failed; see the Unreal log.");
            return false;
        }
        if (bSave)
        {
            FString Filename, IndexError;
            if (!UnrealMCP::BlueprintEditToolUtils::SaveAsset(Copy, Filename, OutError)) return false;
            Result->SetStringField(TEXT("savedFilename"), Filename);
            UnrealMCP::BlueprintEditToolUtils::RefreshAssetIndex(NewObjectPath, IndexError);
        }
        Result->SetBoolField(TEXT("duplicated"), true);
        Result->SetBoolField(TEXT("saved"), bSave);
        Result->SetArrayField(TEXT("newlyDirtiedPackages"), ToJson(NewlyDirty(DirtyBefore)));
        return true;
    }, ExecutionError);
    return bOk ? Respond(Request, Result) : BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecutionError, Result);
}

TSharedPtr<FJsonObject> FDuplicateAssetTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("sourceObjectPath"), BuildStringProperty(TEXT("Asset to copy.")));
    P->SetObjectField(TEXT("destinationPackagePath"), BuildStringProperty(TEXT("Destination folder.")));
    P->SetObjectField(TEXT("newName"), BuildStringProperty(TEXT("Name of the copy.")));
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Validate only.")));
    P->SetObjectField(TEXT("save"), BuildBoolProperty(TEXT("Save the copy.")));
    return Schema(P, {TEXT("sourceObjectPath"), TEXT("destinationPackagePath"), TEXT("newName")});
}

// ---------------------------------------------------------------- DeleteAsset

FDeleteAssetTool::FDeleteAssetTool()
    : FMCPToolBase(TEXT("DeleteAsset"),
        TEXT("Plainly deletes one asset (no reference replacement; see ConsolidateAndDeleteAsset for that). Refuses while indexed referencers exist unless force=true. Requires confirm=true unless dryRun=true."))
{
}

UnrealMCP::FMCPResponse FDeleteAssetTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("DeleteAsset requires objectPath."));
    const bool bDryRun = GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bConfirm = GetOptionalBool(Request.Params, TEXT("confirm"), false);
    const bool bRequireNoReferencers = GetOptionalBool(Request.Params, TEXT("requireNoReferencers"), true);
    const bool bForce = GetOptionalBool(Request.Params, TEXT("force"), false);
    if (!bDryRun && !bConfirm)
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("DeleteAsset requires confirm=true unless dryRun=true."));

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    FString ExecutionError;
    const bool bOk = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UObject* Asset = nullptr;
        if (!LoadAsset(ObjectPath, Asset, OutError)) return false;
        const FName PackageName = Asset->GetPackage()->GetFName();
        FString PackageFilename;
        const bool bHadPackageFile = FPackageName::DoesPackageExist(PackageName.ToString(), &PackageFilename);
        const TArray<FName> Referencers = ExternalReferencers(PackageName);
        Result->SetStringField(TEXT("packageFile"), PackageFilename);
        Result->SetBoolField(TEXT("packageFileExisted"), bHadPackageFile);
        Result->SetStringField(TEXT("objectPath"), Asset->GetPathName());
        Result->SetArrayField(TEXT("referencers"), ToJson(Referencers));
        Result->SetBoolField(TEXT("dryRun"), bDryRun);
        if (!Referencers.IsEmpty() && bRequireNoReferencers && !bForce)
        {
            OutError = FString::Printf(TEXT("%d package(s) still reference this asset; repoint them (MoveAsset/ConsolidateAndDeleteAsset) or pass force=true."), Referencers.Num());
            return false;
        }
        if (bDryRun) return true;

        const TArray<FName> DirtyBefore = DirtyPackageNames();
        const int32 Deleted = ObjectTools::ForceDeleteObjects({Asset}, false);
        Result->SetBoolField(TEXT("deleted"), Deleted > 0);
        const bool bFileRemains = bHadPackageFile && IFileManager::Get().FileExists(*PackageFilename);
        Result->SetBoolField(TEXT("packageFileRemains"), bFileRemains);
        Result->SetBoolField(TEXT("success"), Deleted > 0 && !bFileRemains);
        Result->SetArrayField(TEXT("newlyDirtiedPackages"), ToJson(NewlyDirty(DirtyBefore)));
        if (bFileRemains)
        {
            Result->SetBoolField(TEXT("packageFileReadOnly"), IFileManager::Get().IsReadOnly(*PackageFilename));
            OutError = FString::Printf(TEXT("Package file remains after deletion attempt: %s. Check remaining exports, file access and source-control status."), *PackageFilename);
            return false;
        }
        if (Deleted <= 0)
        {
            OutError = TEXT("Unreal did not delete the asset. See the editor log for the deletion failure.");
            return false;
        }
        return true;
    }, ExecutionError);
    return bOk ? Respond(Request, Result) : BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, ExecutionError, Result);
}

TSharedPtr<FJsonObject> FDeleteAssetTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetObjectField(TEXT("objectPath"), BuildStringProperty(TEXT("Asset to delete.")));
    P->SetObjectField(TEXT("requireNoReferencers"), BuildBoolProperty(TEXT("Refuse while referencers exist. Default true.")));
    P->SetObjectField(TEXT("force"), BuildBoolProperty(TEXT("Delete even with referencers; their references become null.")));
    P->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("List referencers only.")));
    P->SetObjectField(TEXT("confirm"), BuildBoolProperty(TEXT("Required to apply.")));
    return Schema(P, {TEXT("objectPath")});
}
