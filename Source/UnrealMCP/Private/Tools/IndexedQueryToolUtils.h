#pragma once

#include "Async/Async.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Index/UnrealMCPProjectIndex.h"
#include "Misc/ScopeExit.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "Tools/AssetRegistryToolUtils.h"
#include "UnrealMCPModule.h"

namespace UnrealMCP::IndexedQueryToolUtils
{
    template <typename TCallable>
    bool ExecuteWithProjectIndex(TCallable&& Callable, FString& OutError)
    {
        FUnrealMCPProjectIndex& ProjectIndex = FUnrealMCPModule::Get().GetProjectIndex();
        if (!ProjectIndex.GetStatusSnapshot().bDatabaseOpen)
        {
            OutError = TEXT("Project index database is not open.");
            return false;
        }

        bool bSucceeded = false;
        auto Run = [&]()
        {
            bSucceeded = Callable(ProjectIndex.GetDatabase(), OutError);
        };

        if (IsInGameThread())
        {
            Run();
            return bSucceeded;
        }

        FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(false);
        if (CompletionEvent == nullptr)
        {
            OutError = TEXT("Could not create a synchronization event for indexed query execution.");
            return false;
        }

        ON_SCOPE_EXIT
        {
            FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
        };

        AsyncTask(ENamedThreads::GameThread, [&Run, CompletionEvent]()
        {
            Run();
            CompletionEvent->Trigger();
        });

        CompletionEvent->Wait();
        return bSucceeded;
    }

    inline bool QuerySingleAssetRow(
        FSQLiteDatabase& Database,
        const TCHAR* Sql,
        const FString& InputValue,
        TSharedPtr<FJsonObject>& OutAssetObject,
        FString& OutError)
    {
        FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
        if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, InputValue))
        {
            OutError = TEXT("Failed to prepare the asset lookup query.");
            return false;
        }

        bool bFound = false;
        const int64 QueryResult = Statement.Execute([&](const FSQLitePreparedStatement& Row)
        {
            FString ObjectPath;
            FString AssetName;
            FString ClassPath;
            FString PackageName;
            FString PackagePath;
            FString ContentScope;
            int32 bIsBlueprint = 0;

            if (!Row.GetColumnValueByIndex(0, ObjectPath)
                || !Row.GetColumnValueByIndex(1, AssetName)
                || !Row.GetColumnValueByIndex(2, ClassPath)
                || !Row.GetColumnValueByIndex(3, PackageName)
                || !Row.GetColumnValueByIndex(4, PackagePath)
                || !Row.GetColumnValueByIndex(5, ContentScope)
                || !Row.GetColumnValueByIndex(6, bIsBlueprint))
            {
                return ESQLitePreparedStatementExecuteRowResult::Error;
            }

            TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
            AssetObject->SetStringField(TEXT("objectPath"), ObjectPath);
            AssetObject->SetStringField(TEXT("assetName"), AssetName);
            AssetObject->SetStringField(TEXT("classPath"), ClassPath);
            AssetObject->SetStringField(TEXT("packageName"), PackageName);
            AssetObject->SetStringField(TEXT("packagePath"), PackagePath);
            AssetObject->SetStringField(TEXT("contentScope"), ContentScope);
            AssetObject->SetBoolField(TEXT("isBlueprint"), bIsBlueprint != 0);
            OutAssetObject = AssetObject;
            bFound = true;
            return ESQLitePreparedStatementExecuteRowResult::Stop;
        });

        if (QueryResult == INDEX_NONE)
        {
            OutError = Database.GetLastError();
            return false;
        }

        return bFound;
    }

    inline bool QueryAssetByPackageName(FSQLiteDatabase& Database, const FString& PackageName, TSharedPtr<FJsonObject>& OutAssetObject, FString& OutError)
    {
        return QuerySingleAssetRow(
            Database,
            TEXT("SELECT object_path, asset_name, class_path, package_name, package_path, content_scope, is_blueprint "
                 "FROM assets WHERE package_name = ?1 ORDER BY object_path ASC;"),
            PackageName,
            OutAssetObject,
            OutError);
    }

    inline bool QueryAssetByObjectPath(FSQLiteDatabase& Database, const FString& ObjectPath, TSharedPtr<FJsonObject>& OutAssetObject, FString& OutError)
    {
        return QuerySingleAssetRow(
            Database,
            TEXT("SELECT object_path, asset_name, class_path, package_name, package_path, content_scope, is_blueprint "
                 "FROM assets WHERE object_path = ?1;"),
            ObjectPath,
            OutAssetObject,
            OutError);
    }

    inline bool OpenIndexDatabase(FSQLiteDatabase& OutDatabase, FString& OutError)
    {
        const FString DatabasePath = FUnrealMCPModule::Get().GetProjectIndex().GetStatusSnapshot().DatabasePath;
        if (DatabasePath.IsEmpty())
        {
            OutError = TEXT("Project index database path is unavailable.");
            return false;
        }

        if (OutDatabase.Open(*DatabasePath, ESQLiteDatabaseOpenMode::ReadOnly))
        {
            return true;
        }

        OutError = OutDatabase.GetLastError();
        return false;
    }

    inline bool ResolvePackageName(const TSharedPtr<FJsonObject>& Params, FString& OutObjectPath, FName& OutPackageName)
    {
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        return UnrealMCP::AssetRegistryToolUtils::ResolvePackageName(Params, AssetRegistryModule.Get(), OutObjectPath, OutPackageName);
    }

    inline FString GetContentScopeFromPackageName(const FString& PackageName)
    {
        if (PackageName.StartsWith(TEXT("/Game")))
        {
            return TEXT("project");
        }

        if (PackageName.StartsWith(TEXT("/Engine")))
        {
            return TEXT("engine");
        }

        return TEXT("plugin");
    }

    inline TSharedRef<FJsonObject> SerializePackageSummary(const FString& PackageName)
    {
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        const FName PackageFName(*PackageName);
        TArray<FAssetData> PackageAssets;
        AssetRegistry.GetAssetsByPackageName(PackageFName, PackageAssets, true);
        UnrealMCP::AssetRegistryToolUtils::SortAssets(PackageAssets);

        TSharedRef<FJsonObject> PackageObject = MakeShared<FJsonObject>();
        PackageObject->SetStringField(TEXT("packageName"), PackageName);
        PackageObject->SetStringField(TEXT("contentScope"), GetContentScopeFromPackageName(PackageName));
        PackageObject->SetNumberField(TEXT("assetCount"), PackageAssets.Num());
        PackageObject->SetArrayField(TEXT("assets"), UnrealMCP::AssetRegistryToolUtils::SerializeAssetArray(PackageAssets));
        return PackageObject;
    }
}
