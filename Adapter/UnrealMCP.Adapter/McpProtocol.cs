using System.Text.Json;

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
}
