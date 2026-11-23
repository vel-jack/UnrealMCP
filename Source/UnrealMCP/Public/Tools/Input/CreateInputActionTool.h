#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FCreateInputActionTool final : public FMCPToolBase
{
public:
    FCreateInputActionTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
