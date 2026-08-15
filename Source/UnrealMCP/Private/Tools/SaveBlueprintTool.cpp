#include "Tools/SaveBlueprintTool.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FSaveBlueprintTool::FSaveBlueprintTool() : FMCPToolBase(TEXT("SaveBlueprint"), TEXT("Explicitly saves one Blueprint asset and reports its package dirty state.")) {}

UnrealMCP::FMCPResponse FSaveBlueprintTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    FString ObjectPath;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams, TEXT("SaveBlueprint requires objectPath."));

    bool bWasDirty = false;
    FString Filename;
    FString IndexRefreshError;
    bool bIndexRefreshed = false;
    FString ExecutionError;
    const bool bSucceeded = UnrealMCP::BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UBlueprint* Blueprint = nullptr;
        if (!UnrealMCP::BlueprintEditToolUtils::ResolveBlueprint(ObjectPath, Blueprint, OutError)) return false;
        bWasDirty = Blueprint->GetPackage()->IsDirty();
        if (!UnrealMCP::BlueprintEditToolUtils::SaveAsset(Blueprint, Filename, OutError)) return false;
        bIndexRefreshed = UnrealMCP::BlueprintEditToolUtils::RefreshAssetIndex(ObjectPath, IndexRefreshError);
        return true;
    }, ExecutionError);
    if (!bSucceeded) return BuildError(Request, UnrealMCP::EMCPErrorCode::InternalError, ExecutionError);

    UnrealMCP::FMCPResponse Response; Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath); Result->SetStringField(TEXT("filename"), Filename);
    Result->SetBoolField(TEXT("wasDirty"), bWasDirty); Result->SetBoolField(TEXT("isDirty"), false); Result->SetBoolField(TEXT("saved"), true);
    Result->SetBoolField(TEXT("indexRefreshed"), bIndexRefreshed); Result->SetStringField(TEXT("indexRefreshError"), IndexRefreshError);
    Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FSaveBlueprintTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>(); Properties->SetObjectField(TEXT("objectPath"), UnrealMCP::BlueprintEditToolUtils::BuildStringProperty(TEXT("Blueprint object path.")));
    Schema->SetObjectField(TEXT("properties"), Properties); TArray<TSharedPtr<FJsonValue>> Required{MakeShared<FJsonValueString>(TEXT("objectPath"))}; Schema->SetArrayField(TEXT("required"), Required); return Schema;
}
