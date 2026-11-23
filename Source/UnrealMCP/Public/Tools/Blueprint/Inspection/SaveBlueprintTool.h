#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FSaveBlueprintTool final : public FMCPToolBase
{
public:
    FSaveBlueprintTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
