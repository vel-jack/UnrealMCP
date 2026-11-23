#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FAssetExistsTool final : public FMCPToolBase
{
public:
    FAssetExistsTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
