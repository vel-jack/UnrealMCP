#include "Index/UnrealMCPProjectIndex.h"

#include "Editor.h"
#include "UnrealMCPLog.h"
#include "UnrealMCPSettings.h"

FUnrealMCPProjectIndex::FUnrealMCPProjectIndex()
    : DatabasePath(GetDatabasePath())
{
}

FUnrealMCPProjectIndex::~FUnrealMCPProjectIndex()
{
    Shutdown();
}

bool FUnrealMCPProjectIndex::Initialize()
{
    if (bInitialized)
    {
        return true;
    }

    FString Error;
    if (!OpenDatabase(Error) || !EnsureSchema(Error) || !RecalculateIndexedCounts(&Error))
    {
        LastError = Error;
        UE_LOG(LogUnrealMCP, Error, TEXT("Project index initialization failed: %s"), *Error);
        return false;
    }

    LastFullBuildUtc = GetMetadataValue(TEXT("last_full_build_utc"));
    LastUpdateUtc = GetMetadataValue(TEXT("last_update_utc"));
    bHasUsableIndex = IndexedAssetCount > 0 || !LastFullBuildUtc.IsEmpty();
    bIndexDirty = GetMetadataValue(TEXT("is_dirty")) != TEXT("0");
    bAssetRegistryLoaded = QueryAssetRegistryLoaded();
    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();
    if (Settings->bEnableLiveIndexTracking)
    {
        RegisterAssetRegistryDelegates();
    }
    else
    {
        UE_LOG(LogUnrealMCP, Log, TEXT("Project index live tracking is disabled by settings."));
    }

    if (Settings->bEnableLiveIndexTracking && GEditor != nullptr)
    {
        BlueprintCompiledHandle = GEditor->OnBlueprintCompiled().AddRaw(this, &FUnrealMCPProjectIndex::HandleBlueprintCompiled);
    }

    bInitialized = true;
    UE_LOG(LogUnrealMCP, Log, TEXT("Project index initialized. Database=%s Assets=%lld Blueprints=%lld LiveTracking=%s"),
        *DatabasePath,
        IndexedAssetCount,
        IndexedBlueprintCount,
        Settings->bEnableLiveIndexTracking ? TEXT("true") : TEXT("false"));
    return true;
}

void FUnrealMCPProjectIndex::Shutdown()
{
    if (!bInitialized && !Database.IsValid())
    {
        return;
    }

    if (GEditor != nullptr && BlueprintCompiledHandle.IsValid())
    {
        GEditor->OnBlueprintCompiled().Remove(BlueprintCompiledHandle);
        BlueprintCompiledHandle.Reset();
    }

    UnregisterAssetRegistryDelegates();
    Database.Close();

    bInitialized = false;
    bSchemaReady = false;
    bAssetRegistryLoaded = false;
    bHasUsableIndex = false;
}

FUnrealMCPProjectIndex::FStatusSnapshot FUnrealMCPProjectIndex::GetStatusSnapshot() const
{
    FStatusSnapshot Snapshot;
    Snapshot.bDatabaseOpen = Database.IsValid();
    Snapshot.bSchemaReady = bSchemaReady;
    Snapshot.bAssetRegistryLoaded = QueryAssetRegistryLoaded();
    Snapshot.bLiveTrackingEnabled = GetDefault<UUnrealMCPSettings>()->bEnableLiveIndexTracking;
    Snapshot.bHasUsableIndex = bHasUsableIndex;
    Snapshot.bIndexDirty = bIndexDirty;
    Snapshot.SchemaVersion = CurrentSchemaVersion;
    Snapshot.DirtyAssetCount = DirtyAssetSet.Num();
    Snapshot.IndexedAssetCount = IndexedAssetCount;
    Snapshot.IndexedBlueprintCount = IndexedBlueprintCount;
    Snapshot.IndexedProjectAssetCount = IndexedProjectAssetCount;
    Snapshot.IndexedProjectBlueprintCount = IndexedProjectBlueprintCount;
    Snapshot.IndexedEngineAssetCount = IndexedEngineAssetCount;
    Snapshot.IndexedEngineBlueprintCount = IndexedEngineBlueprintCount;
    Snapshot.IndexedPluginAssetCount = IndexedPluginAssetCount;
    Snapshot.IndexedPluginBlueprintCount = IndexedPluginBlueprintCount;
    Snapshot.DatabasePath = DatabasePath;
    Snapshot.LastFullBuildUtc = LastFullBuildUtc;
    Snapshot.LastUpdateUtc = LastUpdateUtc;
    Snapshot.LastError = LastError;
    Snapshot.bManualRebuildPreferred = true;
    Snapshot.RebuildCadenceHint = TEXT("manual_daily");
    Snapshot.bRebuildRecommended = !Snapshot.bHasUsableIndex || (Snapshot.bIndexDirty && !IsUtcDateToday(Snapshot.LastFullBuildUtc));

    for (const FString& ObjectPath : DirtyAssetSet)
    {
        Snapshot.DirtyAssets.Add(ObjectPath);
        if (Snapshot.DirtyAssets.Num() >= 25)
        {
            break;
        }
    }
    Snapshot.DirtyAssets.Sort();
    return Snapshot;
}

FSQLiteDatabase& FUnrealMCPProjectIndex::GetDatabase()
{
    return Database;
}

const FSQLiteDatabase& FUnrealMCPProjectIndex::GetDatabase() const
{
    return Database;
}
