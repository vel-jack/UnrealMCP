#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FGetActorInfoTool final : public FMCPToolBase
{
public:
    FGetActorInfoTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
