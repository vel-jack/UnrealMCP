#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FInspectDataTableRowsTool final : public FMCPToolBase
{
public:
    FInspectDataTableRowsTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
