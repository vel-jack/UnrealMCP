#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FListBlueprintNodePinsTool final:public FMCPToolBase{public:FListBlueprintNodePinsTool();virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest&Request)const override;protected:virtual TSharedPtr<FJsonObject>BuildInputSchema()const override;};
