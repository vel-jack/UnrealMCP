#include "Index/UnrealMCPProjectIndex.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Index/UnrealMCPProjectIndexInternal.h"
#include "UnrealMCPLog.h"

bool FUnrealMCPProjectIndex::BuildFullIndex(FString& OutError, const TFunction<void(int32, int32, const FString&)>& ProgressCallback)
{
    if (!Database.IsValid())
    {
        OutError = TEXT("Project index database is not open.");
        return false;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    TArray<FAssetData> Assets;
    AssetRegistry.GetAllAssets(Assets, true);
    Assets = Assets.FilterByPredicate([](const FAssetData& AssetData)
    {
        return ShouldIndexAsset(AssetData);
    });
    Assets.Sort([](const FAssetData& Left, const FAssetData& Right)
    {
        return Left.GetObjectPathString() < Right.GetObjectPathString();
    });

    if (ProgressCallback)
    {
        ProgressCallback(0, Assets.Num(), TEXT("Preparing UnrealMCP project index rebuild..."));
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

    if (!ExecuteStatement(Database, TEXT("DELETE FROM blueprint_variables;"), &OutError)
        || !ExecuteStatement(Database, TEXT("DELETE FROM blueprint_functions;"), &OutError)
        || !ExecuteStatement(Database, TEXT("DELETE FROM blueprint_components;"), &OutError)
        || !ExecuteStatement(Database, TEXT("DELETE FROM blueprint_edges;"), &OutError)
        || !ExecuteStatement(Database, TEXT("DELETE FROM blueprint_pins;"), &OutError)
        || !ExecuteStatement(Database, TEXT("DELETE FROM blueprint_nodes;"), &OutError)
        || !ExecuteStatement(Database, TEXT("DELETE FROM blueprint_graphs;"), &OutError)
        || !ExecuteStatement(Database, TEXT("DELETE FROM asset_dependencies;"), &OutError)
        || !ExecuteStatement(Database, TEXT("DELETE FROM assets;"), &OutError))
    {
        Rollback();
        LastError = OutError;
        return false;
    }

    for (int32 AssetIndex = 0; AssetIndex < Assets.Num(); ++AssetIndex)
    {
        const FAssetData& AssetData = Assets[AssetIndex];
        if (ProgressCallback && (AssetIndex == 0 || (AssetIndex % 50) == 0 || AssetIndex + 1 == Assets.Num()))
        {
            ProgressCallback(AssetIndex, Assets.Num(), FString::Printf(TEXT("Indexing %s"), *AssetData.AssetName.ToString()));
        }

        if (!UpsertAsset(AssetData, &OutError))
        {
            Rollback();
            LastError = OutError;
            return false;
        }
    }

    if (!ExecuteStatement(Database, TEXT("COMMIT TRANSACTION;"), &OutError))
    {
        Rollback();
        LastError = OutError;
        return false;
    }

    LastFullBuildUtc = ToUtcString(FDateTime::UtcNow());
    LastUpdateUtc = LastFullBuildUtc;
    bHasUsableIndex = true;
    MarkIndexClean();

    if (!SetMetadataValue(TEXT("last_full_build_utc"), LastFullBuildUtc, &OutError)
        || !SetMetadataValue(TEXT("last_update_utc"), LastUpdateUtc, &OutError)
        || !SetMetadataValue(TEXT("schema_version"), LexToString(CurrentSchemaVersion), &OutError)
        || !RecalculateIndexedCounts(&OutError))
    {
        LastError = OutError;
        return false;
    }

    LastError.Reset();
    UE_LOG(LogUnrealMCP, Log, TEXT("Project index full rebuild completed. Assets=%lld Blueprints=%lld"),
        IndexedAssetCount,
        IndexedBlueprintCount);

    if (ProgressCallback)
    {
        ProgressCallback(Assets.Num(), Assets.Num(), TEXT("UnrealMCP index rebuild complete."));
    }
    return true;
}

bool FUnrealMCPProjectIndex::OpenDatabase(FString& OutError)
{
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(DatabasePath), true);
    if (Database.Open(*DatabasePath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
    {
        return true;
    }

    OutError = Database.GetLastError();
    return false;
}

bool FUnrealMCPProjectIndex::EnsureSchema(FString& OutError)
{
    static const TCHAR* SchemaStatements[] =
    {
        TEXT("CREATE TABLE IF NOT EXISTS metadata ( key TEXT PRIMARY KEY, value TEXT NOT NULL );"),
        TEXT("CREATE TABLE IF NOT EXISTS assets ( object_path TEXT PRIMARY KEY, asset_name TEXT NOT NULL, class_path TEXT NOT NULL, package_name TEXT NOT NULL, package_path TEXT NOT NULL, content_scope TEXT NOT NULL, is_blueprint INTEGER NOT NULL, generated_class_path TEXT, parent_class_path TEXT, native_parent_class_path TEXT, blueprint_type TEXT, is_data_only TEXT, blueprint_status TEXT, variable_count INTEGER NOT NULL DEFAULT 0, function_count INTEGER NOT NULL DEFAULT 0, component_count INTEGER NOT NULL DEFAULT 0, interface_count INTEGER NOT NULL DEFAULT 0, indexed_at_utc TEXT NOT NULL );"),
        TEXT("CREATE TABLE IF NOT EXISTS asset_dependencies ( source_package_name TEXT NOT NULL, target_package_name TEXT NOT NULL, PRIMARY KEY(source_package_name, target_package_name) );"),
        TEXT("CREATE TABLE IF NOT EXISTS blueprint_components ( blueprint_object_path TEXT NOT NULL, variable_name TEXT NOT NULL, component_class_path TEXT, component_blueprint_path TEXT, template_name TEXT, template_path TEXT, parent_variable_name TEXT, attach_socket_name TEXT, creation_source TEXT NOT NULL, is_scene_component INTEGER NOT NULL DEFAULT 0, is_default_scene_root INTEGER NOT NULL DEFAULT 0, child_count INTEGER NOT NULL DEFAULT 0, relative_location TEXT, relative_rotation TEXT, relative_scale TEXT, mobility TEXT, PRIMARY KEY(blueprint_object_path, variable_name) );"),
        TEXT("CREATE TABLE IF NOT EXISTS blueprint_functions ( blueprint_object_path TEXT NOT NULL, name TEXT NOT NULL, source TEXT NOT NULL, interface_path TEXT, PRIMARY KEY(blueprint_object_path, name, source, interface_path) );"),
        TEXT("CREATE TABLE IF NOT EXISTS blueprint_variables ( blueprint_object_path TEXT NOT NULL, name TEXT NOT NULL, type_category TEXT, type_subcategory TEXT, type_object_path TEXT, container_type TEXT, is_reference INTEGER NOT NULL DEFAULT 0, is_const INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(blueprint_object_path, name) );"),
        TEXT("CREATE TABLE IF NOT EXISTS blueprint_graphs ( blueprint_object_path TEXT NOT NULL, graph_name TEXT NOT NULL, graph_guid TEXT, graph_type TEXT NOT NULL, entry_node_guid TEXT, node_count INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(blueprint_object_path, graph_name) );"),
        TEXT("CREATE TABLE IF NOT EXISTS blueprint_nodes ( blueprint_object_path TEXT NOT NULL, graph_name TEXT NOT NULL, node_guid TEXT NOT NULL, node_name TEXT NOT NULL, node_class_path TEXT NOT NULL, node_title TEXT, node_type TEXT NOT NULL, member_name TEXT, member_parent_path TEXT, pos_x INTEGER NOT NULL DEFAULT 0, pos_y INTEGER NOT NULL DEFAULT 0, is_pure INTEGER NOT NULL DEFAULT 0, input_pin_count INTEGER NOT NULL DEFAULT 0, output_pin_count INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(blueprint_object_path, graph_name, node_guid) );"),
        TEXT("CREATE TABLE IF NOT EXISTS blueprint_pins ( blueprint_object_path TEXT NOT NULL, graph_name TEXT NOT NULL, node_guid TEXT NOT NULL, pin_id TEXT NOT NULL, pin_name TEXT, direction TEXT NOT NULL, category TEXT, subcategory TEXT, subcategory_object_path TEXT, container_type TEXT, is_reference INTEGER NOT NULL DEFAULT 0, is_const INTEGER NOT NULL DEFAULT 0, linked_pin_count INTEGER NOT NULL DEFAULT 0, default_value TEXT, PRIMARY KEY(blueprint_object_path, graph_name, node_guid, pin_id) );"),
        TEXT("CREATE TABLE IF NOT EXISTS blueprint_edges ( blueprint_object_path TEXT NOT NULL, source_graph_name TEXT NOT NULL, source_node_guid TEXT NOT NULL, source_pin_id TEXT NOT NULL, target_graph_name TEXT NOT NULL, target_node_guid TEXT NOT NULL, target_pin_id TEXT NOT NULL, edge_kind TEXT NOT NULL, PRIMARY KEY(blueprint_object_path, source_graph_name, source_node_guid, source_pin_id, target_graph_name, target_node_guid, target_pin_id) );"),
        TEXT("CREATE INDEX IF NOT EXISTS idx_blueprint_nodes_by_name ON blueprint_nodes(blueprint_object_path, node_name);"),
        TEXT("CREATE INDEX IF NOT EXISTS idx_blueprint_nodes_by_type ON blueprint_nodes(blueprint_object_path, node_type);"),
        TEXT("CREATE INDEX IF NOT EXISTS idx_blueprint_edges_by_source ON blueprint_edges(blueprint_object_path, source_graph_name, source_node_guid);"),
        TEXT("CREATE INDEX IF NOT EXISTS idx_blueprint_edges_by_target ON blueprint_edges(blueprint_object_path, target_graph_name, target_node_guid);"),
        TEXT("CREATE INDEX IF NOT EXISTS idx_blueprint_pins_by_name ON blueprint_pins(blueprint_object_path, pin_name);"),
        TEXT("CREATE INDEX IF NOT EXISTS idx_blueprint_graphs_by_type ON blueprint_graphs(blueprint_object_path, graph_type);")
    };

    for (const TCHAR* Statement : SchemaStatements)
    {
        if (!ExecuteStatement(Database, Statement, &OutError))
        {
            return false;
        }
    }

    const FString ExistingSchemaVersion = GetMetadataValue(TEXT("schema_version"));
    if (!ExistingSchemaVersion.IsEmpty())
    {
        const int32 ParsedSchemaVersion = FCString::Atoi(*ExistingSchemaVersion);
        if (ParsedSchemaVersion != CurrentSchemaVersion)
        {
            OutError = FString::Printf(
                TEXT("Project index schema version mismatch. Expected=%d Actual=%d Database=%s. Delete the SQLite index file and rebuild the index."),
                CurrentSchemaVersion,
                ParsedSchemaVersion,
                *DatabasePath);
            return false;
        }
    }

    if (!SetMetadataValue(TEXT("schema_version"), LexToString(CurrentSchemaVersion), &OutError))
    {
        return false;
    }

    bSchemaReady = true;
    return true;
}

bool FUnrealMCPProjectIndex::RecalculateIndexedCounts(FString* OutError)
{
    const int64 AssetCount = QuerySingleInt64(Database, TEXT("SELECT COUNT(*) FROM assets;"), OutError);
    const int64 BlueprintCount = QuerySingleInt64(Database, TEXT("SELECT COUNT(*) FROM assets WHERE is_blueprint = 1;"), OutError);
    const int64 ProjectAssetCount = QuerySingleInt64(Database, TEXT("SELECT COUNT(*) FROM assets WHERE content_scope = 'project';"), OutError);
    const int64 ProjectBlueprintCount = QuerySingleInt64(Database, TEXT("SELECT COUNT(*) FROM assets WHERE content_scope = 'project' AND is_blueprint = 1;"), OutError);
    const int64 EngineAssetCount = QuerySingleInt64(Database, TEXT("SELECT COUNT(*) FROM assets WHERE content_scope = 'engine';"), OutError);
    const int64 EngineBlueprintCount = QuerySingleInt64(Database, TEXT("SELECT COUNT(*) FROM assets WHERE content_scope = 'engine' AND is_blueprint = 1;"), OutError);
    const int64 PluginAssetCount = QuerySingleInt64(Database, TEXT("SELECT COUNT(*) FROM assets WHERE content_scope = 'plugin';"), OutError);
    const int64 PluginBlueprintCount = QuerySingleInt64(Database, TEXT("SELECT COUNT(*) FROM assets WHERE content_scope = 'plugin' AND is_blueprint = 1;"), OutError);
    if (AssetCount == INDEX_NONE || BlueprintCount == INDEX_NONE
        || ProjectAssetCount == INDEX_NONE || ProjectBlueprintCount == INDEX_NONE
        || EngineAssetCount == INDEX_NONE || EngineBlueprintCount == INDEX_NONE
        || PluginAssetCount == INDEX_NONE || PluginBlueprintCount == INDEX_NONE)
    {
        return false;
    }

    IndexedAssetCount = AssetCount;
    IndexedBlueprintCount = BlueprintCount;
    IndexedProjectAssetCount = ProjectAssetCount;
    IndexedProjectBlueprintCount = ProjectBlueprintCount;
    IndexedEngineAssetCount = EngineAssetCount;
    IndexedEngineBlueprintCount = EngineBlueprintCount;
    IndexedPluginAssetCount = PluginAssetCount;
    IndexedPluginBlueprintCount = PluginBlueprintCount;
    return true;
}
