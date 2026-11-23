#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FFindClassUsagesTool final : public FMCPToolBase
{
public:
    FFindClassUsagesTool();

    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};

class FFindBlueprintNodesTool final : public FMCPToolBase
{
public:
    enum class ESearch { PinType, Member };

    explicit FFindBlueprintNodesTool(ESearch InSearch);
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    TSharedPtr<FJsonObject> BuildInputSchema() const override;

private:
    ESearch Search;
};
