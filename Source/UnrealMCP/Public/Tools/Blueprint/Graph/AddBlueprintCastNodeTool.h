#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FAddBlueprintCastNodeTool final:public FMCPToolBase{public:FAddBlueprintCastNodeTool();virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest&Request)const override;protected:virtual TSharedPtr<FJsonObject>BuildInputSchema()const override;};
