using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed record McpToolDefinition(string Name, string Description, JsonObject InputSchema)
{
    public JsonObject ToJson()
    {
        return new JsonObject
        {
            ["name"] = Name,
            ["description"] = Description,
            ["inputSchema"] = InputSchema.DeepClone()
        };
    }
}
