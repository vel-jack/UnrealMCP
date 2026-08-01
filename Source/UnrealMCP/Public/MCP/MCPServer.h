#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPProtocol.h"
#include "MCP/MCPToolRegistry.h"

class FMCPServer
{
public:
    FMCPServer();

    FMCPToolRegistry& GetToolRegistry();
    const FMCPToolRegistry& GetToolRegistry() const;

    UnrealMCP::FMCPResponse HandleRequest(const UnrealMCP::FMCPRequest& Request) const;
    FString HandleJsonRequest(const FString& InboundJson) const;
    bool ParseJsonRequest(const FString& InboundJson, UnrealMCP::FMCPRequest& OutRequest, UnrealMCP::FMCPResponse& OutErrorResponse) const;
    FString SerializeResponse(const UnrealMCP::FMCPResponse& Response) const;

private:
    UnrealMCP::FMCPResponse BuildErrorResponse(const FString& RequestId, UnrealMCP::EMCPErrorCode Code, const FString& Message, TSharedPtr<FJsonObject> Data = nullptr) const;
    TSharedPtr<FJsonObject> BuildInitializeResult() const;
    TSharedPtr<FJsonObject> SerializeToolDefinitions() const;

    FMCPToolRegistry ToolRegistry;
};
