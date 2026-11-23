#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FListFoldersTool final : public FMCPToolBase
{
public:
    FListFoldersTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
