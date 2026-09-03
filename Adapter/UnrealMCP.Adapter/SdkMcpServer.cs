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
                    "Unreal-hosted tools execute only after a project is selected and its plugin is attached.";
                options.Capabilities = new ServerCapabilities
                {
                    Tools = new ToolsCapability { ListChanged = true }
                };
            })
            .WithStdioServerTransport()
            .WithListToolsHandler(async (_, requestCancellationToken) =>
            {
                var definitions = await dispatcher.GetToolDefinitionsAsync(requestCancellationToken);
                Interlocked.Exchange(ref publishedCatalogVersion, dispatcher.SessionManager.ToolCatalogVersion);
                return new ListToolsResult
                {
                    Tools = definitions.Select(ToProtocolTool).ToList()
                };
            })
            .WithCallToolHandler(async (context, requestCancellationToken) =>
            {
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
