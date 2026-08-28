#include "MCP/MCPServer.h"

#include "Dom/JsonObject.h"
#include "MCP/MutationRequestTracker.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealMCPSettings.h"

using namespace UnrealMCP;

namespace
{
    FString SerializeCanonicalJsonValue(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("null");
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            TArray<FString> Keys;
            Object->Values.GetKeys(Keys);
            Keys.Sort();
            FString Result = TEXT("{");
            for (int32 Index = 0; Index < Keys.Num(); ++Index)
            {
                FString SerializedKey;
                const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> KeyWriter =
                    TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&SerializedKey);
                KeyWriter->WriteValue(Keys[Index]);
                KeyWriter->Close();
                if (Index > 0) Result += TEXT(",");
                Result += SerializedKey + TEXT(":") + SerializeCanonicalJsonValue(Object->Values.FindRef(Keys[Index]));
            }
            return Result + TEXT("}");
        }
        if (Value->Type == EJson::Array)
        {
            FString Result = TEXT("[");
            const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
            for (int32 Index = 0; Index < Values.Num(); ++Index)
            {
                if (Index > 0) Result += TEXT(",");
                Result += SerializeCanonicalJsonValue(Values[Index]);
            }
            return Result + TEXT("]");
        }

        FString SerializedValue;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&SerializedValue);
        FJsonSerializer::Serialize(Value, FString(), Writer);
        return SerializedValue;
    }

    FString BuildMutationRequestFingerprint(const FMCPRequest& Request)
    {
        const FString SerializedParams = Request.Params.IsValid()
            ? SerializeCanonicalJsonValue(MakeShared<FJsonValueObject>(Request.Params))
            : TEXT("null");
        return FMD5::HashAnsiString(*(Request.Method + TEXT("|") + SerializedParams));
    }

    void AddOperationMetadata(FMCPResponse& Response, const FString& OperationId, const FString& State)
    {
        if (Response.Error.IsSet())
        {
            FMCPError Error = Response.Error.GetValue();
            if (!Error.Data.IsValid())
            {
                Error.Data = MakeShared<FJsonObject>();
            }
            Error.Data->SetStringField(TEXT("operationId"), OperationId);
            Error.Data->SetStringField(TEXT("state"), State);
            Response.Error = MoveTemp(Error);
        }
        else if (Response.Result.IsValid())
        {
            Response.Result->SetStringField(TEXT("operationId"), OperationId);
            Response.Result->SetStringField(TEXT("state"), State);
        }
    }
}

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

    if (Request.Method == TEXT("initialize"))
    {
        FMCPResponse Response;
        Response.Id = Request.Id;
        Response.Result = BuildInitializeResult();
        return Response;
    }

    if (Request.Method == TEXT("ping"))
    {
        FMCPResponse Response;
        Response.Id = Request.Id;
        Response.Result = MakeShared<FJsonObject>();
        Response.Result->SetBoolField(JsonKeys::Success, true);
        return Response;
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
        FString OperationId;
        const bool bTracksMutation = Request.Method != TEXT("GetMutationRequestStatus")
            && Request.Params.IsValid()
            && Request.Params->TryGetStringField(TEXT("operationId"), OperationId)
            && !OperationId.IsEmpty();
        if (!bTracksMutation)
        {
            return Tool->Execute(Request);
        }

        if (OperationId.Len() > 128)
        {
            return BuildErrorResponse(Request.Id, EMCPErrorCode::InvalidParams,
                TEXT("operationId must contain no more than 128 characters."));
        }

        FMutationRequestTracker& Tracker = FMutationRequestTracker::Get();
        FMCPResponse StoredResponse;
        FString ExistingState;
        switch (Tracker.Begin(OperationId, Request.Method, BuildMutationRequestFingerprint(Request), StoredResponse, ExistingState))
        {
        case EMutationBeginResult::ReplayTerminal:
            StoredResponse.Id = Request.Id;
            AddOperationMetadata(StoredResponse, OperationId, ExistingState);
            return StoredResponse;
        case EMutationBeginResult::AlreadyRunning:
        {
            TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetStringField(TEXT("operationId"), OperationId);
            Data->SetStringField(TEXT("state"), ExistingState);
            Data->SetBoolField(TEXT("mutationMayStillBeRunning"), true);
            return BuildErrorResponse(Request.Id, EMCPErrorCode::InvalidRequest,
                TEXT("A mutation with this operationId is already running; query GetMutationRequestStatus before retrying."), Data);
        }
        case EMutationBeginResult::Conflict:
        {
            TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetStringField(TEXT("operationId"), OperationId);
            Data->SetStringField(TEXT("state"), ExistingState);
            return BuildErrorResponse(Request.Id, EMCPErrorCode::InvalidParams,
                TEXT("operationId was already used for a different mutation request."), Data);
        }
        case EMutationBeginResult::Begun:
        default:
            break;
        }

        FMCPResponse Response = Tool->Execute(Request);
        Tracker.Complete(OperationId, Response);
        FMutationRequestRecord CompletedRecord;
        const FString TerminalState = Tracker.GetRecord(OperationId, CompletedRecord)
            ? FMutationRequestTracker::StateToString(CompletedRecord.State)
            : (Response.Error.IsSet() ? TEXT("failed") : TEXT("completed"));
        AddOperationMetadata(Response, OperationId, TerminalState);
        Tracker.Complete(OperationId, Response);
        return Response;
    }

    return BuildErrorResponse(Request.Id, EMCPErrorCode::MethodNotFound, FString::Printf(TEXT("Unknown method '%s'."), *Request.Method));
}

FString FMCPServer::HandleJsonRequest(const FString& InboundJson) const
{
    FMCPRequest Request;
    FMCPResponse ErrorResponse;
    if (!ParseJsonRequest(InboundJson, Request, ErrorResponse))
    {
        return SerializeResponse(ErrorResponse);
    }

    return SerializeResponse(HandleRequest(Request));
}

bool FMCPServer::ParseJsonRequest(const FString& InboundJson, FMCPRequest& OutRequest, FMCPResponse& OutErrorResponse) const
{
    FString SanitizedJson = InboundJson;
    SanitizedJson.TrimStartAndEndInline();
    if (!SanitizedJson.IsEmpty() && SanitizedJson[0] == 0xFEFF)
    {
        SanitizedJson.RightChopInline(1, EAllowShrinking::No);
    }

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SanitizedJson);
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
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
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

TSharedPtr<FJsonObject> FMCPServer::BuildInitializeResult() const
{
    const UUnrealMCPSettings* Settings = GetDefault<UUnrealMCPSettings>();

    TSharedRef<FJsonObject> ResultObject = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> ServerInfoObject = MakeShared<FJsonObject>();
    ServerInfoObject->SetStringField(JsonKeys::Name, Settings->ServerName);
    ServerInfoObject->SetStringField(JsonKeys::Version, Settings->ServerVersion);

    TSharedRef<FJsonObject> CapabilitiesObject = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> ToolsObject = MakeShared<FJsonObject>();
    ToolsObject->SetBoolField(JsonKeys::ListChanged, false);
    CapabilitiesObject->SetObjectField(JsonKeys::Tools, ToolsObject);

    ResultObject->SetBoolField(JsonKeys::Success, true);
    ResultObject->SetStringField(JsonKeys::ProtocolVersion, Settings->ProtocolVersion);
    ResultObject->SetObjectField(JsonKeys::ServerInfo, ServerInfoObject);
    ResultObject->SetObjectField(JsonKeys::Capabilities, CapabilitiesObject);

    return ResultObject;
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
