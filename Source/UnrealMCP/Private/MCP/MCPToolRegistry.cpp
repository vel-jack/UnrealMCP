#include "MCP/MCPToolRegistry.h"

bool FMCPToolRegistry::RegisterTool(TSharedRef<IMCPTool> Tool)
{
    const FString Name = Tool->GetDefinition().Name;
    if (Name.IsEmpty() || ToolsByName.Contains(Name))
    {
        return false;
    }

    ToolsByName.Add(Name, Tool);
    return true;
}

const IMCPTool* FMCPToolRegistry::FindTool(const FString& Name) const
{
    if (const TSharedRef<IMCPTool>* FoundTool = ToolsByName.Find(Name))
    {
        return &FoundTool->Get();
    }

    return nullptr;
}

TArray<FMCPToolDefinition> FMCPToolRegistry::ListToolDefinitions() const
{
    TArray<FMCPToolDefinition> Definitions;
    Definitions.Reserve(ToolsByName.Num());

    for (const TPair<FString, TSharedRef<IMCPTool>>& Pair : ToolsByName)
    {
        Definitions.Add(Pair.Value->GetDefinition());
    }

    Definitions.Sort([](const FMCPToolDefinition& Left, const FMCPToolDefinition& Right)
    {
        return Left.Name < Right.Name;
    });

    return Definitions;
}
