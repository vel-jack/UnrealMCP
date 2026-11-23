#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

// Asset move/rename, redirector fixup, duplicate and plain delete, mirroring the Content Browser operations.
class FMoveAssetTool final : public FMCPToolBase
{
public:
    FMoveAssetTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};

class FFixupRedirectorsTool final : public FMCPToolBase
{
public:
    FFixupRedirectorsTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};

class FDuplicateAssetTool final : public FMCPToolBase
{
public:
    FDuplicateAssetTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};

class FDeleteAssetTool final : public FMCPToolBase
{
public:
    FDeleteAssetTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
