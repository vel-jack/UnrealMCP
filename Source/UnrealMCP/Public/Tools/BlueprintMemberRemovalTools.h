#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

// Both operations share the same reference, compile and save gates.
class FRemoveBlueprintMemberTool final : public FMCPToolBase
{
public:
    explicit FRemoveBlueprintMemberTool(bool bInFunction);
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
private:
    bool bFunction;
};
