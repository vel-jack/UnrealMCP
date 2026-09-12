#include "Tools/SaveValidatedBlueprintsTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "MCP/MutationRequestTracker.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

namespace
{
    using namespace UnrealMCP;

    bool IsBlueprintValidated(const UBlueprint* Blueprint)
    {
        return Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
    }

    // The recovery plan must be derived from what actually happened to each asset. Inferring it from
    // list position (everything after the failure is "not attempted") is only true when the run stops
    // at the first failure; with continueOnFailure=true it mislabels attempted and even already-saved
    // assets as unattempted, and it loses every failure but the last.
    enum class ESaveOutcome : uint8
    {
        NotAttempted,
        SkippedNotDirty,
        DryRunWouldSave,
        Saved,
        Failed
    };

    struct FAssetOutcome
    {
        FString ObjectPath;
        ESaveOutcome Outcome = ESaveOutcome::NotAttempted;
        FString Error;
    };

    FString BlueprintStatusToString(EBlueprintStatus Status)
    {
        switch (Status)
        {
        case BS_Unknown: return TEXT("unknown");
        case BS_Dirty: return TEXT("dirty");
        case BS_Error: return TEXT("error");
        case BS_UpToDate: return TEXT("up_to_date");
        case BS_BeingCreated: return TEXT("being_created");
        case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
        default: return TEXT("unknown");
        }
    }
}

FSaveValidatedBlueprintsTool::FSaveValidatedBlueprintsTool()
    : FMCPToolBase(
        TEXT("SaveValidatedBlueprints"),
        TEXT("Saves an ordered list of already-compiled, dirty Blueprint assets one at a time, stopping at the first asset that is not saveable and reporting which assets are saved versus still dirty and unsaved."))
{
}

