#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FFindBlueprintTraceStartPointsTool final : public FMCPToolBase
{
public:
    FFindBlueprintTraceStartPointsTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
