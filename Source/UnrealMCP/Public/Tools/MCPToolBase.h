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

    virtual TSharedPtr<FJsonObject> BuildInputSchema() const;

private:
    FString Name;
    FString Description;
};
