#pragma once

#include "Tools/MCPToolBase.h"

class FGetBlueprintOverviewTool final : public FMCPToolBase
{
public:
    FGetBlueprintOverviewTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
