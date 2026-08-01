#include "Tools/ListToolsTool.h"

#include "Dom/JsonObject.h"
#include "MCP/MCPToolRegistry.h"

FListToolsTool::FListToolsTool(const FMCPToolRegistry& InRegistry)
    : FMCPToolBase(TEXT("ListTools"), TEXT("Lists all registered Unreal MCP tools and schemas."))
    , Registry(InRegistry)
{
}

UnrealMCP::FMCPResponse FListToolsTool::Execute(const UnrealMCP::FMCPRequest& Request) const
{
    UnrealMCP::FMCPResponse Response;
    Response.Id = Request.Id;

    TSharedRef<FJsonObject> Result = BuildBooleanResult(true);
    TArray<TSharedPtr<FJsonValue>> Tools;

    for (const FMCPToolDefinition& Definition : Registry.ListToolDefinitions())
    {
        TSharedRef<FJsonObject> ToolObject = MakeShared<FJsonObject>();
        ToolObject->SetStringField(TEXT("name"), Definition.Name);
        ToolObject->SetStringField(TEXT("description"), Definition.Description);
        if (Definition.InputSchema.IsValid())
        {
            ToolObject->SetObjectField(TEXT("inputSchema"), Definition.InputSchema.ToSharedRef());
        }
        Tools.Add(MakeShared<FJsonValueObject>(ToolObject));
    }

    Result->SetArrayField(TEXT("tools"), Tools);
    Response.Result = Result;
    return Response;
}
