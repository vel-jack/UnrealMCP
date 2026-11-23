#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FGetInputMappingContextMappingsTool final : public FMCPToolBase
{
public:
    FGetInputMappingContextMappingsTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
