#pragma once

#include "Tools/MCPToolBase.h"

class FRefreshBlueprintNodesTool final : public FMCPToolBase
{
public:
    FRefreshBlueprintNodesTool();
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
