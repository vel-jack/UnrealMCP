#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FAddBlueprintVariableSetNodeTool final:public FMCPToolBase{public:FAddBlueprintVariableSetNodeTool();virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest&Request)const override;protected:virtual TSharedPtr<FJsonObject>BuildInputSchema()const override;};
