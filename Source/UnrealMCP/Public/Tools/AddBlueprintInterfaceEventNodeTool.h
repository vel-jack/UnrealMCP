#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FAddBlueprintInterfaceEventNodeTool final : public FMCPToolBase
{
public:
    FAddBlueprintInterfaceEventNodeTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
