#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FPlanProjectRefactorTool final : public FMCPToolBase
{
public:
    FPlanProjectRefactorTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