UnrealMCP::FMCPResponse FSaveValidatedBlueprintsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;

    const TArray<TSharedPtr<FJsonValue>>* ObjectPathValues = nullptr;
    if (!Request.Params.IsValid() || !Request.Params->TryGetArrayField(TEXT("objectPaths"), ObjectPathValues) || ObjectPathValues == nullptr || ObjectPathValues->IsEmpty())
    {
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("SaveValidatedBlueprints requires a non-empty objectPaths array."));
    }

    TArray<FString> ObjectPaths;
    for (const TSharedPtr<FJsonValue>& Value : *ObjectPathValues)
    {
        FString ObjectPath;
        if (!Value.IsValid() || !Value->TryGetString(ObjectPath) || ObjectPath.IsEmpty())
        {
            return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("SaveValidatedBlueprints objectPaths must be a list of non-empty strings."));
        }
        ObjectPaths.Add(ObjectPath);
    }

    FString OperationId;
    Request.Params->TryGetStringField(TEXT("operationId"), OperationId);
    const bool bDryRun = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("dryRun"), false);
    const bool bContinueOnFailure = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("continueOnFailure"), false);
    const bool bRequireUpToDateCompile = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("requireUpToDateCompile"), true);
    const bool bRefreshIndex = BlueprintEditToolUtils::GetOptionalBool(Request.Params, TEXT("refreshIndex"), true);

    FMutationRequestTracker::Get().Update(OperationId, EMutationRequestState::Preflighting,
        FString::Printf(TEXT("Validating %d Blueprint(s) before any save."), ObjectPaths.Num()));

    TArray<TSharedPtr<FJsonValue>> ResultsJson;
    TArray<FAssetOutcome> Outcomes;
    Outcomes.Reserve(ObjectPaths.Num());
    bool bStopped = false;
    FString ExecutionError;

    BlueprintToolUtils::ExecuteOnGameThreadSync(
        [&](FString& OutError)
        {
            for (int32 Index = 0; Index < ObjectPaths.Num(); ++Index)
            {
                const FString& ObjectPath = ObjectPaths[Index];
                FAssetOutcome& Outcome = Outcomes.AddDefaulted_GetRef();
                Outcome.ObjectPath = ObjectPath;

                TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("objectPath"), ObjectPath);

                if (bStopped)
                {
                    Outcome.Outcome = ESaveOutcome::NotAttempted;
                    Item->SetBoolField(TEXT("attempted"), false);
                    Item->SetBoolField(TEXT("saved"), false);
                    ResultsJson.Add(MakeShared<FJsonValueObject>(Item));
                    continue;
                }

                UBlueprint* Blueprint = nullptr;
                FString ResolveError;
                if (!BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, ResolveError))
                {
                    Outcome.Outcome = ESaveOutcome::Failed;
                    Outcome.Error = ResolveError;
                    Item->SetBoolField(TEXT("attempted"), true);
                    Item->SetBoolField(TEXT("saved"), false);
                    Item->SetStringField(TEXT("error"), ResolveError);
                    ResultsJson.Add(MakeShared<FJsonValueObject>(Item));
                    bStopped = !bContinueOnFailure;
                    continue;
                }

                const bool bWasDirty = Blueprint->GetPackage()->IsDirty();
                const bool bValidated = IsBlueprintValidated(Blueprint);
                Item->SetBoolField(TEXT("wasDirty"), bWasDirty);
                Item->SetBoolField(TEXT("validated"), bValidated);
                Item->SetStringField(TEXT("blueprintStatus"), BlueprintStatusToString(Blueprint->Status));

                if (!bWasDirty)
                {
                    Outcome.Outcome = ESaveOutcome::SkippedNotDirty;
                    Item->SetBoolField(TEXT("attempted"), false);
                    Item->SetBoolField(TEXT("saved"), false);
                    Item->SetStringField(TEXT("skippedReason"), TEXT("Asset is not dirty; nothing to save."));
                    ResultsJson.Add(MakeShared<FJsonValueObject>(Item));
                    continue;
                }

                if (bRequireUpToDateCompile && !bValidated)
                {
                    Outcome.Outcome = ESaveOutcome::Failed;
                    Outcome.Error = TEXT("Blueprint is dirty but not compiled up to date; compile it before saving or pass requireUpToDateCompile=false.");
                    Item->SetBoolField(TEXT("attempted"), true);
                    Item->SetBoolField(TEXT("saved"), false);
                    Item->SetStringField(TEXT("error"), Outcome.Error);
                    ResultsJson.Add(MakeShared<FJsonValueObject>(Item));
                    bStopped = !bContinueOnFailure;
                    continue;
                }

                Item->SetBoolField(TEXT("attempted"), true);
                if (bDryRun)
                {
                    Outcome.Outcome = ESaveOutcome::DryRunWouldSave;
                    Item->SetBoolField(TEXT("saved"), false);
                    Item->SetStringField(TEXT("note"), TEXT("dryRun: this asset would be saved."));
                    ResultsJson.Add(MakeShared<FJsonValueObject>(Item));
                    continue;
                }

                FString SavedFilename, SaveError;
                if (!BlueprintEditToolUtils::SaveAsset(Blueprint, SavedFilename, SaveError))
                {
                    Outcome.Outcome = ESaveOutcome::Failed;
                    Outcome.Error = SaveError;
                    Item->SetBoolField(TEXT("saved"), false);
                    Item->SetStringField(TEXT("error"), SaveError);
                    ResultsJson.Add(MakeShared<FJsonValueObject>(Item));
                    bStopped = !bContinueOnFailure;
                    continue;
                }

                FString IndexRefreshError;
                const bool bIndexRefreshed = bRefreshIndex && BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexRefreshError);
                Item->SetBoolField(TEXT("saved"), true);
                Item->SetStringField(TEXT("savedFilename"), SavedFilename);
                Item->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed);
                Item->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
                ResultsJson.Add(MakeShared<FJsonValueObject>(Item));
                Outcome.Outcome = ESaveOutcome::Saved;
            }
            return true;
        },
        ExecutionError);

    TArray<TSharedPtr<FJsonValue>> SavedJson, NotAttemptedJson, SkippedJson, FailedJson;
    FString FirstFailedObjectPath;
    for (const FAssetOutcome& Outcome : Outcomes)
    {
        switch (Outcome.Outcome)
        {
        case ESaveOutcome::Saved:
            SavedJson.Add(MakeShared<FJsonValueString>(Outcome.ObjectPath));
            break;
        case ESaveOutcome::SkippedNotDirty:
            SkippedJson.Add(MakeShared<FJsonValueString>(Outcome.ObjectPath));
            break;
        case ESaveOutcome::Failed:
        {
            if (FirstFailedObjectPath.IsEmpty())
            {
                FirstFailedObjectPath = Outcome.ObjectPath;
            }
            TSharedRef<FJsonObject> FailedItem = MakeShared<FJsonObject>();
            FailedItem->SetStringField(TEXT("objectPath"), Outcome.ObjectPath);
            FailedItem->SetStringField(TEXT("error"), Outcome.Error);
            FailedJson.Add(MakeShared<FJsonValueObject>(FailedItem));
            break;
        }
        case ESaveOutcome::NotAttempted:
            NotAttemptedJson.Add(MakeShared<FJsonValueString>(Outcome.ObjectPath));
            break;
        case ESaveOutcome::DryRunWouldSave:
        default:
            break;
        }
    }

    const bool bAllSucceeded = FailedJson.IsEmpty();

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(bAllSucceeded);
    Result->SetBoolField(TEXT("dryRun"), bDryRun);
    Result->SetNumberField(TEXT("requestedCount"), ObjectPaths.Num());
    Result->SetNumberField(TEXT("savedCount"), SavedJson.Num());
    Result->SetNumberField(TEXT("failedCount"), FailedJson.Num());
    Result->SetArrayField(TEXT("results"), ResultsJson);

    if (!bAllSucceeded)
    {
        TSharedRef<FJsonObject> RecoveryPlan = MakeShared<FJsonObject>();
        RecoveryPlan->SetArrayField(TEXT("savedObjectPaths"), SavedJson);
        // Every failure is reported, not just the last one, and the first failure is named separately
        // because that is the one that stops the default continueOnFailure=false run.
        RecoveryPlan->SetStringField(TEXT("failedObjectPath"), FirstFailedObjectPath);
        RecoveryPlan->SetArrayField(TEXT("failedObjectPaths"), FailedJson);
        RecoveryPlan->SetArrayField(TEXT("notAttemptedObjectPaths"), NotAttemptedJson);
        RecoveryPlan->SetArrayField(TEXT("skippedCleanObjectPaths"), SkippedJson);
        RecoveryPlan->SetStringField(TEXT("guidance"),
            bDryRun
                ? TEXT("Nothing was written: this was a dry run. Assets in failedObjectPaths would not be saveable as-is; resolve each error before running without dryRun.")
                : TEXT("Assets in savedObjectPaths are saved to disk. Assets in failedObjectPaths and notAttemptedObjectPaths remain dirty and unsaved. Assets in skippedCleanObjectPaths were already clean and needed no save. Resolve each failure and call SaveValidatedBlueprints again with the still-unsaved object paths."));
        Result->SetObjectField(TEXT("recoveryPlan"), RecoveryPlan);
    }

    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FSaveValidatedBlueprintsTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;

    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> ObjectPathsProperty = MakeShared<FJsonObject>();
    ObjectPathsProperty->SetStringField(TEXT("type"), TEXT("array"));
    TSharedRef<FJsonObject> ObjectPathItems = MakeShared<FJsonObject>();
    ObjectPathItems->SetStringField(TEXT("type"), TEXT("string"));
    ObjectPathsProperty->SetObjectField(TEXT("items"), ObjectPathItems);
    ObjectPathsProperty->SetStringField(TEXT("description"), TEXT("Ordered Blueprint object paths to save, typically the owner plus every reconstructed caller from RefreshBlueprintCallSites."));
    Properties->SetObjectField(TEXT("objectPaths"), ObjectPathsProperty);

    Properties->SetObjectField(TEXT("dryRun"), BuildBoolProperty(TEXT("Report which assets would be saved without saving any of them.")));
    Properties->SetObjectField(TEXT("continueOnFailure"), BuildBoolProperty(TEXT("Continue attempting later assets after one fails, instead of stopping immediately. Defaults to false.")));
    Properties->SetObjectField(TEXT("requireUpToDateCompile"), BuildBoolProperty(TEXT("Refuse to save a dirty asset whose Blueprint status is not up to date. Defaults to true.")));
    Properties->SetObjectField(TEXT("refreshIndex"), BuildBoolProperty(TEXT("Partially refresh the project index for each asset saved. Defaults to true.")));
    Properties->SetObjectField(TEXT("operationId"), BuildStringProperty(TEXT("Optional client-assigned ID for mutation-request tracking via GetMutationRequestStatus.")));

    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), { MakeShared<FJsonValueString>(TEXT("objectPaths")) });
    return Schema;
}
