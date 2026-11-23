#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

enum class EBlueprintMemberGraphEdit { RetargetMember, SwitchCases };
class FBlueprintMemberGraphTool final : public FMCPToolBase
{
public:
    explicit FBlueprintMemberGraphTool(EBlueprintMemberGraphEdit InEdit);
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
private:
    EBlueprintMemberGraphEdit Edit;
};

class FOverrideBlueprintFunctionTool final : public FMCPToolBase
{
public:
    FOverrideBlueprintFunctionTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
