#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FAddBlueprintBranchNodeTool final : public FMCPToolBase
{
public: FAddBlueprintBranchNodeTool(); virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected: virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
