#pragma once

#include "Tools/MCPToolBase.h"

class FWireEnhancedInputActionToComponentTool final : public FMCPToolBase
{
public:
    FWireEnhancedInputActionToComponentTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
