#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FGetAssetInfoTool final : public FMCPToolBase
{
public:
    FGetAssetInfoTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
