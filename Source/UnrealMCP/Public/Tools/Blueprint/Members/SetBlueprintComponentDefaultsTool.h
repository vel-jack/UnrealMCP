#pragma once

#include "Tools/MCPToolBase.h"

class FSetBlueprintComponentDefaultsTool final : public FMCPToolBase
{
public:
    FSetBlueprintComponentDefaultsTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
