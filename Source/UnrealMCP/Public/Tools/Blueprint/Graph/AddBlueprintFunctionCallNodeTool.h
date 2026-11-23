#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FAddBlueprintFunctionCallNodeTool final:public FMCPToolBase{public:FAddBlueprintFunctionCallNodeTool();virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest&Request)const override;protected:virtual TSharedPtr<FJsonObject>BuildInputSchema()const override;};
