#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FGetLevelActorDependenciesTool final : public FMCPToolBase
{
public:
    FGetLevelActorDependenciesTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
