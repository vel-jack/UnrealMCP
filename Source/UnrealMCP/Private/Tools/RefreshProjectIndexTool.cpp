#include "Tools/RefreshProjectIndexTool.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Index/UnrealMCPProjectIndex.h"
#include "Misc/ScopeExit.h"
#include "UnrealMCPModule.h"

FRefreshProjectIndexTool::FRefreshProjectIndexTool()
    : FMCPToolBase(
        TEXT("RefreshProjectIndex"),
        TEXT("Refreshes only changed, added, deleted, or explicitly requested project assets in the existing UnrealMCP index. This is cheaper than BuildProjectIndex."))
{
}

UnrealMCP::FMCPResponse FRefreshProjectIndexTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    TArray<FString> RequestedObjectPaths;
    if (Request.Params.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* ObjectPathValues = nullptr;
        if (Request.Params->TryGetArrayField(TEXT("objectPaths"), ObjectPathValues) && ObjectPathValues != nullptr)
        {
            for (const TSharedPtr<FJsonValue>& Value : *ObjectPathValues)
            {
                FString ObjectPath;
                if (!Value.IsValid() || !Value->TryGetString(ObjectPath) || ObjectPath.IsEmpty())
                {
                    return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("RefreshProjectIndex params.objectPaths must contain non-empty Blueprint asset object paths."));
                }
                RequestedObjectPaths.AddUnique(ObjectPath);
            }
        }
    }

    const FDateTime StartedAt = FDateTime::UtcNow();
    FUnrealMCPProjectIndex::FRefreshResult RefreshResult;
    bool bSucceeded = false;
    FString Error;

    auto RefreshIndex = [&]()
    {
        bSucceeded = FUnrealMCPModule::Get().GetProjectIndex().RefreshProjectIndex(RequestedObjectPaths, RefreshResult, Error);
    };

    if (IsInGameThread())
    {
        RefreshIndex();
    }
    else
    {
        FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(false);
        if (CompletionEvent == nullptr)
        {
            return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, TEXT("RefreshProjectIndex failed: could not create a synchronization event."));
        }
        ON_SCOPE_EXIT
        {
            FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
        };

        AsyncTask(ENamedThreads::GameThread, [&RefreshIndex, CompletionEvent]()
        {
            RefreshIndex();
            CompletionEvent->Trigger();
        });
        CompletionEvent->Wait();
    }

    if (!bSucceeded)
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, FString::Printf(TEXT("RefreshProjectIndex failed: %s"), *Error));
    }

    const FUnrealMCPProjectIndex::FStatusSnapshot Snapshot = FUnrealMCPModule::Get().GetProjectIndex().GetStatusSnapshot();
    TArray<TSharedPtr<FJsonValue>> RefreshedAssets;
    for (const FString& ObjectPath : RefreshResult.RefreshedAssets)
    {
        RefreshedAssets.Add(MakeShared<FJsonValueString>(ObjectPath));
    }
    TArray<TSharedPtr<FJsonValue>> RemovedAssets;
    for (const FString& ObjectPath : RefreshResult.RemovedAssets)
    {
        RemovedAssets.Add(MakeShared<FJsonValueString>(ObjectPath));
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("mode"), RequestedObjectPaths.Num() > 0 ? TEXT("explicit") : TEXT("auto_detect"));
    Result->SetNumberField(TEXT("candidateCount"), RefreshResult.CandidateCount);
    Result->SetNumberField(TEXT("refreshedAssetCount"), RefreshResult.RefreshedAssetCount);
    Result->SetNumberField(TEXT("removedAssetCount"), RefreshResult.RemovedAssetCount);
    Result->SetNumberField(TEXT("unchangedAssetCount"), RefreshResult.UnchangedAssetCount);
    Result->SetArrayField(TEXT("refreshedAssets"), RefreshedAssets);
    Result->SetArrayField(TEXT("removedAssets"), RemovedAssets);
    Result->SetBoolField(TEXT("hasUsableIndex"), Snapshot.bHasUsableIndex);
    Result->SetBoolField(TEXT("isDirty"), Snapshot.bIndexDirty);
    Result->SetNumberField(TEXT("remainingDirtyAssetCount"), Snapshot.DirtyAssetCount);
    Result->SetNumberField(TEXT("indexedAssetCount"), Snapshot.IndexedAssetCount);
    Result->SetNumberField(TEXT("indexedBlueprintCount"), Snapshot.IndexedBlueprintCount);
    Result->SetStringField(TEXT("lastUpdateUtc"), Snapshot.LastUpdateUtc);
    Result->SetNumberField(TEXT("elapsedMilliseconds"), (FDateTime::UtcNow() - StartedAt).GetTotalMilliseconds());
    Result->SetStringField(TEXT("nextStepHint"), TEXT("Continue indexed queries immediately. Use BuildProjectIndex only when the index is missing, structurally invalid, or a full rebuild is explicitly required."));
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FRefreshProjectIndexTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ObjectPathsProperty = MakeShared<FJsonObject>();
    ObjectPathsProperty->SetStringField(TEXT("type"), TEXT("array"));
    ObjectPathsProperty->SetStringField(TEXT("description"), TEXT("Optional Blueprint asset object paths to refresh. Omit to auto-detect changed, added, and deleted project assets."));
    TSharedRef<FJsonObject> Items = MakeShared<FJsonObject>();
    Items->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathsProperty->SetObjectField(TEXT("items"), Items);
    Properties->SetObjectField(TEXT("objectPaths"), ObjectPathsProperty);

    Schema->SetObjectField(TEXT("properties"), Properties);
    return Schema;
}
