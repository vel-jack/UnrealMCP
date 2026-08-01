#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FSearchAssetsTool final : public FMCPToolBase
{
public:
    FSearchAssetsTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
