#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FListBlueprintVariablesTool final : public FMCPToolBase
{
public:
    FListBlueprintVariablesTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
