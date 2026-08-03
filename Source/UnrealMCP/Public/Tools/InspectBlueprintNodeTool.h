#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FInspectBlueprintNodeTool final : public FMCPToolBase
{
public:
    FInspectBlueprintNodeTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
