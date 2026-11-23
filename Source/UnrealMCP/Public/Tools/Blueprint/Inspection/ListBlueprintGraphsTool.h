#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FListBlueprintGraphsTool final : public FMCPToolBase
{
public: FListBlueprintGraphsTool(); virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected: virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
