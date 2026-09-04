#pragma once

#include "Tools/MCPToolBase.h"

class FTraceLiveBlueprintFlowTool final : public FMCPToolBase
{
public:
    FTraceLiveBlueprintFlowTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
