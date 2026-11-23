#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FAddBlueprintTypedOperatorNodeTool final : public FMCPToolBase
{
public:
    FAddBlueprintTypedOperatorNodeTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
