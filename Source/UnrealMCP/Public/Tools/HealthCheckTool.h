#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FHealthCheckTool final : public FMCPToolBase
{
public:
    FHealthCheckTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
};
