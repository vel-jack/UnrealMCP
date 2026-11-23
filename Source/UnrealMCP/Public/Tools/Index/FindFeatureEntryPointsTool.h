#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FFindFeatureEntryPointsTool final : public FMCPToolBase
{
public:
    FFindFeatureEntryPointsTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
