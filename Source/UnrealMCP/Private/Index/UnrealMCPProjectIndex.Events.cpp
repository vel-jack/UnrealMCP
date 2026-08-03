#include "Index/UnrealMCPProjectIndex.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/BlueprintSupport.h"
#include "Index/UnrealMCPProjectIndexInternal.h"
#include "Misc/PackageName.h"

void FUnrealMCPProjectIndex::RegisterAssetRegistryDelegates()
{
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    FilesLoadedHandle = AssetRegistry.OnFilesLoaded().AddRaw(this, &FUnrealMCPProjectIndex::HandleFilesLoaded);
    AssetAddedHandle = AssetRegistry.OnAssetAdded().AddRaw(this, &FUnrealMCPProjectIndex::HandleAssetAdded);
    AssetRemovedHandle = AssetRegistry.OnAssetRemoved().AddRaw(this, &FUnrealMCPProjectIndex::HandleAssetRemoved);
    AssetRenamedHandle = AssetRegistry.OnAssetRenamed().AddRaw(this, &FUnrealMCPProjectIndex::HandleAssetRenamed);
    AssetUpdatedHandle = AssetRegistry.OnAssetUpdated().AddRaw(this, &FUnrealMCPProjectIndex::HandleAssetUpdated);

    bAssetRegistryLoaded = QueryAssetRegistryLoaded();
}

void FUnrealMCPProjectIndex::UnregisterAssetRegistryDelegates()
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
    {
        return;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    if (FilesLoadedHandle.IsValid())
    {
        AssetRegistry.OnFilesLoaded().Remove(FilesLoadedHandle);
        FilesLoadedHandle.Reset();
    }

    if (AssetAddedHandle.IsValid())
    {
        AssetRegistry.OnAssetAdded().Remove(AssetAddedHandle);
        AssetAddedHandle.Reset();
    }

    if (AssetRemovedHandle.IsValid())
    {
        AssetRegistry.OnAssetRemoved().Remove(AssetRemovedHandle);
        AssetRemovedHandle.Reset();
    }

    if (AssetRenamedHandle.IsValid())
    {
        AssetRegistry.OnAssetRenamed().Remove(AssetRenamedHandle);
        AssetRenamedHandle.Reset();
    }

    if (AssetUpdatedHandle.IsValid())
    {
        AssetRegistry.OnAssetUpdated().Remove(AssetUpdatedHandle);
        AssetUpdatedHandle.Reset();
    }
}

void FUnrealMCPProjectIndex::HandleFilesLoaded()
{
    bAssetRegistryLoaded = true;
}

bool FUnrealMCPProjectIndex::QueryAssetRegistryLoaded()
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
    {
        return false;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    return !AssetRegistryModule.Get().IsLoadingAssets();
}

void FUnrealMCPProjectIndex::HandleAssetAdded(const FAssetData& AssetData)
{
    if (!ShouldIndexAsset(AssetData))
    {
        return;
    }

    MarkIndexDirty(AssetData.GetObjectPathString());

    if (!bHasUsableIndex)
    {
        return;
    }

    FString Error;
    if (UpsertAsset(AssetData, &Error) && RecalculateIndexedCounts(&Error))
    {
        DirtyAssetSet.Remove(AssetData.GetObjectPathString());
        bIndexDirty = DirtyAssetSet.Num() > 0;
        SetMetadataValue(TEXT("is_dirty"), bIndexDirty ? TEXT("1") : TEXT("0"), nullptr);
        LastError.Reset();
        return;
    }

    LastError = Error;
}

void FUnrealMCPProjectIndex::HandleAssetRemoved(const FAssetData& AssetData)
{
    if (!ShouldIndexAsset(AssetData))
    {
        return;
    }

    MarkIndexDirty(AssetData.GetObjectPathString());

    if (!bHasUsableIndex)
    {
        return;
    }

    FString Error;
    if (RemoveAsset(AssetData.GetObjectPathString(), AssetData.PackageName.ToString(), &Error) && RecalculateIndexedCounts(&Error))
    {
        DirtyAssetSet.Remove(AssetData.GetObjectPathString());
        bIndexDirty = DirtyAssetSet.Num() > 0;
        SetMetadataValue(TEXT("is_dirty"), bIndexDirty ? TEXT("1") : TEXT("0"), nullptr);
        LastError.Reset();
        return;
    }

    LastError = Error;
}

