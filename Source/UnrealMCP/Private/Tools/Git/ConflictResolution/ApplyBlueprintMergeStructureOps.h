#pragma once

#include "Tools/MCPToolBase.h"

// Internal executor. The public merge wrapper verifies Git stages and preview tokens.
class FApplyBlueprintMergeStructureOps final : public FMCPToolBase
{
public:
    FApplyBlueprintMergeStructureOps();
    UnrealMCP::FMCPResponse Execute(const UnrealMCP::FMCPRequest& Request) const override;
};
