#pragma once

#include "Tools/MCPToolBase.h"

class FBuildProjectIndexTool final : public FMCPToolBase
{
public:
    FBuildProjectIndexTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
};
