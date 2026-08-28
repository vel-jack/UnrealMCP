#pragma once

#include "Tools/MCPToolBase.h"

class FListUnrealMCPAutomationTestsTool final : public FMCPToolBase
{
public:
    FListUnrealMCPAutomationTestsTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
