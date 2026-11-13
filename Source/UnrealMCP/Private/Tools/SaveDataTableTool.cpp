#include "Tools/SaveDataTableTool.h"

#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "Tools/BlueprintEditToolUtils.h"
#include "Tools/BlueprintToolUtils.h"

FSaveDataTableTool::FSaveDataTableTool()
    : FMCPToolBase(TEXT("SaveDataTable"), TEXT("Saves exactly one DataTable package after explicit row edits and reports the resulting dirty state."))
{
}

UnrealMCP::FMCPResponse FSaveDataTableTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    using namespace UnrealMCP;
    FString ObjectPath;
    if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
        return BuildError(Request, EMCPErrorCode::InvalidParams, TEXT("SaveDataTable requires objectPath."));
    FString Filename, Error;
    bool bWasDirty = false, bIsDirty = false;
    const bool bOk = BlueprintToolUtils::ExecuteOnGameThreadSync([&](FString& OutError)
    {
        UDataTable* Table = LoadObject<UDataTable>(nullptr, *ObjectPath);
        if (!Table) { OutError = TEXT("DataTable not found."); return false; }
        bWasDirty = Table->GetOutermost()->IsDirty();
        if (bWasDirty && !BlueprintEditToolUtils::SaveAsset(Table, Filename, OutError)) return false;
        bIsDirty = Table->GetOutermost()->IsDirty();
        if (bIsDirty) { OutError = TEXT("DataTable package remains dirty after save."); return false; }
        return true;
    }, Error);
    if (!bOk) return BuildError(Request, EMCPErrorCode::InternalError, Error);
    FMCPResponse Response; Response.Id = Request.Id;
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    Result->SetBoolField(TEXT("wasDirty"), bWasDirty);
    Result->SetBoolField(TEXT("saved"), bWasDirty);
    Result->SetBoolField(TEXT("isDirty"), bIsDirty);
    Result->SetStringField(TEXT("filename"), Filename);
    Response.Result = Result; return Response;
}

TSharedPtr<FJsonObject> FSaveDataTableTool::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>(); Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> Path = MakeShared<FJsonObject>(); Path->SetStringField(TEXT("type"), TEXT("string"));
    Properties->SetObjectField(TEXT("objectPath"), Path);
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("objectPath"))});
    return Schema;
}
