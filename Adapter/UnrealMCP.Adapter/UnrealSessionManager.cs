using System.Diagnostics;
using System.Runtime.Versioning;
using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

[SupportedOSPlatform("windows")]
internal sealed class UnrealSessionManager
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

    public UnrealSessionManager(AdapterOptions options)
    {
        _options = options;

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
        _selectedEngineExecutablePath = null;
        _resolvedEngineExecutablePath = null;
        _cachedInitializeResult = null;
        _cachedRemoteTools = [];
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
        catch (OperationCanceledException)
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
            "Layout", "Move", "Refresh", "RunUnrealMCPAutomationTest", "Save", "Set",
            "Splice", "Wire"
        ];
        return mutationPrefixes.Any(prefix => toolName.StartsWith(prefix, StringComparison.Ordinal));
    }

    public async Task<JsonArray> GetMirroredToolListAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (_activeProject is null)
        {
            return (JsonArray)_cachedRemoteTools.DeepClone();
        }

        var attach = await EnsureAttachedAsync(false, null, cancellationToken);
        return attach.Ready
            ? (JsonArray)_cachedRemoteTools.DeepClone()
            : (JsonArray)_cachedRemoteTools.DeepClone();
    }

    public JsonObject GetStandaloneDiscoverPayload()
    {
        var workspaceRoot = GetEffectiveWorkspaceRoot();
        var projects = DiscoverProjects(workspaceRoot);
        return new JsonObject
        {
            ["success"] = true,
            ["mode"] = "discover",
            ["workspaceRoot"] = workspaceRoot,
            ["workspaceSource"] = GetWorkspaceSource(),
            ["projects"] = new JsonArray(projects.Select(project => project.ToJson()).ToArray())
        };
    }

    public async Task<JsonObject> GetStandaloneStatusPayloadAsync(CancellationToken cancellationToken)
    {
        return await GetUnrealStatusAsync(null, cancellationToken);
    }

    public async Task<JsonObject> GetStandaloneLaunchPayloadAsync(CancellationToken cancellationToken)
    {
        if (_activeProject is null && string.IsNullOrWhiteSpace(_options.ProjectPath))
        {
            var projects = DiscoverProjects(GetEffectiveWorkspaceRoot());
            if (projects.Count == 1)
            {
                _activeProject = projects[0];
            }
        }

        var arguments = new JsonObject();
        if (!string.IsNullOrWhiteSpace(_options.ProjectPath))
        {
            arguments["projectPath"] = _options.ProjectPath;
        }

        return await LaunchProjectAsync(arguments, cancellationToken);
    }

    private async Task<AttachResult> EnsureAttachedAsync(bool allowLaunch, JsonNode? arguments, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (arguments is JsonObject argumentObject)
        {
            var requestedProjectPath = ReadOptionalString(argumentObject, "projectPath");
            if (!string.IsNullOrWhiteSpace(requestedProjectPath))
            {
                _activeProject = _projectDiscovery.LoadProject(requestedProjectPath);
                _cachedInitializeResult = null;
                _cachedRemoteTools = [];
            }

            var requestedEngineExecutablePath = ReadOptionalString(argumentObject, "engineExe");
            if (!string.IsNullOrWhiteSpace(requestedEngineExecutablePath))
            {
                _selectedEngineExecutablePath = AdapterOptions.NormalizeExecutablePath(requestedEngineExecutablePath, "engineExe");
            }
        }

        if (_activeProject is null)
        {
            return SetUnavailable("project_not_selected", "No Unreal project is selected for this adapter session.", true, "DiscoverProjects or SelectProject", []);
        }

        var engineResolution = ResolveEngineForSelectedProject(ReadOptionalString(arguments, "engineExe"));
        if (!engineResolution.Success)
        {
            return SetUnavailable(engineResolution.ErrorCode ?? "engine_not_found", engineResolution.Message, true, "Provide engineExe or configure --engine-exe", [], engineResolution.ToJson());
        }

        _resolvedEngineExecutablePath = engineResolution.EditorExecutablePath;

        var launchedThisRequest = false;
        var processes = FindRunningEditorProcesses(_activeProject.ProjectName);
        if (processes.Count == 0 && allowLaunch)
        {
            await LaunchProjectInternalAsync(_activeProject, engineResolution, cancellationToken);
            processes = await WaitForMatchingProcessesAsync(_activeProject.ProjectName, _options.LaunchReadyTimeout, cancellationToken);
            launchedThisRequest = processes.Count > 0;
        }

        var readyDeadlineUtc = launchedThisRequest ? DateTime.UtcNow + _options.LaunchReadyTimeout : DateTime.UtcNow;
        Exception? lastTransportException = null;

        while (true)
        {
            try
            {
                var pipeClient = CreatePipeClient();
                var initializeResponse = await pipeClient.SendRequestAsync("initialize", new JsonObject(), cancellationToken);
                if (initializeResponse["error"] is JsonObject initializeError)
                {
                    if (DateTime.UtcNow < readyDeadlineUtc)
                    {
                        await Task.Delay(500, cancellationToken);
                        processes = FindRunningEditorProcesses(_activeProject.ProjectName);
                        continue;
                    }

                    return SetUnavailable(
                        "unreal_mcp_not_ready",
                        initializeError["message"]?.GetValue<string>() ?? "UnrealMCP did not initialize successfully.",
                        true,
                        "ReconnectUnreal",
                        processes,
                        initializeError);
                }

                var toolsResponse = await pipeClient.SendRequestAsync("tools/list", new JsonObject(), cancellationToken);
                if (toolsResponse["error"] is JsonObject toolsError)
                {
                    if (DateTime.UtcNow < readyDeadlineUtc)
                    {
                        await Task.Delay(500, cancellationToken);
                        processes = FindRunningEditorProcesses(_activeProject.ProjectName);
                        continue;
                    }

                    return SetUnavailable(
                        "unreal_mcp_not_ready",
                        toolsError["message"]?.GetValue<string>() ?? "UnrealMCP did not return a tool list.",
                        true,
                        "ReconnectUnreal",
                        processes,
                        toolsError);
                }

                _cachedInitializeResult = initializeResponse["result"]?.AsObject();
                _cachedRemoteTools = toolsResponse["result"]?["tools"]?.AsArray() ?? [];
                _sessionState = "ready";
                _lastAttachUtc = DateTimeOffset.UtcNow;

                return new AttachResult(true, "ready", null, null, false, "Call Unreal tools directly through the adapter.", processes, _cachedInitializeResult, _cachedRemoteTools, engineResolution);
            }
            catch (OperationCanceledException)
            {
                return SetUnavailable("unreal_request_timeout", "Timed out while waiting for UnrealMCP to respond.", true, "ReconnectUnreal", processes);
            }
            catch (TimeoutException exception)
            {
                lastTransportException = exception;
            }
            catch (Exception exception)
            {
                lastTransportException = exception;
            }

            if (DateTime.UtcNow >= readyDeadlineUtc)
            {
                break;
            }

            await Task.Delay(500, cancellationToken);
            processes = FindRunningEditorProcesses(_activeProject.ProjectName);
            if (processes.Count == 0)
            {
                return SetUnavailable("unreal_not_running", "Unreal Editor exited before UnrealMCP became ready.", true, "LaunchUnrealProject", []);
            }
        }

        if (processes.Count == 0)
        {
            return SetUnavailable("unreal_not_running", "Unreal Editor is not running for the selected project.", true, "LaunchUnrealProject", []);
        }

        var lastMessage = lastTransportException?.Message ?? "The UnrealMCP named pipe did not become ready before the timeout elapsed.";
        return SetUnavailable("unreal_mcp_unavailable", $"Unreal Editor is running, but the UnrealMCP pipe is unavailable: {lastMessage}", true, "ReconnectUnreal", processes);
    }

    private IReadOnlyList<ProjectDescriptor> DiscoverProjects(string? workspaceRoot)
    {
        var projects = _projectDiscovery.DiscoverProjects(workspaceRoot);
        if (IsDefaultWorkspaceRoot(workspaceRoot))
        {
            _lastDiscoveredProjects = projects;
        }

        return projects;
    }

    private EngineResolutionResult ResolveEngineForSelectedProject(string? explicitEnginePath)
    {
        return _engineResolver.Resolve(explicitEnginePath, _selectedEngineExecutablePath, _options.DefaultEngineExecutablePath, _activeProject?.EngineAssociation);
    }

    private JsonObject BuildStatusPayload(AttachResult attach)
    {
        var workspaceRoot = GetEffectiveWorkspaceRoot();
        var projects = _lastDiscoveredProjects.Count > 0 ? _lastDiscoveredProjects : DiscoverProjects(workspaceRoot);

        return new JsonObject
        {
            ["success"] = attach.Ready,
            ["projectPath"] = _activeProject?.ProjectPath,
            ["projectName"] = _activeProject?.ProjectName,
            ["engineAssociation"] = _activeProject?.EngineAssociation,
            ["workspaceRoot"] = workspaceRoot,
            ["workspaceSource"] = GetWorkspaceSource(),
            ["pipeName"] = GetEffectivePipeName(),
            ["pipeSource"] = GetPipeSource(),
            ["defaultEngineExecutablePath"] = _options.DefaultEngineExecutablePath,
            ["selectedEngineExecutablePath"] = _selectedEngineExecutablePath,
            ["resolvedEngineExecutablePath"] = attach.EngineResolution?.EditorExecutablePath,
            ["engineResolutionSource"] = attach.EngineResolution?.ResolutionSource,
            ["sessionState"] = attach.SessionState,
            ["unrealRunning"] = attach.Processes.Count > 0,
            ["attached"] = attach.Ready,
            ["canRetry"] = attach.CanRetry,
            ["recommendedAction"] = attach.RecommendedAction,
            ["message"] = attach.Message ?? GetDefaultMessage(attach),
            ["processes"] = new JsonArray(attach.Processes.Select(ToProcessJson).ToArray()),
            ["projectCount"] = projects.Count,
            ["recommendedProjectPath"] = projects.Count == 1 ? projects[0].ProjectPath : null,
            ["projects"] = new JsonArray(projects.Select(project => project.ToJson()).ToArray()),
            ["serverInfo"] = _cachedInitializeResult?["serverInfo"]?.DeepClone()
        };
    }

    private JsonObject BuildAvailabilityErrorPayload(AttachResult attach)
    {
        return new JsonObject
        {
            ["success"] = false,
            ["errorCode"] = attach.ErrorCode,
            ["message"] = attach.Message ?? "UnrealMCP is unavailable.",
            ["projectPath"] = _activeProject?.ProjectPath,
            ["projectName"] = _activeProject?.ProjectName,
            ["workspaceRoot"] = GetEffectiveWorkspaceRoot(),
            ["workspaceSource"] = GetWorkspaceSource(),
            ["pipeName"] = GetEffectivePipeName(),
            ["pipeSource"] = GetPipeSource(),
            ["sessionState"] = attach.SessionState,
            ["canRetry"] = attach.CanRetry,
            ["recommendedAction"] = attach.RecommendedAction,
            ["processes"] = new JsonArray(attach.Processes.Select(ToProcessJson).ToArray()),
            ["engineAssociation"] = _activeProject?.EngineAssociation,
            ["resolvedEngineExecutablePath"] = attach.EngineResolution?.EditorExecutablePath,
            ["engineResolutionSource"] = attach.EngineResolution?.ResolutionSource,
            ["engineResolution"] = attach.EngineResolution?.ToJson()
        };
    }

    private JsonObject BuildProjectNotSelectedPayload(string recommendedAction)
    {
        return new JsonObject
        {
            ["success"] = false,
            ["errorCode"] = "project_not_selected",
            ["sessionState"] = "project_not_selected",
            ["message"] = "No Unreal project is selected for this adapter session.",
            ["recommendedAction"] = recommendedAction,
            ["workspaceRoot"] = GetEffectiveWorkspaceRoot(),
            ["workspaceSource"] = GetWorkspaceSource(),
            ["pipeName"] = GetEffectivePipeName(),
            ["pipeSource"] = GetPipeSource()
        };
    }

    private PipeJsonRpcClient CreatePipeClient()
    {
        return new PipeJsonRpcClient(GetEffectivePipeName(), _options.PipeConnectTimeout, _options.RequestTimeout);
    }

    private string GetEffectivePipeName()
    {
        return PipeNameUtility.GetEffectivePipeName(_options.PipeNameOverride, _activeProject?.ProjectName);
    }

    private string GetPipeSource()
    {
        return string.IsNullOrWhiteSpace(_options.PipeNameOverride) ? "derived_from_project" : "startup_override";
    }

    private string? GetEffectiveWorkspaceRoot()
    {
        if (!string.IsNullOrWhiteSpace(_options.WorkspaceRoot))
        {
            return _options.WorkspaceRoot;
        }

        return Directory.Exists(Environment.CurrentDirectory)
            ? Path.GetFullPath(Environment.CurrentDirectory)
            : null;
    }

    private bool IsDefaultWorkspaceRoot(string? workspaceRoot)
    {
        return string.Equals(workspaceRoot, GetEffectiveWorkspaceRoot(), StringComparison.OrdinalIgnoreCase);
    }

    private string GetWorkspaceSource(string? workspaceRoot = null, bool requestOverrideProvided = false)
    {
        if (requestOverrideProvided)
        {
            return "tool_argument";
        }

        if (!string.IsNullOrWhiteSpace(_options.WorkspaceRoot))
        {
            return "startup_argument";
        }

        workspaceRoot ??= GetEffectiveWorkspaceRoot();
        return workspaceRoot is null ? "unavailable" : "current_directory";
    }

    private async Task LaunchProjectInternalAsync(ProjectDescriptor project, EngineResolutionResult engineResolution, CancellationToken cancellationToken)
    {
        if (FindRunningEditorProcesses(project.ProjectName).Count > 0)
        {
            return;
        }

        var executablePath = engineResolution.EditorExecutablePath;
        if (string.IsNullOrWhiteSpace(executablePath))
        {
            throw new InvalidOperationException("Launch requested without a resolved Unreal Editor executable path.");
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = executablePath,
            UseShellExecute = true
        };
        startInfo.ArgumentList.Add(project.ProjectPath);

        Process.Start(startInfo);
        await Task.Delay(250, cancellationToken);
    }

    private async Task<List<Process>> WaitForMatchingProcessesAsync(string projectName, TimeSpan timeout, CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var processes = FindRunningEditorProcesses(projectName);
            if (processes.Count > 0)
            {
                return processes;
            }

            await Task.Delay(500, cancellationToken);
        }

        return [];
    }

    private List<Process> FindRunningEditorProcesses(string? projectName)
    {
        return Process.GetProcessesByName("UnrealEditor")
            .Where(process =>
            {
                try
                {
                    if (process.HasExited)
                    {
                        return false;
                    }

                    if (string.IsNullOrWhiteSpace(projectName))
                    {
                        return true;
                    }

                    var title = process.MainWindowTitle ?? string.Empty;
                    return title.Contains(projectName, StringComparison.OrdinalIgnoreCase);
                }
                catch
                {
                    return false;
                }
            })
            .OrderBy(process => process.Id)
            .ToList();
    }

    private AttachResult SetUnavailable(
        string errorCode,
        string message,
        bool canRetry,
        string recommendedAction,
        IReadOnlyList<Process> processes,
        JsonObject? rawError = null)
    {
        _sessionState = errorCode;
        if (rawError is not null && rawError["message"] is JsonValue rawMessage)
        {
            message = rawMessage.GetValue<string>();
        }

        return new AttachResult(false, errorCode, errorCode, message, canRetry, recommendedAction, processes, _cachedInitializeResult, _cachedRemoteTools, ResolveEngineForSelectedProject(null));
    }

    private static JsonObject ToProcessJson(Process process)
    {
        return new JsonObject
        {
            ["processId"] = process.Id,
            ["processName"] = process.ProcessName,
            ["windowTitle"] = process.MainWindowTitle
        };
    }

    private static string GetDefaultMessage(AttachResult attach)
    {
        return attach.Ready
            ? "UnrealMCP is attached and ready."
            : attach.SessionState switch
            {
                "project_not_selected" => "No Unreal project is selected for this adapter session.",
                "engine_not_found" => "No Unreal Editor installation could be resolved for the selected project.",
                "engine_resolution_ambiguous" => "Multiple Unreal Editor installs match the selected project.",
                "unreal_not_running" => "Unreal Editor is not running for the selected project.",
                "unreal_mcp_unavailable" => "Unreal Editor is running, but the UnrealMCP named pipe is unavailable.",
                "unreal_mcp_not_ready" => "UnrealMCP is running but not ready to answer requests.",
                "unreal_request_timeout" => "UnrealMCP did not respond before the timeout elapsed.",
                "unreal_connection_lost" => "The connection to UnrealMCP was lost during the request.",
                _ => "UnrealMCP availability is unknown."
            };
    }

    private static string? ReadOptionalString(JsonNode? arguments, string propertyName)
    {
        return arguments is JsonObject argumentObject && argumentObject[propertyName] is JsonValue value
            ? value.GetValue<string>()
            : null;
    }

    private static string ReadRequiredString(JsonNode? arguments, string propertyName)
    {
        var value = ReadOptionalString(arguments, propertyName);
        if (string.IsNullOrWhiteSpace(value))
        {
            throw new InvalidOperationException($"Missing required adapter argument '{propertyName}'.");
        }

        return value;
    }
}

internal sealed record AttachResult(
    bool Ready,
    string SessionState,
    string? ErrorCode,
    string? Message,
    bool CanRetry,
    string RecommendedAction,
    IReadOnlyList<Process> Processes,
    JsonObject? InitializeResult,
    JsonArray RemoteTools,
    EngineResolutionResult? EngineResolution);
