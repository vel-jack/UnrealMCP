#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

// Function-graph local variables (R3). One file group for the family; each tool stays a separate class.
class FListBlueprintLocalVariablesTool final : public FMCPToolBase
{
public:
    FListBlueprintLocalVariablesTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};

class FAddBlueprintLocalVariableTool final : public FMCPToolBase
{
public:
    FAddBlueprintLocalVariableTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};

class FRemoveBlueprintLocalVariableTool final : public FMCPToolBase
{
public:
    FRemoveBlueprintLocalVariableTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
