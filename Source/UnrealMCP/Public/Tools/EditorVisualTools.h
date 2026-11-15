#pragma once

#include "Tools/MCPToolBase.h"

class FGetEditorWidgetSnapshotTool final : public FMCPToolBase
{
public:
    FGetEditorWidgetSnapshotTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};

class FCaptureEditorWidgetTool final : public FMCPToolBase
{
public:
    FCaptureEditorWidgetTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};

class FFrameBlueprintGraphTool final : public FMCPToolBase
{
public:
    FFrameBlueprintGraphTool();
    virtual UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    virtual TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
