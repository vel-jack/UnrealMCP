#pragma once

#include "Tools/MCPToolBase.h"

class FRefreshProjectIndexTool final : public FMCPToolBase
{
public:
    FRefreshProjectIndexTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
