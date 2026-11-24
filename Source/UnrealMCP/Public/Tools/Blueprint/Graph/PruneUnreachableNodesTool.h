#pragma once

#include "Tools/MCPToolBase.h"

class FPruneUnreachableNodesTool final : public FMCPToolBase
{
public:
    FPruneUnreachableNodesTool();
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
