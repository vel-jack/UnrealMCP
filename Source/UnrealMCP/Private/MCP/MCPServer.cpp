#include "MCP/MCPServer.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

using namespace UnrealMCP;

FMCPServer::FMCPServer() = default;

FMCPToolRegistry& FMCPServer::GetToolRegistry()
{
    return ToolRegistry;
}

const FMCPToolRegistry& FMCPServer::GetToolRegistry() const
{
    return ToolRegistry;
}

FMCPResponse FMCPServer::HandleRequest(const FMCPRequest& Request) const
{
    if (Request.Method.IsEmpty())
    {
        return BuildErrorResponse(Request.Id, EMCPErrorCode::InvalidRequest, TEXT("Request method is required."));
    }

    if (Request.Method == TEXT("tools/list"))
    {
        FMCPResponse Response;
        Response.Id = Request.Id;
        Response.Result = SerializeToolDefinitions();
        return Response;
    }

    if (const IMCPTool* Tool = ToolRegistry.FindTool(Request.Method))
    {
        return Tool->Execute(Request);
    }

    return BuildErrorResponse(Request.Id, EMCPErrorCode::MethodNotFound, FString::Printf(TEXT("Unknown method '%s'."), *Request.Method));
}

bool FMCPServer::ParseJsonRequest(const FString& InboundJson, FMCPRequest& OutRequest, FMCPResponse& OutErrorResponse) const
{
    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InboundJson);
    if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
    {
        OutErrorResponse = BuildErrorResponse(TEXT(""), EMCPErrorCode::ParseError, TEXT("Invalid JSON request payload."));
        return false;
    }

    RootObject->TryGetStringField(JsonKeys::JsonRpc, OutRequest.JsonRpcVersion);
    RootObject->TryGetStringField(JsonKeys::Id, OutRequest.Id);
    RootObject->TryGetStringField(JsonKeys::Method, OutRequest.Method);

    if (RootObject->HasTypedField<EJson::Object>(JsonKeys::Params))
    {
        OutRequest.Params = RootObject->GetObjectField(JsonKeys::Params);
    }

    if (OutRequest.Method.IsEmpty())
    {
        OutErrorResponse = BuildErrorResponse(OutRequest.Id, EMCPErrorCode::InvalidRequest, TEXT("Request method is required."));
        return false;
    }

    return true;
}

FString FMCPServer::SerializeResponse(const FMCPResponse& Response) const
{
    TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
    RootObject->SetStringField(JsonKeys::JsonRpc, Response.JsonRpcVersion);

    if (!Response.Id.IsEmpty())
    {
        RootObject->SetStringField(JsonKeys::Id, Response.Id);
    }

    if (Response.Error.IsSet())
    {
        const FMCPError& Error = Response.Error.GetValue();
        TSharedRef<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
        ErrorObject->SetNumberField(JsonKeys::Code, static_cast<int32>(Error.Code));
        ErrorObject->SetStringField(JsonKeys::Message, Error.Message);
        if (Error.Data.IsValid())
        {
            ErrorObject->SetObjectField(JsonKeys::Data, Error.Data.ToSharedRef());
        }
        RootObject->SetObjectField(JsonKeys::Error, ErrorObject);
    }
    else if (Response.Result.IsValid())
    {
        RootObject->SetObjectField(JsonKeys::Result, Response.Result.ToSharedRef());
    }

    FString Serialized;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    FJsonSerializer::Serialize(RootObject, Writer);
    return Serialized;
}

FMCPResponse FMCPServer::BuildErrorResponse(const FString& RequestId, EMCPErrorCode Code, const FString& Message, TSharedPtr<FJsonObject> Data) const
{
    FMCPResponse Response;
    Response.Id = RequestId;

    FMCPError Error;
    Error.Code = Code;
    Error.Message = Message;
    Error.Data = Data;

    Response.Error = Error;
    return Response;
}

TSharedPtr<FJsonObject> FMCPServer::SerializeToolDefinitions() const
{
    TSharedRef<FJsonObject> ResultObject = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> ToolValues;

    for (const FMCPToolDefinition& Definition : ToolRegistry.ListToolDefinitions())
    {
        TSharedRef<FJsonObject> ToolObject = MakeShared<FJsonObject>();
        ToolObject->SetStringField(JsonKeys::Name, Definition.Name);
        ToolObject->SetStringField(JsonKeys::Description, Definition.Description);
        if (Definition.InputSchema.IsValid())
        {
            ToolObject->SetObjectField(JsonKeys::InputSchema, Definition.InputSchema.ToSharedRef());
        }
        ToolValues.Add(MakeShared<FJsonValueObject>(ToolObject));
    }

    ResultObject->SetArrayField(JsonKeys::Tools, ToolValues);
    ResultObject->SetBoolField(JsonKeys::Success, true);
    return ResultObject;
}
