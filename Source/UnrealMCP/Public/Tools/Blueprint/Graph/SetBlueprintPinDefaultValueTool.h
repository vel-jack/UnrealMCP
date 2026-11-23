#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FSetBlueprintPinDefaultValueTool final:public FMCPToolBase{public:FSetBlueprintPinDefaultValueTool();virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest&Request)const override;protected:virtual TSharedPtr<FJsonObject>BuildInputSchema()const override;};
