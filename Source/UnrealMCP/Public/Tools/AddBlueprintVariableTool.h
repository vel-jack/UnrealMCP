#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FAddBlueprintVariableTool final : public FMCPToolBase
{
public: FAddBlueprintVariableTool(); virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected: virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
