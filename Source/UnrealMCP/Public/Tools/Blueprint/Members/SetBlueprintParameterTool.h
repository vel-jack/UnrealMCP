#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FSetBlueprintParameterTool final : public FMCPToolBase
{
public:
    FSetBlueprintParameterTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
