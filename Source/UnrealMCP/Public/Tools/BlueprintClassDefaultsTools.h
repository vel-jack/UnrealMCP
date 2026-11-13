#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FGetBlueprintClassDefaultsTool final : public FMCPToolBase
{
public:
    FGetBlueprintClassDefaultsTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};

class FSetBlueprintClassDefaultsTool final : public FMCPToolBase
{
public:
    FSetBlueprintClassDefaultsTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};

class FResetBlueprintClassDefaultsTool final : public FMCPToolBase
{
public:
    FResetBlueprintClassDefaultsTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};

class FCompareBlueprintClassDefaultsTool final : public FMCPToolBase
{
public:
    FCompareBlueprintClassDefaultsTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
