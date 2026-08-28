#pragma once

#include "Tools/MCPToolBase.h"

class FGetMutationRequestStatusTool final : public FMCPToolBase
{
public:
    FGetMutationRequestStatusTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
