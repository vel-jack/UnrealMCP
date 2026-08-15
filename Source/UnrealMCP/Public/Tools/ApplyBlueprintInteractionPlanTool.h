#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FApplyBlueprintInteractionPlanTool final : public FMCPToolBase
{
public:
    FApplyBlueprintInteractionPlanTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
