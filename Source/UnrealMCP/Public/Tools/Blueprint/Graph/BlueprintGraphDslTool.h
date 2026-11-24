#pragma once

#include "Tools/MCPToolBase.h"

class FBlueprintGraphDslTool final : public FMCPToolBase
{
public:
    explicit FBlueprintGraphDslTool(bool bInWrite);
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    TSharedPtr<FJsonObject> BuildInputSchema() const override;

private:
    bool bWrite;
};
