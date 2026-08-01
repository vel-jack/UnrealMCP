#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPTool.h"

class FMCPToolRegistry
{
public:
    bool RegisterTool(TSharedRef<IMCPTool> Tool);
    const IMCPTool* FindTool(const FString& Name) const;
    TArray<FMCPToolDefinition> ListToolDefinitions() const;

private:
    TMap<FString, TSharedRef<IMCPTool>> ToolsByName;
};