void FUnrealMCPProjectIndex::HandleAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath)
{
    if (!ShouldIndexAsset(AssetData))
    {
        return;
    }

    MarkIndexDirty(OldObjectPath);
    MarkIndexDirty(AssetData.GetObjectPathString());

    if (!bHasUsableIndex)
    {
        return;
    }

    const FString OldPackageName = FPackageName::ObjectPathToPackageName(OldObjectPath);

    FString Error;
    const bool bRemovedOld = RemoveAsset(OldObjectPath, OldPackageName, &Error);
    const bool bInsertedNew = bRemovedOld && UpsertAsset(AssetData, &Error);
    if (bInsertedNew && RecalculateIndexedCounts(&Error))
    {
        DirtyAssetSet.Remove(OldObjectPath);
        DirtyAssetSet.Remove(AssetData.GetObjectPathString());
        bIndexDirty = DirtyAssetSet.Num() > 0;
        SetMetadataValue(TEXT("is_dirty"), bIndexDirty ? TEXT("1") : TEXT("0"), nullptr);
        LastError.Reset();
        return;
    }

    LastError = Error;
}

void FUnrealMCPProjectIndex::HandleAssetUpdated(const FAssetData& AssetData)
{
    HandleAssetAdded(AssetData);
}

void FUnrealMCPProjectIndex::HandleBlueprintCompiled()
{
    MarkIndexDirty();
}

void FUnrealMCPProjectIndex::MarkIndexDirty(const FString& ObjectPath)
{
    bIndexDirty = true;
    if (!ObjectPath.IsEmpty())
    {
        DirtyAssetSet.Add(ObjectPath);
    }

    SetMetadataValue(TEXT("is_dirty"), TEXT("1"), nullptr);
}

void FUnrealMCPProjectIndex::MarkIndexClean()
{
    bIndexDirty = false;
    DirtyAssetSet.Reset();
    SetMetadataValue(TEXT("is_dirty"), TEXT("0"), nullptr);
}

bool FUnrealMCPProjectIndex::SetMetadataValue(const FString& Key, const FString& Value, FString* OutError)
{
    return ExecuteBoundStatement(Database,
        TEXT("INSERT INTO metadata(key, value) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value;"),
        [&Key, &Value](FSQLitePreparedStatement& Statement)
        {
            return Statement.SetBindingValueByIndex(1, Key)
                && Statement.SetBindingValueByIndex(2, Value);
        },
        OutError);
}

FString FUnrealMCPProjectIndex::GetMetadataValue(const FString& Key) const
{
    if (!Database.IsValid())
    {
        return FString();
    }

    FString Value;
    FSQLitePreparedStatement Statement(const_cast<FSQLiteDatabase&>(Database),
        TEXT("SELECT value FROM metadata WHERE key = ?1;"),
        ESQLitePreparedStatementFlags::None);
    if (!Statement.IsValid())
    {
        return FString();
    }

    if (!Statement.SetBindingValueByIndex(1, Key))
    {
        return FString();
    }

    const int64 Result = Statement.Execute([&Value](const FSQLitePreparedStatement& Row)
    {
        return Row.GetColumnValueByIndex(0, Value)
            ? ESQLitePreparedStatementExecuteRowResult::Continue
            : ESQLitePreparedStatementExecuteRowResult::Error;
    });

    return Result > 0 ? Value : FString();
}

FString FUnrealMCPProjectIndex::GetLastDatabaseError() const
{
    return Database.IsValid() ? Database.GetLastError() : FString();
}

FString FUnrealMCPProjectIndex::GetDatabasePath()
{
    return FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMCP"), TEXT("ProjectIndex.sqlite3")));
}

FString FUnrealMCPProjectIndex::ToUtcString(const FDateTime& Value)
{
    return Value.ToIso8601();
}

bool FUnrealMCPProjectIndex::IsUtcDateToday(const FString& Iso8601Utc)
{
    if (Iso8601Utc.IsEmpty())
    {
        return false;
    }

    FDateTime Parsed;
    if (!FDateTime::ParseIso8601(*Iso8601Utc, Parsed))
    {
        return false;
    }

    const FDateTime TodayUtc = FDateTime::UtcNow().GetDate();
    return Parsed.GetDate() == TodayUtc;
}

bool FUnrealMCPProjectIndex::IsBlueprintAsset(const FAssetData& AssetData)
{
    return AssetData.AssetClassPath.ToString().Contains(TEXT("Blueprint"));
}

FString FUnrealMCPProjectIndex::GetContentScope(const FAssetData& AssetData)
{
    const FString PackagePath = AssetData.PackagePath.ToString();
    if (PackagePath.StartsWith(TEXT("/Game")))
    {
        return TEXT("project");
    }

    if (PackagePath.StartsWith(TEXT("/Engine")))
    {
        return TEXT("engine");
    }

    return TEXT("plugin");
}

bool FUnrealMCPProjectIndex::ShouldIndexAsset(const FAssetData& AssetData)
{
    return GetContentScope(AssetData) == TEXT("project");
}
