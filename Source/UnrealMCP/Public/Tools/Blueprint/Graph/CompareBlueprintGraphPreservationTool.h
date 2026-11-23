#pragma once

#include "Tools/MCPToolBase.h"

class FCompareBlueprintGraphPreservationTool final : public FMCPToolBase
{
public:
    FCompareBlueprintGraphPreservationTool();
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
