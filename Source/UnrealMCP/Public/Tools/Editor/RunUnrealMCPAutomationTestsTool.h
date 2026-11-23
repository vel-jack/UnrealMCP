#pragma once

#include "Tools/MCPToolBase.h"

class FRunUnrealMCPAutomationTestsTool final : public FMCPToolBase
{
public:
    FRunUnrealMCPAutomationTestsTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
