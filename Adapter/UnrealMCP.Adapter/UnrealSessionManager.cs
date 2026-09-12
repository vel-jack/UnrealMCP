using System.Diagnostics;
using System.Runtime.Versioning;
using System.Text.Json.Nodes;

using static UnrealMCP.Adapter.JsonArgumentReaders;

namespace UnrealMCP.Adapter;

[SupportedOSPlatform("windows")]
internal sealed partial class UnrealSessionManager
{
    private readonly AdapterOptions _options;
    private readonly ProjectDiscoveryService _projectDiscovery = new();
    private readonly EngineInstallationResolver _engineResolver = new();

    private ProjectDescriptor? _activeProject;
    private string? _selectedEngineExecutablePath;
    private string? _resolvedEngineExecutablePath;
    private string _sessionState = "project_not_selected";
    private JsonObject? _cachedInitializeResult;
    private JsonArray _cachedRemoteTools = [];
    private DateTimeOffset? _lastAttachUtc;
    private IReadOnlyList<ProjectDescriptor> _lastDiscoveredProjects = [];
    private bool _initialCatalogDiscoveryAttempted;

    // Forwarded to the plugin on every native initialize so the Unreal log can name who is calling.
    // Without it the plugin only ever sees an anonymous connection per request.
    private string? _clientName;
    private string? _clientVersion;

    public void SetConnectedClient(string? name, string? version)
    {
        _clientName = string.IsNullOrWhiteSpace(name) ? null : name;
        _clientVersion = string.IsNullOrWhiteSpace(version) ? null : version;
    }
    private long _toolCatalogVersion;
    private DateTimeOffset? _launchStartedUtc;
    private Process? _launchedProcess;
    private readonly Func<string?, List<Process>>? _processFinderOverride;
    private readonly Func<ProjectDescriptor, EngineResolutionResult, CancellationToken, Task<Process?>>? _projectLauncherOverride;

    public long ToolCatalogVersion => Interlocked.Read(ref _toolCatalogVersion);
    public JsonArray GetCachedToolCatalog() => (JsonArray)_cachedRemoteTools.DeepClone();

    public UnrealSessionManager(
        AdapterOptions options,
        Func<string?, List<Process>>? processFinderOverride = null,
        Func<ProjectDescriptor, EngineResolutionResult, CancellationToken, Task<Process?>>? projectLauncherOverride = null)
    {
        _options = options;
        _processFinderOverride = processFinderOverride;
        _projectLauncherOverride = projectLauncherOverride;

        if (!string.IsNullOrWhiteSpace(options.ProjectPath))
        {
            _activeProject = _projectDiscovery.LoadProject(options.ProjectPath);
        }
    }

    public Task<JsonObject> DiscoverProjectsAsync(JsonNode? arguments, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var workspaceRoot = ReadOptionalString(arguments, "workspaceRoot") ?? GetEffectiveWorkspaceRoot();
        if (!string.IsNullOrWhiteSpace(workspaceRoot))
        {
            workspaceRoot = Path.GetFullPath(workspaceRoot);
        }

        var projects = _projectDiscovery.DiscoverProjects(workspaceRoot);
        if (IsDefaultWorkspaceRoot(workspaceRoot))
        {
            _lastDiscoveredProjects = projects;
        }

        return Task.FromResult(new JsonObject
        {
            ["success"] = true,
            ["workspaceRoot"] = workspaceRoot,
            ["workspaceSource"] = GetWorkspaceSource(workspaceRoot, ReadOptionalString(arguments, "workspaceRoot") is not null),
            ["projectCount"] = projects.Count,
            ["recommendedProjectPath"] = projects.Count == 1 ? projects[0].ProjectPath : null,
            ["projects"] = new JsonArray(projects.Select(project => project.ToJson()).ToArray())
        });
    }

