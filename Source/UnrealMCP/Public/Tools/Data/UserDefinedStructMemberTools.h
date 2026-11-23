#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

enum class EUserStructMemberOperation { Inspect, Add, Remove, Rename, SetDefault, InspectEnum, AddEnum, SetEnumDisplayName };
class FUserDefinedStructMemberTool final : public FMCPToolBase
{
public:
    explicit FUserDefinedStructMemberTool(EUserStructMemberOperation InOperation);
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
private:
    EUserStructMemberOperation Operation;
};
