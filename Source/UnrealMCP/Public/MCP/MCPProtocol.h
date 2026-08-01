#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UnrealMCP
{
    namespace JsonKeys
    {
        inline constexpr TCHAR JsonRpc[] = TEXT("jsonrpc");
        inline constexpr TCHAR Id[] = TEXT("id");
        inline constexpr TCHAR Method[] = TEXT("method");
        inline constexpr TCHAR Params[] = TEXT("params");
        inline constexpr TCHAR Result[] = TEXT("result");
        inline constexpr TCHAR Error[] = TEXT("error");
        inline constexpr TCHAR Code[] = TEXT("code");
        inline constexpr TCHAR Message[] = TEXT("message");
        inline constexpr TCHAR Data[] = TEXT("data");
        inline constexpr TCHAR Name[] = TEXT("name");
        inline constexpr TCHAR Description[] = TEXT("description");
        inline constexpr TCHAR InputSchema[] = TEXT("inputSchema");
        inline constexpr TCHAR Tools[] = TEXT("tools");
        inline constexpr TCHAR Success[] = TEXT("success");
        inline constexpr TCHAR Capabilities[] = TEXT("capabilities");
        inline constexpr TCHAR ServerInfo[] = TEXT("serverInfo");
        inline constexpr TCHAR Version[] = TEXT("version");
        inline constexpr TCHAR ProtocolVersion[] = TEXT("protocolVersion");
        inline constexpr TCHAR ListChanged[] = TEXT("listChanged");
    }

    enum class EMCPErrorCode : int32
    {
        ParseError = -32700,
        InvalidRequest = -32600,
        MethodNotFound = -32601,
        InvalidParams = -32602,
        InternalError = -32603
    };

    struct FMCPRequest
    {
        FString JsonRpcVersion = TEXT("2.0");
        FString Id;
        FString Method;
        TSharedPtr<FJsonObject> Params;
    };

    struct FMCPError
    {
        EMCPErrorCode Code = EMCPErrorCode::InternalError;
        FString Message;
        TSharedPtr<FJsonObject> Data;
    };

    struct FMCPResponse
    {
        FString JsonRpcVersion = TEXT("2.0");
        FString Id;
        TSharedPtr<FJsonObject> Result;
        TOptional<FMCPError> Error;
    };
}