    public Task<JsonObject> SelectProjectAsync(JsonNode? arguments, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var projectPath = ReadRequiredString(arguments, "projectPath");
        var engineExecutablePath = ReadOptionalString(arguments, "engineExe");
        _activeProject = _projectDiscovery.LoadProject(projectPath);
        _selectedEngineExecutablePath = string.IsNullOrWhiteSpace(engineExecutablePath)
            ? null
            : AdapterOptions.NormalizeExecutablePath(engineExecutablePath, "engineExe");
        _resolvedEngineExecutablePath = null;
        _cachedInitializeResult = null;
        _cachedRemoteTools = [];
        _launchStartedUtc = null;
        _launchedProcess = null;
        _sessionState = "project_selected";

        return Task.FromResult(new JsonObject
        {
            ["success"] = true,
            ["projectPath"] = _activeProject.ProjectPath,
            ["projectName"] = _activeProject.ProjectName,
            ["engineAssociation"] = _activeProject.EngineAssociation,
            ["engineExecutablePath"] = _selectedEngineExecutablePath,
            ["message"] = "Selected Unreal project for this adapter session."
        });
    }

    public Task<JsonObject> ClearSelectedProjectAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        _activeProject = null;
        _initialCatalogDiscoveryAttempted = true;
        _selectedEngineExecutablePath = null;
        _resolvedEngineExecutablePath = null;
        _cachedInitializeResult = null;
        _cachedRemoteTools = [];
        _launchStartedUtc = null;
        _launchedProcess = null;
        _sessionState = "project_not_selected";

