#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FConnectBlueprintPinsTool final:public FMCPToolBase{public:FConnectBlueprintPinsTool();virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest&Request)const override;protected:virtual TSharedPtr<FJsonObject>BuildInputSchema()const override;};
