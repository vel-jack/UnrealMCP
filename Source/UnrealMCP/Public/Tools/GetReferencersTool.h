#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FGetReferencersTool final : public FMCPToolBase
{
public:
    FGetReferencersTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
