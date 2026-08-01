#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPProtocol.h"

class FJsonObject;

struct FMCPToolDefinition
{
    FString Name;
    FString Description;
    TSharedPtr<FJsonObject> InputSchema;
};

class IMCPTool
{
public:
    virtual ~IMCPTool() = default;

    virtual FMCPToolDefinition GetDefinition() const = 0;
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const = 0;
};
