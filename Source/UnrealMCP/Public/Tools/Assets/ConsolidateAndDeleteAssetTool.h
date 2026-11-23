#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FConsolidateAndDeleteAssetTool final : public FMCPToolBase
{
public:
    FConsolidateAndDeleteAssetTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
