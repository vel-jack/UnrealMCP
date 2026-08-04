#include "Tools/GetIndexStatusTool.h"

#include "Dom/JsonObject.h"
#include "Index/UnrealMCPProjectIndex.h"
#include "UnrealMCPModule.h"

FGetIndexStatusTool::FGetIndexStatusTool()
    : FMCPToolBase(TEXT("GetIndexStatus"), TEXT("Returns status information for the local UnrealMCP project index.")) 
{
}

UnrealMCP::FMCPResponse FGetIndexStatusTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FUnrealMCPProjectIndex::FStatusSnapshot Snapshot = FUnrealMCPModule::Get().GetProjectIndex().GetStatusSnapshot();

    TArray<TSharedPtr<FJsonValue>> DirtyAssets;
    DirtyAssets.Reserve(Snapshot.DirtyAssets.Num());
    for (const FString& ObjectPath : Snapshot.DirtyAssets)
    {
        DirtyAssets.Add(MakeShared<FJsonValueString>(ObjectPath));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetBoolField(TEXT("databaseOpen"), Snapshot.bDatabaseOpen);
    Result->SetBoolField(TEXT("schemaReady"), Snapshot.bSchemaReady);
    Result->SetBoolField(TEXT("assetRegistryLoaded"), Snapshot.bAssetRegistryLoaded);
    Result->SetBoolField(TEXT("liveTrackingEnabled"), Snapshot.bLiveTrackingEnabled);
    Result->SetBoolField(TEXT("hasUsableIndex"), Snapshot.bHasUsableIndex);
    Result->SetBoolField(TEXT("isDirty"), Snapshot.bIndexDirty);
    Result->SetNumberField(TEXT("schemaVersion"), Snapshot.SchemaVersion);
    Result->SetNumberField(TEXT("dirtyAssetCount"), Snapshot.DirtyAssetCount);
    Result->SetStringField(TEXT("indexedScope"), Snapshot.IndexedScope);
    Result->SetNumberField(TEXT("indexedAssetCount"), Snapshot.IndexedAssetCount);
    Result->SetNumberField(TEXT("indexedBlueprintCount"), Snapshot.IndexedBlueprintCount);
    Result->SetNumberField(TEXT("indexedProjectAssetCount"), Snapshot.IndexedProjectAssetCount);
    Result->SetNumberField(TEXT("indexedProjectBlueprintCount"), Snapshot.IndexedProjectBlueprintCount);
    Result->SetNumberField(TEXT("indexedEngineAssetCount"), Snapshot.IndexedEngineAssetCount);
    Result->SetNumberField(TEXT("indexedEngineBlueprintCount"), Snapshot.IndexedEngineBlueprintCount);
    Result->SetNumberField(TEXT("indexedPluginAssetCount"), Snapshot.IndexedPluginAssetCount);
    Result->SetNumberField(TEXT("indexedPluginBlueprintCount"), Snapshot.IndexedPluginBlueprintCount);
    Result->SetStringField(TEXT("databasePath"), Snapshot.DatabasePath);
    Result->SetStringField(TEXT("lastFullBuildUtc"), Snapshot.LastFullBuildUtc);
    Result->SetStringField(TEXT("lastUpdateUtc"), Snapshot.LastUpdateUtc);
    Result->SetStringField(TEXT("lastError"), Snapshot.LastError);
    Result->SetBoolField(TEXT("manualRebuildPreferred"), Snapshot.bManualRebuildPreferred);
    Result->SetBoolField(TEXT("rebuildRecommended"), Snapshot.bRebuildRecommended);
    Result->SetBoolField(TEXT("partialRefreshAvailable"), Snapshot.bHasUsableIndex);
    Result->SetBoolField(TEXT("refreshRecommended"), Snapshot.bHasUsableIndex && Snapshot.bIndexDirty);
    Result->SetStringField(TEXT("rebuildCadenceHint"), Snapshot.RebuildCadenceHint);
    Result->SetStringField(
        TEXT("agentPolicyHint"),
        Snapshot.bHasUsableIndex
            ? TEXT("prefer_RefreshProjectIndex_for_ordinary_changes; reserve_BuildProjectIndex_for_missing_or_invalid_indexes")
            : TEXT("request_user_triggered_BuildProjectIndex"));
    Result->SetArrayField(TEXT("dirtyAssetsPreview"), DirtyAssets);
    Response.Result = Result;
    return Response;
}
