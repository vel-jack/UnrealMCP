#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FFindBrokenBlueprintReferencesTool final : public FMCPToolBase
{
public:
    FFindBrokenBlueprintReferencesTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
