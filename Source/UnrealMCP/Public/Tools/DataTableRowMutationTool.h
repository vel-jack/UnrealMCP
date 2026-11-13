#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FDataTableRowMutationTool final : public FMCPToolBase
{
public:
    enum class EAction { Set, Add, Remove };
    explicit FDataTableRowMutationTool(EAction InAction);
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;

private:
    EAction Action;
};
