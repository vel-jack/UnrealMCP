#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FListBlueprintComponentsTool final : public FMCPToolBase
{
public:
    FListBlueprintComponentsTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
