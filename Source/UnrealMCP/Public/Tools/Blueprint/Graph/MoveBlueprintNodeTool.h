#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FMoveBlueprintNodeTool final : public FMCPToolBase{public:FMoveBlueprintNodeTool();virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest&Request)const override;protected:virtual TSharedPtr<FJsonObject> BuildInputSchema()const override;};
