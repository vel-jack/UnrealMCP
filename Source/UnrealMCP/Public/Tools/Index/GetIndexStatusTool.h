#pragma once

#include "Tools/MCPToolBase.h"

class FGetIndexStatusTool final : public FMCPToolBase
{
public:
    FGetIndexStatusTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
};
