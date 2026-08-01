#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FFindAssetsByClassTool final : public FMCPToolBase
{
public:
    FFindAssetsByClassTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
