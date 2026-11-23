#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FDisconnectBlueprintPinsTool final:public FMCPToolBase{public:FDisconnectBlueprintPinsTool();virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest&Request)const override;protected:virtual TSharedPtr<FJsonObject>BuildInputSchema()const override;};