        return Task.FromResult(new JsonObject
        {
            ["success"] = true,
            ["sessionState"] = _sessionState,
            ["message"] = "Cleared the selected Unreal project."
        });
    }

    public Task<JsonObject> ListRunningSessionsAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var sessions = FindRunningEditorProcesses(null);
        return Task.FromResult(new JsonObject
        {
            ["success"] = true,
            ["sessionCount"] = sessions.Count,
            ["sessions"] = new JsonArray(sessions.Select(ToProcessJson).ToArray())
        });
    }

    public async Task<JsonObject> GetUnrealStatusAsync(JsonNode? arguments, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var attach = await EnsureAttachedAsync(false, arguments, cancellationToken);
        return BuildStatusPayload(attach);
    }

    public async Task<JsonObject> GetSessionInfoAsync(JsonNode? arguments, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var attach = await EnsureAttachedAsync(false, arguments, cancellationToken);
        var engineResolution = ResolveEngineForSelectedProject(null);

        return new JsonObject
        {
            ["projectPath"] = _activeProject?.ProjectPath,
            ["projectName"] = _activeProject?.ProjectName,
            ["engineAssociation"] = _activeProject?.EngineAssociation,
            ["workspaceRoot"] = GetEffectiveWorkspaceRoot(),
            ["workspaceSource"] = GetWorkspaceSource(),
            ["pipeName"] = GetEffectivePipeName(),
            ["pipeSource"] = GetPipeSource(),
            ["defaultEngineExecutablePath"] = _options.DefaultEngineExecutablePath,
            ["selectedEngineExecutablePath"] = _selectedEngineExecutablePath,
            ["resolvedEngineExecutablePath"] = engineResolution.EditorExecutablePath,
            ["engineResolutionSource"] = engineResolution.ResolutionSource,
            ["sessionState"] = attach.SessionState,
            ["attached"] = attach.Ready,
            ["lastAttachUtc"] = _lastAttachUtc?.ToString("O"),
            ["processes"] = new JsonArray(attach.Processes.Select(ToProcessJson).ToArray()),
            ["serverInfo"] = _cachedInitializeResult?["serverInfo"]?.DeepClone(),
            ["remoteToolCount"] = _cachedRemoteTools.Count
        };
    }

    public async Task<JsonObject> LaunchProjectAsync(JsonNode? arguments, CancellationToken cancellationToken)
    {
        var attach = await EnsureAttachedAsync(true, arguments, cancellationToken);
        return BuildStatusPayload(attach);
    }

    public async Task<JsonObject> AttachToProjectAsync(JsonNode? arguments, CancellationToken cancellationToken)
    {
        var attach = await EnsureAttachedAsync(false, arguments, cancellationToken);
        return BuildStatusPayload(attach);
    }

    public async Task<JsonObject> ReconnectAsync(CancellationToken cancellationToken)
    {
        _cachedInitializeResult = null;
        _cachedRemoteTools = [];
        _resolvedEngineExecutablePath = null;
        var attach = await EnsureAttachedAsync(false, null, cancellationToken);
        return BuildStatusPayload(attach);
    }

    public async Task<JsonObject> RequestShutdownAsync(CancellationToken cancellationToken)
    {
        if (_activeProject is null)
        {
            return BuildProjectNotSelectedPayload("RequestUnrealShutdown");
        }

        // Saving is a separate, explicitly selected asset operation. Let Unreal prompt for dirty work.

        var processes = FindRunningEditorProcesses(_activeProject.ProjectName);
        foreach (var process in processes)
        {
            if (!process.HasExited)
            {
                try
                {
                    process.CloseMainWindow();
                }
                catch
                {
                    // Best effort.
                }
            }
        }

        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
        while (DateTime.UtcNow < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (FindRunningEditorProcesses(_activeProject.ProjectName).Count == 0)
            {
                _sessionState = "unreal_not_running";
                _cachedInitializeResult = null;
                _cachedRemoteTools = [];
                return new JsonObject
                {
                    ["success"] = true,
                    ["projectPath"] = _activeProject.ProjectPath,
                    ["sessionState"] = _sessionState,
                    ["message"] = "Requested Unreal Editor shutdown and the selected project is no longer running."
                };
            }

            await Task.Delay(250, cancellationToken);
        }

        return new JsonObject
        {
            ["success"] = false,
            ["projectPath"] = _activeProject.ProjectPath,
            ["sessionState"] = "shutdown_pending",
            ["message"] = "Requested Unreal Editor shutdown, but the process is still running.",
            ["recommendedAction"] = "Close the Unreal Editor window manually or retry RequestUnrealShutdown."
        };
    }

    public async Task<(JsonObject Payload, bool IsError)> InvokeUnrealToolAsync(string toolName, JsonNode? arguments, CancellationToken cancellationToken)
    {
        var attach = await EnsureAttachedAsync(false, null, cancellationToken);
        if (!attach.Ready)
        {
            return (BuildAvailabilityErrorPayload(attach), true);
        }

        JsonObject requestParameters;
        if (arguments is null)
        {
            requestParameters = new JsonObject();
        }
        else if (arguments is JsonObject argumentObject)
        {
            requestParameters = (JsonObject)argumentObject.DeepClone();
        }
        else
        {
            return (new JsonObject
            {
                ["success"] = false,
                ["errorCode"] = "invalid_arguments",
                ["message"] = $"Tool '{toolName}' requires object arguments.",
                ["projectPath"] = _activeProject?.ProjectPath,
                ["pipeName"] = GetEffectivePipeName(),
                ["pipeSource"] = GetPipeSource()
            }, true);
        }

        var isMutation = IsMutationTool(toolName);
        string? operationId = null;
        if (isMutation)
        {
            if (requestParameters["operationId"] is JsonValue operationValue)
            {
                operationValue.TryGetValue(out operationId);
            }
            if (string.IsNullOrWhiteSpace(operationId))
            {
                operationId = Guid.NewGuid().ToString("N");
                requestParameters["operationId"] = operationId;
            }
        }

        try
        {
            var response = await CreatePipeClient().SendRequestAsync(toolName, requestParameters, cancellationToken);
            if (response["error"] is JsonObject error)
            {
                var errorData = error["data"] as JsonObject;
                return (new JsonObject
                {
                    ["success"] = false,
                    ["errorCode"] = error["code"]?.ToString(),
                    ["message"] = error["message"]?.GetValue<string>() ?? $"Unreal tool '{toolName}' failed.",
                    ["operationId"] = errorData?["operationId"]?.DeepClone() ?? operationId,
                    ["state"] = errorData?["state"]?.DeepClone(),
                    ["mutationMayStillBeRunning"] = errorData?["mutationMayStillBeRunning"]?.DeepClone(),
                    ["projectPath"] = _activeProject?.ProjectPath,
                    ["pipeName"] = GetEffectivePipeName(),
                    ["pipeSource"] = GetPipeSource(),
                    ["errorData"] = errorData?.DeepClone()
                }, true);
            }

            var result = response["result"]?.AsObject() ?? new JsonObject();
            var isError = result["success"] is JsonValue successValue &&
                          successValue.TryGetValue<bool>(out var success) &&
                          !success;
            return (result, isError);
        }
        catch (TimeoutException)
        {
            var processes = FindRunningEditorProcesses(_activeProject?.ProjectName);
            return (new JsonObject
            {
                ["success"] = false,
                ["errorCode"] = isMutation ? "mutation_request_timeout" : "unreal_request_timeout",
                ["message"] = $"Timed out while calling Unreal tool '{toolName}'.",
                ["operationId"] = operationId,
                ["mutationMayStillBeRunning"] = isMutation,
                ["canRetry"] = !isMutation,
                ["sessionState"] = _sessionState,
                ["unrealRunning"] = processes.Count > 0,
                ["attached"] = _cachedInitializeResult is not null,
                ["projectPath"] = _activeProject?.ProjectPath,
                ["pipeName"] = GetEffectivePipeName(),
                ["pipeSource"] = GetPipeSource(),
                ["recommendedAction"] = isMutation
                    ? "Call GetMutationRequestStatus with operationId; do not retry the mutation while its state is unknown."
                    : "Retry the read request. ReconnectUnreal only if a subsequent health check fails."
            }, true);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception exception)
        {
            var unavailable = SetUnavailable("unreal_connection_lost", $"Lost connection to Unreal while calling '{toolName}': {exception.Message}", true, "ReconnectUnreal", attach.Processes);
            return (BuildAvailabilityErrorPayload(unavailable), true);
        }
    }

    internal static bool IsMutationTool(string toolName)
    {
        if (toolName.Equals("GetMutationRequestStatus", StringComparison.Ordinal))
        {
            return false;
        }

        string[] mutationPrefixes =
        [
            "Add", "Apply", "Compile", "Connect", "Create", "Delete", "Disconnect",
            "Layout", "Move", "Refresh", "Remove", "RunUnrealMCPAutomationTest", "Save", "Set",
            "Splice", "Wire"
        ];
        return mutationPrefixes.Any(prefix => toolName.StartsWith(prefix, StringComparison.Ordinal));
    }

    public async Task<JsonArray> GetMirroredToolListAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        // EnsureAttachedAsync itself auto-selects a single unambiguous project on the first
        // call from any entry point (not just this one), then attempts to attach.
        await EnsureAttachedAsync(false, null, cancellationToken);

        // The cached catalog is served regardless of attach outcome by design: a failed
        // attach leaves the previous successful catalog in place rather than clearing it,
        // so tool schemas stay discoverable while Unreal is unavailable (see AGENTS.md's
        // "cached authoritative Unreal tool catalog" requirement).
        return (JsonArray)_cachedRemoteTools.DeepClone();
    }
}

internal sealed record AttachResult(
    bool Ready,
    bool RequestSucceeded,
    bool LaunchAccepted,
    string SessionState,
    string? ErrorCode,
    string? Message,
    bool CanRetry,
    string RecommendedAction,
    IReadOnlyList<Process> Processes,
    JsonObject? InitializeResult,
    JsonArray RemoteTools,
    EngineResolutionResult? EngineResolution);
