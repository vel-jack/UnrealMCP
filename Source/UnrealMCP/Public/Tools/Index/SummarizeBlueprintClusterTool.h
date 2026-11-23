#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FSummarizeBlueprintClusterTool final : public FMCPToolBase
{
public:
    FSummarizeBlueprintClusterTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
