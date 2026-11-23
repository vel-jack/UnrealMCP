#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"
class FAddBlueprintVariableGetNodeTool final:public FMCPToolBase{public:FAddBlueprintVariableGetNodeTool();virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest&Request)const override;protected:virtual TSharedPtr<FJsonObject>BuildInputSchema()const override;};
