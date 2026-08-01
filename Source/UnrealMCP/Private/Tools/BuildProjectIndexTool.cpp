#include "Tools/BuildProjectIndexTool.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Index/UnrealMCPProjectIndex.h"
#include "Misc/ScopeExit.h"
#include "UnrealMCPModule.h"

FBuildProjectIndexTool::FBuildProjectIndexTool()
    : FMCPToolBase(TEXT("BuildProjectIndex"), TEXT("Builds or rebuilds the local UnrealMCP project-understanding index.")) 
{
}

UnrealMCP::FMCPResponse FBuildProjectIndexTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    const FDateTime StartedAt = FDateTime::UtcNow();

    bool bSucceeded = false;
    FString Error;

    auto BuildIndex = [&bSucceeded, &Error]()
    {
        bSucceeded = FUnrealMCPModule::Get().GetProjectIndex().BuildFullIndex(Error);
    };

    if (IsInGameThread())
    {
        BuildIndex();
    }
    else
    {
        FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(false);
        if (CompletionEvent == nullptr)
        {
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, TEXT("BuildProjectIndex failed: could not create a synchronization event."));
        }

        ON_SCOPE_EXIT
        {
            FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
        };

        AsyncTask(ENamedThreads::GameThread, [&BuildIndex, CompletionEvent]()
        {
            BuildIndex();
            CompletionEvent->Trigger();
        });

        CompletionEvent->Wait();
    }

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("BuildProjectIndex failed: %s"), *Error));
    }

    const FUnrealMCPProjectIndex::FStatusSnapshot Snapshot = FUnrealMCPModule::Get().GetProjectIndex().GetStatusSnapshot();

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("databasePath"), Snapshot.DatabasePath);
    Result->SetStringField(TEXT("indexedScope"), Snapshot.IndexedScope);
    Result->SetBoolField(TEXT("hasUsableIndex"), Snapshot.bHasUsableIndex);
    Result->SetBoolField(TEXT("isDirty"), Snapshot.bIndexDirty);
    Result->SetBoolField(TEXT("assetRegistryLoaded"), Snapshot.bAssetRegistryLoaded);
    Result->SetNumberField(TEXT("indexedAssetCount"), Snapshot.IndexedAssetCount);
    Result->SetNumberField(TEXT("indexedBlueprintCount"), Snapshot.IndexedBlueprintCount);
    Result->SetNumberField(TEXT("indexedProjectAssetCount"), Snapshot.IndexedProjectAssetCount);
    Result->SetNumberField(TEXT("indexedProjectBlueprintCount"), Snapshot.IndexedProjectBlueprintCount);
    Result->SetNumberField(TEXT("indexedEngineAssetCount"), Snapshot.IndexedEngineAssetCount);
    Result->SetNumberField(TEXT("indexedEngineBlueprintCount"), Snapshot.IndexedEngineBlueprintCount);
    Result->SetNumberField(TEXT("indexedPluginAssetCount"), Snapshot.IndexedPluginAssetCount);
    Result->SetNumberField(TEXT("indexedPluginBlueprintCount"), Snapshot.IndexedPluginBlueprintCount);
    Result->SetStringField(TEXT("lastFullBuildUtc"), Snapshot.LastFullBuildUtc);
    Result->SetStringField(TEXT("lastUpdateUtc"), Snapshot.LastUpdateUtc);
    Result->SetNumberField(TEXT("elapsedMilliseconds"), (FDateTime::UtcNow() - StartedAt).GetTotalMilliseconds());
    Response.Result = Result;
    return Response;
}
