#pragma once

#include "Tools/MCPToolBase.h"

class FCompareBlueprintMergeStructureTool final : public FMCPToolBase
{
public:
    explicit FCompareBlueprintMergeStructureTool(bool bCompareSnapshots = false);
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    TSharedPtr<FJsonObject> BuildInputSchema() const override;

private:
    bool bCompareSnapshots;
};

class FUnrealMCPGitHistoryTool final : public FMCPToolBase
{
public:
    enum class EOperation { History, Changes, BlueprintGraph };
    explicit FUnrealMCPGitHistoryTool(EOperation InOperation);
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    TSharedPtr<FJsonObject> BuildInputSchema() const override;

private:
    EOperation Operation;
};

class FApplyBlueprintMergeTool final : public FMCPToolBase
{
public:
    enum class EOperation { ClassDefaults, ComponentDefaults, AddInterface, AddVariable, Structure };
    explicit FApplyBlueprintMergeTool(EOperation InOperation = EOperation::ClassDefaults);
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    TSharedPtr<FJsonObject> BuildInputSchema() const override;

private:
    EOperation Operation;
};

class FApplyBlueprintMergePlanTool final : public FMCPToolBase
{
public:
    FApplyBlueprintMergePlanTool();
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;

protected:
    TSharedPtr<FJsonObject> BuildInputSchema() const override;
};

class FBlueprintGitConflictTool final : public FMCPToolBase
{
public:
    explicit FBlueprintGitConflictTool(bool bInStage);
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    TSharedPtr<FJsonObject> BuildInputSchema() const override;
private:
    bool bStage;
};

class FPreviewBlueprintSideReplayTool final : public FMCPToolBase
{
public:
    FPreviewBlueprintSideReplayTool();
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
protected:
    TSharedPtr<FJsonObject> BuildInputSchema() const override;
};
