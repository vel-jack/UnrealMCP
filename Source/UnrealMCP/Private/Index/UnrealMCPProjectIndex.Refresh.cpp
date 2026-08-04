#include "Index/UnrealMCPProjectIndex.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Index/UnrealMCPProjectIndexInternal.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UnrealMCPLog.h"

namespace
{
    struct FIndexedAssetStamp
    {
        FString PackageName;
        FDateTime IndexedAtUtc;
    };

    bool IsPackageNewerThanIndex(const FString& PackageName, const FDateTime& IndexedAtUtc)
    {
        if (UPackage* LoadedPackage = FindPackage(nullptr, *PackageName))
        {
            if (LoadedPackage->IsDirty())
            {
                return true;
            }
        }

        FString PackageFilename;
        if (!FPackageName::DoesPackageExist(PackageName, &PackageFilename))
        {
            return false;
        }

        const FDateTime PackageTimestamp = IFileManager::Get().GetTimeStamp(*PackageFilename);
        return PackageTimestamp != FDateTime::MinValue() && PackageTimestamp > IndexedAtUtc + FTimespan::FromSeconds(1.0);
    }
}

bool FUnrealMCPProjectIndex::RefreshProjectIndex(
    const TArray<FString>& RequestedObjectPaths,
    FRefreshResult& OutResult,
    FString& OutError,
    const TFunction<void(int32, int32, const FString&)>& ProgressCallback)
{
    OutResult = FRefreshResult();
    if (!Database.IsValid())
    {
        OutError = TEXT("Project index database is not open.");
        return false;
    }
    if (!bHasUsableIndex)
    {
        OutError = TEXT("Project index has not been built yet. Run BuildProjectIndex first.");
        return false;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    TArray<FAssetData> RegistryAssets;
    AssetRegistry.GetAllAssets(RegistryAssets, true);
    TMap<FString, FAssetData> CurrentAssetsByPath;
    for (const FAssetData& AssetData : RegistryAssets)
    {
        if (ShouldIndexAsset(AssetData))
        {
            CurrentAssetsByPath.Add(AssetData.GetObjectPathString(), AssetData);
        }
    }

    TMap<FString, FIndexedAssetStamp> IndexedAssetsByPath;
    FSQLitePreparedStatement IndexedStatement(
        Database,
        TEXT("SELECT object_path, package_name, indexed_at_utc FROM assets WHERE content_scope = 'project' ORDER BY object_path ASC;"),
        ESQLitePreparedStatementFlags::None);
    if (!IndexedStatement.IsValid())
    {
        OutError = TEXT("Could not prepare the indexed asset timestamp query.");
        return false;
    }

    const int64 IndexedQueryResult = IndexedStatement.Execute([&](const FSQLitePreparedStatement& Row)
    {
        FString ObjectPath;
        FString PackageName;
        FString IndexedAtText;
        if (!Row.GetColumnValueByIndex(0, ObjectPath)
            || !Row.GetColumnValueByIndex(1, PackageName)
            || !Row.GetColumnValueByIndex(2, IndexedAtText))
        {
            return ESQLitePreparedStatementExecuteRowResult::Error;
        }

        FIndexedAssetStamp Stamp;
        Stamp.PackageName = MoveTemp(PackageName);
        FDateTime::ParseIso8601(*IndexedAtText, Stamp.IndexedAtUtc);
        IndexedAssetsByPath.Add(MoveTemp(ObjectPath), MoveTemp(Stamp));
        return ESQLitePreparedStatementExecuteRowResult::Continue;
    });
    if (IndexedQueryResult == INDEX_NONE)
    {
        OutError = Database.GetLastError().IsEmpty() ? TEXT("Indexed asset timestamp query failed.") : Database.GetLastError();
        return false;
    }

    TSet<FString> CandidatePaths;
    if (RequestedObjectPaths.Num() > 0)
    {
        for (const FString& ObjectPath : RequestedObjectPaths)
        {
            if (!ObjectPath.IsEmpty())
            {
                CandidatePaths.Add(ObjectPath);
            }
        }
    }
    else
    {
        CandidatePaths.Append(DirtyAssetSet);

        for (const TPair<FString, FAssetData>& Pair : CurrentAssetsByPath)
        {
            const FIndexedAssetStamp* Existing = IndexedAssetsByPath.Find(Pair.Key);
            if (Existing == nullptr || IsPackageNewerThanIndex(Existing->PackageName, Existing->IndexedAtUtc))
            {
                CandidatePaths.Add(Pair.Key);
            }
        }

        for (const TPair<FString, FIndexedAssetStamp>& Pair : IndexedAssetsByPath)
        {
            if (!CurrentAssetsByPath.Contains(Pair.Key))
            {
                CandidatePaths.Add(Pair.Key);
            }
        }
    }

    TArray<FString> SortedCandidates = CandidatePaths.Array();
    SortedCandidates.Sort();
    OutResult.CandidateCount = SortedCandidates.Num();
    if (ProgressCallback)
    {
        ProgressCallback(0, SortedCandidates.Num(), TEXT("Preparing UnrealMCP partial index refresh..."));
    }

    if (!ExecuteStatement(Database, TEXT("BEGIN TRANSACTION;"), &OutError))
    {
        LastError = OutError;
        return false;
    }

    auto Rollback = [this]()
    {
        FString Ignored;
        ExecuteStatement(Database, TEXT("ROLLBACK TRANSACTION;"), &Ignored);
    };

    for (int32 CandidateIndex = 0; CandidateIndex < SortedCandidates.Num(); ++CandidateIndex)
    {
        const FString& ObjectPath = SortedCandidates[CandidateIndex];
        if (ProgressCallback)
        {
            ProgressCallback(CandidateIndex, SortedCandidates.Num(), FString::Printf(TEXT("Refreshing %s"), *ObjectPath));
        }

        if (const FAssetData* AssetData = CurrentAssetsByPath.Find(ObjectPath))
        {
            if (!UpsertAsset(*AssetData, &OutError))
            {
                Rollback();
                LastError = OutError;
                return false;
            }
            ++OutResult.RefreshedAssetCount;
            OutResult.RefreshedAssets.Add(ObjectPath);
        }
        else if (const FIndexedAssetStamp* IndexedAsset = IndexedAssetsByPath.Find(ObjectPath))
        {
            if (!RemoveAsset(ObjectPath, IndexedAsset->PackageName, &OutError))
            {
                Rollback();
                LastError = OutError;
                return false;
            }
            ++OutResult.RemovedAssetCount;
            OutResult.RemovedAssets.Add(ObjectPath);
        }
        else
        {
            ++OutResult.UnchangedAssetCount;
        }
    }

    if (!ExecuteStatement(Database, TEXT("COMMIT TRANSACTION;"), &OutError))
    {
        Rollback();
        LastError = OutError;
        return false;
    }

    for (const FString& ObjectPath : SortedCandidates)
    {
        DirtyAssetSet.Remove(ObjectPath);
    }
    bIndexDirty = DirtyAssetSet.Num() > 0;
    LastUpdateUtc = ToUtcString(FDateTime::UtcNow());
    if (!SetMetadataValue(TEXT("last_update_utc"), LastUpdateUtc, &OutError)
        || !SetMetadataValue(TEXT("is_dirty"), bIndexDirty ? TEXT("1") : TEXT("0"), &OutError)
        || !RecalculateIndexedCounts(&OutError))
    {
        LastError = OutError;
        return false;
    }

    LastError.Reset();
    UE_LOG(LogUnrealMCP, Log, TEXT("Project index partial refresh completed. Candidates=%d Refreshed=%d Removed=%d"),
        OutResult.CandidateCount,
        OutResult.RefreshedAssetCount,
        OutResult.RemovedAssetCount);
    if (ProgressCallback)
    {
        ProgressCallback(SortedCandidates.Num(), SortedCandidates.Num(), TEXT("UnrealMCP partial index refresh complete."));
    }
    return true;
}
