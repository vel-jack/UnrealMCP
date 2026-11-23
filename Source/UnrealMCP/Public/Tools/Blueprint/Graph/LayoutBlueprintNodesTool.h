#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FLayoutBlueprintNodesTool final : public FMCPToolBase
{
public:
    FLayoutBlueprintNodesTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
