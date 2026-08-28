#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FSpliceBlueprintExecFlowTool final : public FMCPToolBase
{
public:
    FSpliceBlueprintExecFlowTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
