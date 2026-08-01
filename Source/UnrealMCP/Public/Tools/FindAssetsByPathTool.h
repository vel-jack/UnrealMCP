#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FFindAssetsByPathTool final : public FMCPToolBase
{
public:
    FFindAssetsByPathTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
