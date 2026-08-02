#include "Index/UnrealMCPProjectIndex.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/BlueprintSupport.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SQLitePreparedStatement.h"
#include "Tools/BlueprintToolUtils.h"
#include "UnrealMCPLog.h"
#include "UnrealMCPSettings.h"

namespace
{
    bool ExecuteStatement(FSQLiteDatabase& Database, const TCHAR* Sql, FString* OutError = nullptr)
    {
        if (Database.Execute(Sql))
        {
            return true;
        }

        if (OutError)
        {
            *OutError = Database.GetLastError();
        }
        return false;
    }

    bool ExecuteBoundStatement(FSQLiteDatabase& Database, const TCHAR* Sql, TFunctionRef<bool(FSQLitePreparedStatement&)> Binder, FString* OutError = nullptr)
    {
        FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
        if (!Statement.IsValid())
        {
            if (OutError)
            {
                *OutError = Database.GetLastError();
            }
            return false;
        }

        if (!Binder(Statement))
        {
            if (OutError)
            {
                *OutError = TEXT("Failed to bind SQLite statement values.");
            }
            return false;
        }

        if (Statement.Execute())
        {
            return true;
        }

        if (OutError)
        {
            *OutError = Database.GetLastError();
        }
        return false;
    }

    int64 QuerySingleInt64(FSQLiteDatabase& Database, const TCHAR* Sql, FString* OutError = nullptr)
    {
        int64 Value = 0;
        bool bHasRow = false;

        FSQLitePreparedStatement Statement(Database, Sql, ESQLitePreparedStatementFlags::None);
        if (!Statement.IsValid())
        {
            if (OutError)
            {
                *OutError = Database.GetLastError();
            }
            return INDEX_NONE;
        }

        const int64 RowCount = Statement.Execute([&Value, &bHasRow](const FSQLitePreparedStatement& Row)
        {
            bHasRow = Row.GetColumnValueByIndex(0, Value);
            return bHasRow ? ESQLitePreparedStatementExecuteRowResult::Continue : ESQLitePreparedStatementExecuteRowResult::Error;
        });

        if (RowCount == INDEX_NONE || !bHasRow)
        {
            if (OutError)
            {
                *OutError = Database.GetLastError();
            }
            return INDEX_NONE;
        }

        return Value;
    }
}

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

FSQLiteDatabase& FUnrealMCPProjectIndex::GetDatabase()
{
    return Database;
}

