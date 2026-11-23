#pragma once

#include "Tools/MCPToolBase.h"

class FGetBlueprintComponentDefaultsTool final : public FMCPToolBase
{
public:
    FGetBlueprintComponentDefaultsTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
