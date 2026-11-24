#pragma once

#include "Tools/MCPToolBase.h"

class FWrapWithIsValidTool final : public FMCPToolBase
{
public:
    FWrapWithIsValidTool();
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
