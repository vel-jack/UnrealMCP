#pragma once
#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FStructMemberValueMigrationTool final : public FMCPToolBase
{
public:
    explicit FStructMemberValueMigrationTool(bool bInRestore = false);
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
private:
    bool bRestore;
};
