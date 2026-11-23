#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FSaveDataTableTool final : public FMCPToolBase
{
public:
    FSaveDataTableTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
