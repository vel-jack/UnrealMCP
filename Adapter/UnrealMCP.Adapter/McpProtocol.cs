using System.Text.Json;
using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal static class McpProtocol
{
    public const string AdapterServerName = "unreal-mcp-adapter";
    public const string AdapterServerVersion = "0.1.0";
    public const string ProtocolVersion = "2026-07-31";

    public static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = false
    };

    public static JsonObject CreateInitializeResult()
    {
        return new JsonObject
        {
            ["protocolVersion"] = ProtocolVersion,
            ["capabilities"] = new JsonObject
            {
                ["tools"] = new JsonObject
                {
                    ["listChanged"] = false
                }
            },
            ["serverInfo"] = new JsonObject
            {
                ["name"] = AdapterServerName,
                ["version"] = AdapterServerVersion
            }
        };
    }

    public static JsonObject CreateTextContent(string text)
    {
        return new JsonObject
        {
            ["type"] = "text",
            ["text"] = text
        };
    }

    public static JsonObject CreateToolCallResult(JsonNode structuredContent, bool isError)
    {
        var text = structuredContent.ToJsonString(JsonOptions);
        return new JsonObject
        {
            ["content"] = new JsonArray { CreateTextContent(text) },
            ["structuredContent"] = structuredContent.DeepClone(),
            ["isError"] = isError
        };
    }

    public static JsonObject CreateJsonRpcResponse(JsonNode? id, JsonNode result)
    {
        return new JsonObject
        {
            ["jsonrpc"] = "2.0",
            ["id"] = id?.DeepClone(),
            ["result"] = result.DeepClone()
        };
    }

    public static JsonObject CreateJsonRpcError(JsonNode? id, int code, string message, JsonNode? data = null)
    {
        var error = new JsonObject
        {
            ["code"] = code,
            ["message"] = message
        };

        if (data is not null)
        {
            error["data"] = data.DeepClone();
        }

        return new JsonObject
        {
            ["jsonrpc"] = "2.0",
            ["id"] = id?.DeepClone(),
            ["error"] = error
        };
    }
}
