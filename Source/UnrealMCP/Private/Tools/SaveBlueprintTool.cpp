#include "Tools/SaveBlueprintTool.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"
#include "UObject/Package.h"

FSaveBlueprintTool::FSaveBlueprintTool() : FMCPToolBase(TEXT("SaveBlueprint"), TEXT("Explicitly saves one Blueprint asset, refusing by default to write a Blueprint whose compile is not up to date, and reports its measured package dirty state.")) {}

UnrealMCP::FMCPResponse FSaveBlueprintTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("SaveBlueprint requires objectPath."));

    const bool bRequireUpToDateCompile = UnrealMCP::BlueprintEditToolUtils::GetOptionalBool(
        Request.Params, TEXT("requireUpToDateCompile"), true);

    bool bWasDirty = false;
    bool bIsDirty = false;
    bool bSaved = false;
    bool bValidated = false;
    bool bCompileStatusRejected = false;
    FString BlueprintStatus;
    FString Filename;
    FString IndexRefreshError;
    bool bIndexRefreshed = false;
    FString ExecutionError;
    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        bWasDirty = Blueprint->GetPackage()->IsDirty();
        bValidated = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
        BlueprintStatus = UnrealMCP::BlueprintToolUtils::GetBlueprintStatusString(Blueprint->Status);

        // Writing a Blueprint whose compile is stale or failed persists that state to disk. Match
        // SaveValidatedBlueprints rather than leaving the single-asset path silently unguarded.
        if (bRequireUpToDateCompile && !bValidated)
        {
            bCompileStatusRejected = true;
            OutError = FString::Printf(
                TEXT("Blueprint compile status is '%s', not up to date; compile it before saving or pass requireUpToDateCompile=false to save anyway."),
                *BlueprintStatus);
            return false;
        }

        if (!UnrealMCP::BlueprintEditToolUtils::SaveAsset(Blueprint, Filename, OutError)) return false;
        bSaved = true;
        // Measure the result instead of asserting it: a save that reports success but leaves the
        // package dirty has not done what the caller asked.
        bIsDirty = Blueprint->GetPackage()->IsDirty();
        bIndexRefreshed = UnrealMCP::BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexRefreshError);
        return true;
    }, ExecutionError);
    if (!bSucceeded)
    {
        if (bCompileStatusRejected)
        {
            UnrealMCP::FMCPResponse Response;
            Response.Id = Request.Id;
            TSharedRef<FJsonObject> Result = BuildBooleanResult(false);
            Result->SetStringField(TEXT("objectPath"), ObjectPath);
            Result->SetBoolField(TEXT("wasDirty"), bWasDirty);
            Result->SetBoolField(TEXT("isDirty"), bWasDirty);
            Result->SetBoolField(TEXT("saved"), false);
            Result->SetBoolField(TEXT("validated"), false);
            Result->SetStringField(TEXT("blueprintStatus"), BlueprintStatus);
            Result->SetStringField(TEXT("errorCode"), TEXT("blueprint_compile_not_up_to_date"));
            Result->SetStringField(TEXT("message"), ExecutionError);
            Result->SetStringField(TEXT("recommendedAction"), TEXT("CompileBlueprint, then retry SaveBlueprint. Only set requireUpToDateCompile=false when intentionally persisting the current compile state."));
            Response.Result = Result;
            return Response;
        }
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);
    }

    UnrealMCP::FMCPResponse Response; Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(bSaved && !bIsDirty);
    Result->SetStringField(TEXT("objectPath"), ObjectPath); Result->SetStringField(TEXT("filename"), Filename);
    Result->SetBoolField(TEXT("wasDirty"), bWasDirty); Result->SetBoolField(TEXT("isDirty"), bIsDirty); Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetBoolField(TEXT("validated"), bValidated); Result->SetStringField(TEXT("blueprintStatus"), BlueprintStatus);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed); Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    if (bSaved && bIsDirty)
    {
        Result->SetStringField(TEXT("errorCode"), TEXT("asset_still_dirty_after_save"));
        Result->SetStringField(TEXT("message"), TEXT("SaveAsset reported success but the package is still dirty; treat this asset as unsaved."));
    }
    Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FSaveBlueprintTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("objectPath"), UnrealMCP::BlueprintEditToolUtils::BuildStringProperty(TEXT("Blueprint object path.")));
    Properties->SetObjectField(TEXT("requireUpToDateCompile"), UnrealMCP::BlueprintEditToolUtils::BuildBoolProperty(TEXT("Refuse to save a Blueprint whose compile status is not up to date. Defaults to true.")));
    Schema->SetObjectField(TEXT("properties"), Properties); TArray<TSharedPtr<FJsonValue>> Required{MakeShared<FJsonValueString>(TEXT("objectPath"))}; Schema->SetArrayField(TEXT("required"), Required); return Schema;
}
