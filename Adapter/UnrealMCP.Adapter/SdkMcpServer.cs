using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using ModelContextProtocol.Protocol;
using ModelContextProtocol.Server;

namespace UnrealMCP.Adapter;

internal static class SdkMcpServer
{
    public static async Task RunAsync(McpRequestDispatcher dispatcher, CancellationToken cancellationToken)
    {
        var builder = Host.CreateApplicationBuilder([]);
        long publishedCatalogVersion = -1;
        builder.Logging.ClearProviders();
        builder.Logging.AddConsole(options => options.LogToStandardErrorThreshold = LogLevel.Trace);

        builder.Services
            .AddMcpServer(options =>
            {
                options.ServerInfo = new Implementation
                {
                    Name = McpProtocol.AdapterServerName,
                    Version = McpProtocol.AdapterServerVersion
                };
                options.ServerInstructions =
                    "UnrealMCP adapter lifecycle tools remain available when Unreal Editor is closed. " +
                    "Unreal-hosted tools execute only after a project is selected and its plugin is attached. " +
                    "Start with unreal.adapter.SearchTools using task keywords, GetToolSchema for exact inputs, then CallTool. " +
                    "Prefer GetBlueprintOverview before detailed node/pin inspection and declarative graph patches over many primitive edits. " +
                    "Discovery uses cached schemas while offline; it does not prove a tool is available in the attached project. " +
                    "Respect coverage/truncation and live_editor versus cached_index evidence. After mutation timeout query GetMutationRequestStatus using the original operationId; never blindly retry. " +
                    "Map/level editing is deferred. PIE and visual acceptance remain user-controlled.";
                options.Capabilities = new ServerCapabilities
                {
                    Tools = new ToolsCapability { ListChanged = true }
                };
            })
            .WithStdioServerTransport()
            .WithListToolsHandler(async (context, requestCancellationToken) =>
            {
                CaptureClientIdentity(dispatcher, context.Server.ClientInfo);
                var definitions = await dispatcher.GetToolDefinitionsAsync(requestCancellationToken);
                Interlocked.Exchange(ref publishedCatalogVersion, dispatcher.SessionManager.ToolCatalogVersion);
                return new ListToolsResult
                {
                    Tools = definitions.Select(ToProtocolTool).ToList()
                };
            })
            .WithCallToolHandler(async (context, requestCancellationToken) =>
            {
                CaptureClientIdentity(dispatcher, context.Server.ClientInfo);
                var parameters = context.Params ?? throw new InvalidOperationException("tools/call params are required.");
                JsonNode? arguments = parameters.Arguments is null
                    ? new JsonObject()
                    : JsonSerializer.SerializeToNode(parameters.Arguments, McpProtocol.JsonOptions);
                var (payload, isError) = await dispatcher.InvokeToolPayloadAsync(
                    parameters.Name,
                    arguments,
                    requestCancellationToken);

                var catalogVersion = dispatcher.SessionManager.ToolCatalogVersion;
                var previousVersion = Interlocked.Exchange(ref publishedCatalogVersion, catalogVersion);
                if (previousVersion != catalogVersion)
                {
                    try
                    {
                        await context.Server.SendNotificationAsync(
                            NotificationMethods.ToolListChangedNotification, requestCancellationToken);
                    }
                    catch (Exception exception)
                    {
                        // Notification failure must not turn a completed mutation into a tool failure.
                        Interlocked.CompareExchange(ref publishedCatalogVersion, previousVersion, catalogVersion);
                        Console.Error.WriteLine($"Could not notify tool catalog change: {exception.Message}");
                    }
                }

                return new CallToolResult
                {
                    Content =
                    [
                        new TextContentBlock
                        {
                            Text = payload.ToJsonString(McpProtocol.JsonOptions)
                        }
                    ],
                    StructuredContent = JsonSerializer.SerializeToElement(payload, McpProtocol.JsonOptions),
                    IsError = isError
                };
            });

        await builder.Build().RunAsync(cancellationToken);
    }

    // The MCP client identifies itself once, during the SDK handshake. Capture it here so the plugin
    // can name the caller in the Unreal log instead of reporting an anonymous pipe connection.
    private static void CaptureClientIdentity(McpRequestDispatcher dispatcher, Implementation? clientInfo)
    {
        if (clientInfo is not null)
        {
            dispatcher.SessionManager.SetConnectedClient(clientInfo.Name, clientInfo.Version);
        }
    }

    private static Tool ToProtocolTool(McpToolDefinition definition)
    {
        return new Tool
        {
            Name = definition.Name,
            Description = definition.Description,
            InputSchema = JsonSerializer.SerializeToElement(definition.InputSchema, McpProtocol.JsonOptions)
        };
    }
}
