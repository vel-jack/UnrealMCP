#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FSetBlueprintPinDefaultObjectTool final : public FMCPToolBase
{
public:
    FSetBlueprintPinDefaultObjectTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
