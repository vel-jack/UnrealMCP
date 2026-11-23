#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FGetBlueprintDependenciesTool final : public FMCPToolBase
{
public:
    FGetBlueprintDependenciesTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
