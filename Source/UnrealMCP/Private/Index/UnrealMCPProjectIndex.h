#pragma once

#include "CoreMinimal.h"
#include "SQLiteDatabase.h"
#include "Templates/Function.h"

class IAssetRegistry;

class FUnrealMCPProjectIndex final
{
public:
    struct FStatusSnapshot
    {
        bool bDatabaseOpen = false;
        bool bSchemaReady = false;
        bool bAssetRegistryLoaded = false;
        bool bLiveTrackingEnabled = false;
        bool bHasUsableIndex = false;
        bool bIndexDirty = true;
        int32 SchemaVersion = 1;
        int32 DirtyAssetCount = 0;
        int64 IndexedAssetCount = 0;
        int64 IndexedBlueprintCount = 0;
        int64 IndexedProjectAssetCount = 0;
        int64 IndexedProjectBlueprintCount = 0;
        int64 IndexedEngineAssetCount = 0;
        int64 IndexedEngineBlueprintCount = 0;
        int64 IndexedPluginAssetCount = 0;
        int64 IndexedPluginBlueprintCount = 0;
        FString IndexedScope = TEXT("project");
        FString DatabasePath;
        FString LastFullBuildUtc;
        FString LastUpdateUtc;
        FString LastError;
        bool bManualRebuildPreferred = true;
        bool bRebuildRecommended = false;
        FString RebuildCadenceHint = TEXT("manual_daily");
        TArray<FString> DirtyAssets;
    };

    FUnrealMCPProjectIndex();
    ~FUnrealMCPProjectIndex();

    bool Initialize();
    void Shutdown();

    FStatusSnapshot GetStatusSnapshot() const;
    bool BuildFullIndex(FString& OutError, const TFunction<void(int32, int32, const FString&)>& ProgressCallback = {});
    FSQLiteDatabase& GetDatabase();
    const FSQLiteDatabase& GetDatabase() const;

private:
    static constexpr int32 CurrentSchemaVersion = 1;

    bool OpenDatabase(FString& OutError);
    bool EnsureSchema(FString& OutError);
    bool RecalculateIndexedCounts(FString* OutError = nullptr);
    bool UpsertAsset(const FAssetData& AssetData, FString* OutError = nullptr);
    bool RemoveAsset(const FString& ObjectPath, const FString& PackageName, FString* OutError = nullptr);

    void RegisterAssetRegistryDelegates();
    void UnregisterAssetRegistryDelegates();

    void HandleFilesLoaded();
    void HandleAssetAdded(const FAssetData& AssetData);
    void HandleAssetRemoved(const FAssetData& AssetData);
    void HandleAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath);
    void HandleAssetUpdated(const FAssetData& AssetData);
    void HandleBlueprintCompiled();

    void MarkIndexDirty(const FString& ObjectPath = FString());
    void MarkIndexClean();

    bool SetMetadataValue(const FString& Key, const FString& Value, FString* OutError = nullptr);
    FString GetMetadataValue(const FString& Key) const;
    FString GetLastDatabaseError() const;

    static FString GetDatabasePath();
    static FString ToUtcString(const FDateTime& Value);
    static bool IsUtcDateToday(const FString& Iso8601Utc);
    static bool IsBlueprintAsset(const FAssetData& AssetData);
    static FString GetContentScope(const FAssetData& AssetData);
    static bool ShouldIndexAsset(const FAssetData& AssetData);
    static bool QueryAssetRegistryLoaded();

    FSQLiteDatabase Database;

    bool bInitialized = false;
    bool bSchemaReady = false;
    bool bAssetRegistryLoaded = false;
    bool bHasUsableIndex = false;
    bool bIndexDirty = true;

    int64 IndexedAssetCount = 0;
    int64 IndexedBlueprintCount = 0;
    int64 IndexedProjectAssetCount = 0;
    int64 IndexedProjectBlueprintCount = 0;
    int64 IndexedEngineAssetCount = 0;
    int64 IndexedEngineBlueprintCount = 0;
    int64 IndexedPluginAssetCount = 0;
    int64 IndexedPluginBlueprintCount = 0;

    FString DatabasePath;
    FString LastFullBuildUtc;
    FString LastUpdateUtc;
    FString LastError;

    TSet<FString> DirtyAssetSet;

    FDelegateHandle FilesLoadedHandle;
    FDelegateHandle AssetAddedHandle;
    FDelegateHandle AssetRemovedHandle;
    FDelegateHandle AssetRenamedHandle;
    FDelegateHandle AssetUpdatedHandle;
    FDelegateHandle BlueprintCompiledHandle;
};
