#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FAddBlueprintFunctionParameterTool final : public FMCPToolBase
{
public:
    FAddBlueprintFunctionParameterTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
