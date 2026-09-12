#include "Tools/MCPToolBase.h"

#include "Dom/JsonObject.h"

FMCPToolBase::FMCPToolBase(FString InName, FString InDescription)
    : Name(MoveTemp(InName))
    , Description(MoveTemp(InDescription))
{
}

FMCPToolDefinition FMCPToolBase::GetDefinition() const
{
    FMCPToolDefinition Definition;
    Definition.Name = Name;
    Definition.Description = Description;
    Definition.InputSchema = BuildInputSchema();
    return Definition;
}

TSharedRef<FJsonObject> FMCPToolBase::BuildBooleanResult(bool bSuccess) const
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), bSuccess);
    return Result;
}

UnrealMCP::FMCPResponse FMCPToolBase::BuildError(const UnrealMCP::FMCPRequest& Request, UnrealMCP::EMCPErrorCode Code, const FString& Message) const
{
    return BuildError(Request, Code, Message, nullptr);
}

UnrealMCP::FMCPResponse FMCPToolBase::BuildError(const UnrealMCP::FMCPRequest& Request, UnrealMCP::EMCPErrorCode Code, const FString& Message, TSharedPtr<FJsonObject> Data) const
{
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    UnrealMCP::FMCPError Error;
    Error.Code = Code;
    Error.Message = Message;
    Error.Data = Data;
    Response.Error = Error;
    return Response;
}

TSharedPtr<FJsonObject> FMCPToolBase::BuildInputSchema() const
{
    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    return Schema;
}
