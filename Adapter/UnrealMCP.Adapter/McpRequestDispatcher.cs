using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed partial class McpRequestDispatcher(
    AdapterOptions options,
    ToolCatalogService? toolCatalog = null,
    UnrealSessionManager? sessionManager = null)
{
    private readonly UnrealSessionManager _sessionManager = sessionManager ?? new(options);
    private readonly IReadOnlyList<McpToolDefinition> _localTools = BuildLocalTools();
    private readonly ToolCatalogService _toolCatalog = toolCatalog ?? new();

    public UnrealSessionManager SessionManager => _sessionManager;

    public async Task<IReadOnlyList<McpToolDefinition>> GetToolDefinitionsAsync(CancellationToken cancellationToken)
    {
        var all = await GetAllToolDefinitionsAsync(cancellationToken);
        return options.ToolSurface == "full" ? all : all.Where(t => t.Name.StartsWith("unreal.adapter.", StringComparison.Ordinal)).ToArray();
    }

    private async Task<IReadOnlyList<McpToolDefinition>> GetAllToolDefinitionsAsync(CancellationToken cancellationToken)
    {
        var tools = new List<McpToolDefinition>(_localTools);
        var remoteTools = await _sessionManager.GetMirroredToolListAsync(cancellationToken);
        if (remoteTools.Count > 0)
        {
            _toolCatalog.Save(remoteTools);
        }

        var catalog = remoteTools.Count > 0
            ? remoteTools.OfType<JsonObject>().ToArray()
            : _toolCatalog.Load();
        foreach (var remoteTool in catalog)
        {
            var name = remoteTool["name"]?.GetValue<string>();
            if (string.IsNullOrWhiteSpace(name) || tools.Any(tool => tool.Name.Equals(name, StringComparison.Ordinal)))
            {
                continue;
            }

            var description = remoteTool["description"]?.GetValue<string>() ?? string.Empty;
            var inputSchema = (JsonObject)(remoteTool["inputSchema"]?.DeepClone()
                ?? new JsonObject { ["type"] = "object" });
            if (UnrealSessionManager.IsMutationTool(name))
            {
                var properties = inputSchema["properties"] as JsonObject;
                if (properties is null)
                {
                    properties = new JsonObject();
                    inputSchema["properties"] = properties;
                }
                properties["operationId"] = new JsonObject
                {
                    ["type"] = "string",
                    ["description"] = "Optional stable retry ID. Reuse only for an identical request; query GetMutationRequestStatus after an uncertain timeout."
                };
            }
            tools.Add(new McpToolDefinition(name, description, inputSchema));
        }

        tools.AddRange(BuildDiscoveryTools());
        return tools;
    }

    public async Task<(JsonObject Payload, bool IsError)> InvokeToolPayloadAsync(
        string toolName,
        JsonNode? arguments,
        CancellationToken cancellationToken)
    {
        var catalogVersion = _sessionManager.ToolCatalogVersion;
        var discovery = await TryInvokeDiscoveryAsync(toolName, arguments, cancellationToken);
        if (discovery is not null) return discovery.Value;
        var localPayload = await TryInvokeLocalToolPayloadAsync(toolName, arguments, cancellationToken);
        var result = localPayload ?? await _sessionManager.InvokeUnrealToolAsync(toolName, arguments, cancellationToken);
        if (_sessionManager.ToolCatalogVersion != catalogVersion)
        {
            _toolCatalog.Save(_sessionManager.GetCachedToolCatalog());
        }
        return result;
    }

    private async Task<(JsonObject Payload, bool IsError)?> TryInvokeLocalToolPayloadAsync(
        string toolName,
        JsonNode? arguments,
        CancellationToken cancellationToken)
    {
        try
        {
            JsonObject? payload = toolName switch
            {
                "unreal.adapter.GetUnrealStatus" => await _sessionManager.GetUnrealStatusAsync(arguments, cancellationToken),
                "unreal.adapter.GetUnrealSessionInfo" => await _sessionManager.GetSessionInfoAsync(arguments, cancellationToken),
                "unreal.adapter.DiscoverProjects" => await _sessionManager.DiscoverProjectsAsync(arguments, cancellationToken),
                "unreal.adapter.SelectProject" => await _sessionManager.SelectProjectAsync(arguments, cancellationToken),
                "unreal.adapter.ClearSelectedProject" => await _sessionManager.ClearSelectedProjectAsync(cancellationToken),
                "unreal.adapter.ListRunningUnrealSessions" => await _sessionManager.ListRunningSessionsAsync(cancellationToken),
                "unreal.adapter.LaunchUnrealProject" => await _sessionManager.LaunchProjectAsync(arguments, cancellationToken),
                "unreal.adapter.AttachToUnrealProject" => await _sessionManager.AttachToProjectAsync(arguments, cancellationToken),
                "unreal.adapter.ReconnectUnreal" => await _sessionManager.ReconnectAsync(cancellationToken),
                "unreal.adapter.RefreshToolManifest" => await _sessionManager.AttachToProjectAsync(arguments, cancellationToken),
                "unreal.adapter.RequestUnrealShutdown" => await _sessionManager.RequestShutdownAsync(cancellationToken),
                _ => null
            };

            if (payload is null)
            {
                return null;
            }

            var isError = payload["success"] is JsonValue successValue
                && successValue.TryGetValue<bool>(out var success)
                && !success;
            return (payload, isError);
        }
        catch (Exception exception)
        {
            var payload = new JsonObject
            {
                ["success"] = false,
                ["errorCode"] = "adapter_invalid_request",
                ["message"] = exception.Message
            };
            return (payload, true);
        }
    }

    private static IReadOnlyList<McpToolDefinition> BuildLocalTools()
    {
        return
        [
            new McpToolDefinition(
                "unreal.adapter.GetUnrealStatus",
                "Returns whether the selected Unreal project and UnrealMCP pipe are currently available.",
                CreateObjectSchema()),
            new McpToolDefinition(
                "unreal.adapter.GetUnrealSessionInfo",
                "Returns adapter session metadata, including selected project and engine resolution details.",
                CreateObjectSchema()),
            new McpToolDefinition(
                "unreal.adapter.DiscoverProjects",
                "Scans the workspace for .uproject files and returns project metadata including EngineAssociation.",
                CreateObjectSchema(("workspaceRoot", "string", "Optional workspace root override for project discovery.", false))),
            new McpToolDefinition(
                "unreal.adapter.SelectProject",
                "Selects the active Unreal project for this adapter session.",
                CreateObjectSchema(
                    ("projectPath", "string", "Absolute path to the target .uproject file.", true),
                    ("engineExe", "string", "Optional per-project UnrealEditor.exe override.", false))),
            new McpToolDefinition(
                "unreal.adapter.ClearSelectedProject",
                "Clears the currently selected Unreal project.",
                CreateObjectSchema()),
            new McpToolDefinition(
                "unreal.adapter.ListRunningUnrealSessions",
                "Lists currently running Unreal Editor processes.",
                CreateObjectSchema()),
            new McpToolDefinition(
                "unreal.adapter.LaunchUnrealProject",
                "Launches a selected or explicitly provided Unreal project and waits for UnrealMCP to become ready.",
                CreateObjectSchema(
                    ("projectPath", "string", "Optional absolute path to a .uproject file. When provided it becomes the active project.", false),
                    ("engineExe", "string", "Optional UnrealEditor.exe override for this launch request.", false))),
            new McpToolDefinition(
                "unreal.adapter.AttachToUnrealProject",
                "Attaches to the selected Unreal project if it is already running.",
                CreateObjectSchema(("projectPath", "string", "Optional absolute path to a .uproject file to select before attaching.", false))),
            new McpToolDefinition(
                "unreal.adapter.ReconnectUnreal",
                "Clears adapter caches and retries UnrealMCP attachment for the selected project.",
                CreateObjectSchema()),
            new McpToolDefinition(
                "unreal.adapter.RefreshToolManifest",
                "Refreshes the live native tool catalog without launching Unreal. Changed catalogs trigger an MCP tools/list_changed notification; clients must support re-listing tools.",
                CreateObjectSchema(("projectPath", "string", "Optional absolute project path to select before refreshing.", false))),
            new McpToolDefinition(
                "unreal.adapter.RequestUnrealShutdown",
                "Requests a graceful shutdown for the selected Unreal Editor window.",
                CreateObjectSchema())
        ];
    }

    private static JsonObject CreateObjectSchema(params (string Name, string Type, string Description, bool Required)[] properties)
    {
        var schema = new JsonObject
        {
            ["type"] = "object"
        };

        if (properties.Length == 0)
        {
            return schema;
        }

        var propertyMap = new JsonObject();
        var required = new JsonArray();
        foreach (var property in properties)
        {
            propertyMap[property.Name] = new JsonObject
            {
                ["type"] = property.Type,
                ["description"] = property.Description
            };

            if (property.Required)
            {
                required.Add(property.Name);
            }
        }

        schema["properties"] = propertyMap;
        if (required.Count > 0)
        {
            schema["required"] = required;
        }

        return schema;
    }
}
