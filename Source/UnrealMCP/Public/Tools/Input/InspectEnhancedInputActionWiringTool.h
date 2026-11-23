#pragma once

#include "Tools/MCPToolBase.h"

class FInspectEnhancedInputActionWiringTool final : public FMCPToolBase
{
public:
    FInspectEnhancedInputActionWiringTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
