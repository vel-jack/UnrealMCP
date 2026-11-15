#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FReparentBlueprintTool final : public FMCPToolBase
{
public:
    FReparentBlueprintTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
