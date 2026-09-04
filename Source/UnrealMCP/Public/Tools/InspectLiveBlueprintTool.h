#pragma once

#include "Tools/MCPToolBase.h"

class FInspectLiveBlueprintTool final : public FMCPToolBase
{
public:
    FInspectLiveBlueprintTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
