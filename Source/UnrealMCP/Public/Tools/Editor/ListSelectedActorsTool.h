#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FListSelectedActorsTool final : public FMCPToolBase
{
public:
    FListSelectedActorsTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
};
