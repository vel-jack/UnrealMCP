#pragma once

#include "Tools/MCPToolBase.h"

class FRunUnrealMCPAutomationTestTool final : public FMCPToolBase
{
public:
    FRunUnrealMCPAutomationTestTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
