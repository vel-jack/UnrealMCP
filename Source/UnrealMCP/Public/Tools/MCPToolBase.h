#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPTool.h"

class FMCPToolBase : public IMCPTool
{
public:
    explicit FMCPToolBase(FString InName, FString InDescription);

    virtual FMCPToolDefinition GetDefinition() const override;

protected:
    TSharedRef<FJsonObject> BuildBooleanResult(bool bSuccess) const;
    UnrealMCP::FMCPResponse BuildError(const UnrealMCP::FMCPRequest& Request, UnrealMCP::EMCPErrorCode Code, const FString& Message) const;
    // Use this overload whenever a tool fails after it has already changed something: the error alone
    // tells the caller nothing about the partial state it now has to reconcile.
    UnrealMCP::FMCPResponse BuildError(const UnrealMCP::FMCPRequest& Request, UnrealMCP::EMCPErrorCode Code, const FString& Message, TSharedPtr<FJsonObject> Data) const;

    virtual TSharedPtr<FJsonObject> BuildInputSchema() const;

private:
    FString Name;
    FString Description;
};
