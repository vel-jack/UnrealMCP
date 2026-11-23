#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FAddBlueprintArrayOperationNodeTool final : public FMCPToolBase
{
public:
    FAddBlueprintArrayOperationNodeTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
