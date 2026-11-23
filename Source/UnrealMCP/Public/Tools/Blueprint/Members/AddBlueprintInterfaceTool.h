#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FAddBlueprintInterfaceTool final : public FMCPToolBase
{
public:
    FAddBlueprintInterfaceTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
