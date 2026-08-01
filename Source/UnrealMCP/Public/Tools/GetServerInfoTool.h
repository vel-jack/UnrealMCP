#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FGetServerInfoTool final : public FMCPToolBase
{
public:
    FGetServerInfoTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
};
