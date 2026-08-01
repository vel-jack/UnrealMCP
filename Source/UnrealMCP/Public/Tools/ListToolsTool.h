#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FListToolsTool final : public FMCPToolBase
{
public:
    explicit FListToolsTool(const class FMCPToolRegistry& InRegistry);

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

private:
    const FMCPToolRegistry& Registry;
};
