#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FSetInputMappingContextMappingKeyTool final : public FMCPToolBase
{
public:
    FSetInputMappingContextMappingKeyTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
