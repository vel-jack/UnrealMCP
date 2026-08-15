#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FValidateBlueprintTool final : public FMCPToolBase
{
public: FValidateBlueprintTool(); virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected: virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
