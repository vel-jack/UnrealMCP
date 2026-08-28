#include "Tools/GetMutationRequestStatusTool.h"

#include "Dom/JsonObject.h"
#include "MCP/MutationRequestTracker.h"
#include "Tools/BlueprintEditToolUtils.h"

FGetMutationRequestStatusTool::FGetMutationRequestStatusTool()
    : FMCPToolBase(
        TEXT("GetMutationRequestStatus"),
        TEXT("Returns the durable in-editor state and terminal response for one mutation operation ID."))
{
}

UnrealMCP::FMCPResponse FGetMutationRequestStatusTool::Execute(
    const UnrealMCP::FMCPRequest& Request) const
{
    FString OperationId;
    if (!Request.Params.IsValid()
        || !Request.Params->TryGetStringField(TEXT("operationId"), OperationId)
        || OperationId.IsEmpty())
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            TEXT("GetMutationRequestStatus requires operationId."));
    }

    UnrealMCP::FMutationRequestRecord Record;
    if (!UnrealMCP::FMutationRequestTracker::Get().GetRecord(OperationId, Record))
    {
        return BuildError(Request, UnrealMCP::EMCPErrorCode::InvalidParams,
            FString::Printf(TEXT("Mutation operation '%s' was not found in this editor session."), *OperationId));
    }

    const bool bTerminal = UnrealMCP::FMutationRequestTracker::IsTerminal(Record.State);
    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    Result->SetStringField(TEXT("operationId"), Record.OperationId);
    Result->SetStringField(TEXT("toolName"), Record.ToolName);
    Result->SetStringField(TEXT("state"), UnrealMCP::FMutationRequestTracker::StateToString(Record.State));
    Result->SetStringField(TEXT("message"), Record.Message);
    Result->SetStringField(TEXT("createdAtUtc"), Record.CreatedAtUtc.ToIso8601());
    Result->SetStringField(TEXT("updatedAtUtc"), Record.UpdatedAtUtc.ToIso8601());
    Result->SetBoolField(TEXT("terminal"), bTerminal);
    Result->SetBoolField(TEXT("mutationMayStillBeRunning"), !bTerminal);
    if (Record.TerminalResponse.IsSet())
    {
        const UnrealMCP::FMCPResponse& Terminal = Record.TerminalResponse.GetValue();
        if (Terminal.Result.IsValid()) Result->SetObjectField(TEXT("result"), Terminal.Result.ToSharedRef());
        if (Terminal.Error.IsSet())
        {
            TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
            Error->SetNumberField(TEXT("code"), static_cast<int32>(Terminal.Error.GetValue().Code));
            Error->SetStringField(TEXT("message"), Terminal.Error.GetValue().Message);
            if (Terminal.Error.GetValue().Data.IsValid())
                Error->SetObjectField(TEXT("data"), Terminal.Error.GetValue().Data.ToSharedRef());
            Result->SetObjectField(TEXT("error"), Error);
        }
    }

    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;
    Response.Result = Result;
    return Response;
}

TSharedPtr<FJsonObject> FGetMutationRequestStatusTool::BuildInputSchema() const
{
    using namespace UnrealMCP::BlueprintEditToolUtils;
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("operationId"), BuildStringProperty(TEXT("Exact operation ID returned by a mutation request or adapter timeout.")));
    Schema->SetObjectField(TEXT("properties"), Properties);
    Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("operationId"))});
    Schema->SetBoolField(TEXT("additionalProperties"), false);
    return Schema;
}
