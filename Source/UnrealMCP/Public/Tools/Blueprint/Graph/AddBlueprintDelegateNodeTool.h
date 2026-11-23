#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FAddBlueprintDelegateNodeTool final : public FMCPToolBase
{
public:
    FAddBlueprintDelegateNodeTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
