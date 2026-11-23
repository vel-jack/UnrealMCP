#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FRetargetBlueprintNodeTool final : public FMCPToolBase
{
public:
    FRetargetBlueprintNodeTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
