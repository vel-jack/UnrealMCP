#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FWireSelectionWorkflowTool final : public FMCPToolBase
{
public:
    FWireSelectionWorkflowTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