const FSQLiteDatabase& FUnrealMCPProjectIndex::GetDatabase() const
{
    return Database;
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

bool FUnrealMCPProjectIndex::EnsureSchema(FString& OutError, bool bSkipVersionCheck)
{
    static const TCHAR* SchemaStatements[] =
    {
        TEXT("CREATE TABLE IF NOT EXISTS metadata ("
            " key TEXT PRIMARY KEY,"
            " value TEXT NOT NULL"
            ");"),
        TEXT("CREATE TABLE IF NOT EXISTS assets ("
            " object_path TEXT PRIMARY KEY,"
            " asset_name TEXT NOT NULL,"
            " class_path TEXT NOT NULL,"
            " package_name TEXT NOT NULL,"
            " package_path TEXT NOT NULL,"
            " content_scope TEXT NOT NULL,"
            " is_blueprint INTEGER NOT NULL,"
            " generated_class_path TEXT,"
            " parent_class_path TEXT,"
            " native_parent_class_path TEXT,"
            " blueprint_type TEXT,"
            " is_data_only TEXT,"
            " blueprint_status TEXT,"
            " variable_count INTEGER NOT NULL DEFAULT 0,"
            " function_count INTEGER NOT NULL DEFAULT 0,"
            " component_count INTEGER NOT NULL DEFAULT 0,"
            " interface_count INTEGER NOT NULL DEFAULT 0,"
            " indexed_at_utc TEXT NOT NULL"
            ");"),
        TEXT("CREATE TABLE IF NOT EXISTS asset_dependencies ("
            " source_package_name TEXT NOT NULL,"
            " target_package_name TEXT NOT NULL,"
            " PRIMARY KEY(source_package_name, target_package_name)"
            ");"),
        TEXT("CREATE TABLE IF NOT EXISTS blueprint_components ("
            " blueprint_object_path TEXT NOT NULL,"
            " variable_name TEXT NOT NULL,"
            " component_class_path TEXT,"
            " parent_variable_name TEXT,"
            " is_default_scene_root INTEGER NOT NULL DEFAULT 0,"
            " child_count INTEGER NOT NULL DEFAULT 0,"
            " PRIMARY KEY(blueprint_object_path, variable_name)"
            ");"),
        TEXT("CREATE TABLE IF NOT EXISTS blueprint_functions ("
            " blueprint_object_path TEXT NOT NULL,"
            " name TEXT NOT NULL,"
            " source TEXT NOT NULL,"
            " interface_path TEXT,"
            " PRIMARY KEY(blueprint_object_path, name, source, interface_path)"
            ");"),
        TEXT("CREATE TABLE IF NOT EXISTS blueprint_variables ("
            " blueprint_object_path TEXT NOT NULL,"
            " name TEXT NOT NULL,"
            " type_category TEXT,"
            " type_subcategory TEXT,"
            " type_object_path TEXT,"
            " container_type TEXT,"
            " is_reference INTEGER NOT NULL DEFAULT 0,"
            " is_const INTEGER NOT NULL DEFAULT 0,"
            " PRIMARY KEY(blueprint_object_path, name)"
            ");")
    };

    for (const TCHAR* Statement : SchemaStatements)
    {
        if (!ExecuteStatement(Database, Statement, &OutError))
        {
            return false;
        }
    }

    const FString ExistingSchemaVersion = GetMetadataValue(TEXT("schema_version"));
    if (!bSkipVersionCheck && !ExistingSchemaVersion.IsEmpty())
    {
        const int32 ParsedSchemaVersion = FCString::Atoi(*ExistingSchemaVersion);
        if (ParsedSchemaVersion != CurrentSchemaVersion)
        {
            return RebuildSchema(OutError);
        }
    }

    if (!SetMetadataValue(TEXT("schema_version"), LexToString(CurrentSchemaVersion), &OutError))
    {
        return false;
    }

    bSchemaReady = true;
    return true;
}

bool FUnrealMCPProjectIndex::RebuildSchema(FString& OutError)
{
    static const TCHAR* DropStatements[] =
    {
        TEXT("DROP TABLE IF EXISTS blueprint_variables;"),
        TEXT("DROP TABLE IF EXISTS blueprint_functions;"),
        TEXT("DROP TABLE IF EXISTS blueprint_components;"),
        TEXT("DROP TABLE IF EXISTS asset_dependencies;"),
        TEXT("DROP TABLE IF EXISTS assets;")
    };

    for (const TCHAR* Statement : DropStatements)
    {
        if (!ExecuteStatement(Database, Statement, &OutError))
        {
            return false;
        }
    }

    bSchemaReady = false;
    return EnsureSchema(OutError, true);
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

bool FUnrealMCPProjectIndex::UpsertAsset(const FAssetData& AssetData, FString* OutError)
{
    const FString ObjectPath = AssetData.GetObjectPathString();
    const FString PackageName = AssetData.PackageName.ToString();

    if (!RemoveAsset(ObjectPath, PackageName, OutError))
    {
        return false;
    }

    const bool bIsBlueprint = IsBlueprintAsset(AssetData);
    const FString ContentScope = GetContentScope(AssetData);
    FString GeneratedClassPath = AssetData.GetTagValueRef<FString>(FBlueprintTags::GeneratedClassPath);
    FString ParentClassPath = AssetData.GetTagValueRef<FString>(FBlueprintTags::ParentClassPath);
    const FString NativeParentClassPath = AssetData.GetTagValueRef<FString>(FBlueprintTags::NativeParentClassPath);
    const FString BlueprintType = AssetData.GetTagValueRef<FString>(FBlueprintTags::BlueprintType);
    const FString IsDataOnly = AssetData.GetTagValueRef<FString>(FBlueprintTags::IsDataOnly);
    FString BlueprintStatus;
    int32 VariableCount = 0;
    int32 FunctionCount = 0;
    int32 ComponentCount = 0;
    int32 InterfaceCount = 0;

    TArray<FBPVariableDescription> BlueprintVariables;
    TArray<TTuple<FString, FString, FString>> BlueprintFunctions;
    TArray<TTuple<FString, FString, FString, bool, int32>> BlueprintComponents;

    if (bIsBlueprint)
    {
        if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath))
        {
            BlueprintStatus = UnrealMCP::BlueprintToolUtils::GetBlueprintStatusString(Blueprint->Status);
            VariableCount = Blueprint->NewVariables.Num();
            InterfaceCount = Blueprint->ImplementedInterfaces.Num();
            GeneratedClassPath = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : GeneratedClassPath;
            ParentClassPath = Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : ParentClassPath;

            BlueprintVariables = Blueprint->NewVariables;

            TArray<UEdGraph*> FunctionGraphs = Blueprint->FunctionGraphs;
            FunctionGraphs.Sort([](const UEdGraph& Left, const UEdGraph& Right)
            {
                return Left.GetName() < Right.GetName();
            });

            for (const UEdGraph* Graph : FunctionGraphs)
            {
                BlueprintFunctions.Emplace(Graph ? Graph->GetName() : FString(), TEXT("functionGraph"), FString());
            }

            for (const FBPInterfaceDescription& InterfaceDescription : Blueprint->ImplementedInterfaces)
            {
                for (const UEdGraph* Graph : InterfaceDescription.Graphs)
                {
                    BlueprintFunctions.Emplace(
                        Graph ? Graph->GetName() : FString(),
                        TEXT("interfaceGraph"),
                        InterfaceDescription.Interface ? InterfaceDescription.Interface->GetPathName() : FString());
                }
            }

            FunctionCount = BlueprintFunctions.Num();

            if (Blueprint->SimpleConstructionScript)
            {
                TArray<const USCS_Node*> Nodes;
                for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
                {
                    Nodes.Add(Node);
                }

                Nodes.Sort([](const USCS_Node& Left, const USCS_Node& Right)
                {
                    return Left.GetVariableName().LexicalLess(Right.GetVariableName());
                });

                for (const USCS_Node* Node : Nodes)
                {
                    const USCS_Node* ParentNode = Blueprint->SimpleConstructionScript->FindParentNode(const_cast<USCS_Node*>(Node));
                    BlueprintComponents.Emplace(
                        Node ? Node->GetVariableName().ToString() : FString(),
                        Node && Node->ComponentClass ? Node->ComponentClass->GetPathName() : FString(),
                        ParentNode ? ParentNode->GetVariableName().ToString() : FString(),
                        Blueprint->SimpleConstructionScript->GetDefaultSceneRootNode() == Node,
                        Node ? Node->GetChildNodes().Num() : 0);
                }
            }

            ComponentCount = BlueprintComponents.Num();
        }
        else
        {
            BlueprintStatus = TEXT("load_failed");
        }
    }

    const FString IndexedAtUtc = ToUtcString(FDateTime::UtcNow());
    if (!ExecuteBoundStatement(Database,
        TEXT("INSERT INTO assets("
            "object_path, asset_name, class_path, package_name, package_path, content_scope, is_blueprint, generated_class_path, parent_class_path, native_parent_class_path, blueprint_type, is_data_only, blueprint_status, variable_count, function_count, component_count, interface_count, indexed_at_utc)"
            " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18);"),
        [&AssetData, &ObjectPath, &PackageName, &ContentScope, &GeneratedClassPath, &ParentClassPath, &NativeParentClassPath, &BlueprintType, &IsDataOnly, &BlueprintStatus, VariableCount, FunctionCount, ComponentCount, InterfaceCount, &IndexedAtUtc, bIsBlueprint](FSQLitePreparedStatement& Statement)
        {
            return Statement.SetBindingValueByIndex(1, ObjectPath)
                && Statement.SetBindingValueByIndex(2, AssetData.AssetName.ToString())
                && Statement.SetBindingValueByIndex(3, AssetData.AssetClassPath.ToString())
                && Statement.SetBindingValueByIndex(4, PackageName)
                && Statement.SetBindingValueByIndex(5, AssetData.PackagePath.ToString())
                && Statement.SetBindingValueByIndex(6, ContentScope)
                && Statement.SetBindingValueByIndex(7, bIsBlueprint ? 1 : 0)
                && Statement.SetBindingValueByIndex(8, GeneratedClassPath)
                && Statement.SetBindingValueByIndex(9, ParentClassPath)
                && Statement.SetBindingValueByIndex(10, NativeParentClassPath)
                && Statement.SetBindingValueByIndex(11, BlueprintType)
                && Statement.SetBindingValueByIndex(12, IsDataOnly)
                && Statement.SetBindingValueByIndex(13, BlueprintStatus)
                && Statement.SetBindingValueByIndex(14, VariableCount)
                && Statement.SetBindingValueByIndex(15, FunctionCount)
                && Statement.SetBindingValueByIndex(16, ComponentCount)
                && Statement.SetBindingValueByIndex(17, InterfaceCount)
                && Statement.SetBindingValueByIndex(18, IndexedAtUtc);
        },
        OutError))
    {
        return false;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FName> Dependencies;
    AssetRegistryModule.Get().GetDependencies(AssetData.PackageName, Dependencies);
    Dependencies.Sort(FNameLexicalLess());

    for (const FName& DependencyPackage : Dependencies)
    {
        if (!ExecuteBoundStatement(Database,
            TEXT("INSERT INTO asset_dependencies(source_package_name, target_package_name) VALUES(?1, ?2);"),
            [&PackageName, &DependencyPackage](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, PackageName)
                    && Statement.SetBindingValueByIndex(2, DependencyPackage.ToString());
            },
            OutError))
        {
            return false;
        }
    }

    for (const FBPVariableDescription& Variable : BlueprintVariables)
    {
        if (!ExecuteBoundStatement(Database,
            TEXT("INSERT INTO blueprint_variables(blueprint_object_path, name, type_category, type_subcategory, type_object_path, container_type, is_reference, is_const)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);"),
            [&ObjectPath, &Variable](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath)
                    && Statement.SetBindingValueByIndex(2, Variable.VarName.ToString())
                    && Statement.SetBindingValueByIndex(3, Variable.VarType.PinCategory.ToString())
                    && Statement.SetBindingValueByIndex(4, Variable.VarType.PinSubCategory.ToString())
                    && Statement.SetBindingValueByIndex(5, Variable.VarType.PinSubCategoryObject.IsValid() ? Variable.VarType.PinSubCategoryObject->GetPathName() : FString())
                    && Statement.SetBindingValueByIndex(6, StaticEnum<EPinContainerType>()->GetNameStringByValue(static_cast<int64>(Variable.VarType.ContainerType)))
                    && Statement.SetBindingValueByIndex(7, Variable.VarType.bIsReference ? 1 : 0)
                    && Statement.SetBindingValueByIndex(8, Variable.VarType.bIsConst ? 1 : 0);
            },
            OutError))
        {
            return false;
        }
    }

    for (const TTuple<FString, FString, FString>& FunctionRow : BlueprintFunctions)
    {
        if (!ExecuteBoundStatement(Database,
            TEXT("INSERT INTO blueprint_functions(blueprint_object_path, name, source, interface_path) VALUES(?1, ?2, ?3, ?4);"),
            [&ObjectPath, &FunctionRow](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath)
                    && Statement.SetBindingValueByIndex(2, FunctionRow.Get<0>())
                    && Statement.SetBindingValueByIndex(3, FunctionRow.Get<1>())
                    && Statement.SetBindingValueByIndex(4, FunctionRow.Get<2>());
            },
            OutError))
        {
            return false;
        }
    }

    for (const TTuple<FString, FString, FString, bool, int32>& ComponentRow : BlueprintComponents)
    {
        if (!ExecuteBoundStatement(Database,
            TEXT("INSERT INTO blueprint_components(blueprint_object_path, variable_name, component_class_path, parent_variable_name, is_default_scene_root, child_count)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6);"),
            [&ObjectPath, &ComponentRow](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath)
                    && Statement.SetBindingValueByIndex(2, ComponentRow.Get<0>())
                    && Statement.SetBindingValueByIndex(3, ComponentRow.Get<1>())
                    && Statement.SetBindingValueByIndex(4, ComponentRow.Get<2>())
                    && Statement.SetBindingValueByIndex(5, ComponentRow.Get<3>() ? 1 : 0)
                    && Statement.SetBindingValueByIndex(6, ComponentRow.Get<4>());
            },
            OutError))
        {
            return false;
        }
    }

    LastUpdateUtc = IndexedAtUtc;
    SetMetadataValue(TEXT("last_update_utc"), LastUpdateUtc, nullptr);
    return true;
}

bool FUnrealMCPProjectIndex::RemoveAsset(const FString& ObjectPath, const FString& PackageName, FString* OutError)
{
    return ExecuteBoundStatement(Database,
            TEXT("DELETE FROM blueprint_variables WHERE blueprint_object_path = ?1;"),
            [&ObjectPath](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath);
            },
            OutError)
        && ExecuteBoundStatement(Database,
            TEXT("DELETE FROM blueprint_functions WHERE blueprint_object_path = ?1;"),
            [&ObjectPath](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath);
            },
            OutError)
        && ExecuteBoundStatement(Database,
            TEXT("DELETE FROM blueprint_components WHERE blueprint_object_path = ?1;"),
            [&ObjectPath](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath);
            },
            OutError)
        && ExecuteBoundStatement(Database,
            TEXT("DELETE FROM asset_dependencies WHERE source_package_name = ?1;"),
            [&PackageName](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, PackageName);
            },
            OutError)
        && ExecuteBoundStatement(Database,
            TEXT("DELETE FROM assets WHERE object_path = ?1;"),
            [&ObjectPath](FSQLitePreparedStatement& Statement)
            {
                return Statement.SetBindingValueByIndex(1, ObjectPath);
            },
            OutError);
}

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
