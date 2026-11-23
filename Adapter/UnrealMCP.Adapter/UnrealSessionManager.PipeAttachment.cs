using System.Diagnostics;
using System.Text.Json.Nodes;

using static UnrealMCP.Adapter.JsonArgumentReaders;

namespace UnrealMCP.Adapter;

internal sealed partial class UnrealSessionManager
{
    private async Task<AttachResult> EnsureAttachedAsync(bool allowLaunch, JsonNode? arguments, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (arguments is JsonObject argumentObject)
        {
            var requestedProjectPath = ReadOptionalString(argumentObject, "projectPath");
            if (!string.IsNullOrWhiteSpace(requestedProjectPath))
            {
                var project = _projectDiscovery.LoadProject(requestedProjectPath);
                if (!string.Equals(_activeProject?.ProjectPath, project.ProjectPath, StringComparison.OrdinalIgnoreCase))
                {
                    _attachedProcess = null;
                    ClearTrackedLaunch();
                    _cachedInitializeResult = null;
                    _cachedRemoteTools = [];
                }
                _activeProject = project;
            }

            var requestedEngineExecutablePath = ReadOptionalString(argumentObject, "engineExe");
            if (!string.IsNullOrWhiteSpace(requestedEngineExecutablePath))
            {
                _selectedEngineExecutablePath = AdapterOptions.NormalizeExecutablePath(requestedEngineExecutablePath, "engineExe");
            }
        }

        if (_activeProject is null && !_initialCatalogDiscoveryAttempted)
        {
            // Auto-select a single unambiguous project once per adapter process, so a client whose
            // first request is a tool call (not tools/list) can still attach without a manual
            // SelectProject step. Any explicit ClearSelectedProject already marks this attempted so
            // it doesn't override the user's choice to have no active project.
            _initialCatalogDiscoveryAttempted = true;
            var discoveredProjects = DiscoverProjects(GetEffectiveWorkspaceRoot());
            if (discoveredProjects.Count == 1)
            {
                _activeProject = discoveredProjects[0];
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

        var unattended = _options.LaunchUnattended;
        if (allowLaunch && arguments is JsonObject launchArguments && launchArguments.ContainsKey("unattended")
            && (launchArguments["unattended"] is not JsonValue value || !value.TryGetValue(out unattended)))
            return SetUnavailable("invalid_arguments", "unattended must be a boolean.", false, "Use unattended=true for automated startup, false for interactive dialogs.", []);
        var launchedThisRequest = false;
        List<Process> processes;
        try { processes = GetMatchingOrTrackedProcesses(_activeProject.ProjectPath); }
        catch (InvalidOperationException exception)
        {
            return SetUnavailable("editor_identity_unavailable", exception.Message, true,
                "Restore editor process inspection and ReconnectUnreal; no additional editor was launched.", FindRunningEditorProcesses(null));
        }
        if (processes.Count == 0 && allowLaunch)
        {
            _launchedProcess = await LaunchProjectInternalAsync(_activeProject, engineResolution, unattended, cancellationToken);
            launchedThisRequest = _launchedProcess is not null;
            _launchStartedUtc = DateTimeOffset.UtcNow;
            processes = GetMatchingOrTrackedProcesses(_activeProject.ProjectPath);
        }
        if (processes.Count == 0)
        {
            ClearTrackedLaunch();
            return SetUnavailable("unreal_not_running", "Unreal Editor is not running for the selected project.", true, "LaunchUnrealProject", []);
        }

        JsonObject? nativeError = null;
        Exception? transportException = null;
        try
        {
            var pipeClient = CreatePipeClient(processes);
            var initializeResponse = await pipeClient.SendRequestAsync("initialize", BuildInitializeParameters(), cancellationToken);
            nativeError = initializeResponse["error"] as JsonObject;
            if (nativeError is null)
            {
                var toolsResponse = await pipeClient.SendRequestAsync("tools/list", new JsonObject(), cancellationToken);
                nativeError = toolsResponse["error"] as JsonObject;
                if (nativeError is null)
                {
                    _cachedInitializeResult = initializeResponse["result"]?.AsObject();
                    var remoteTools = toolsResponse["result"]?["tools"] as JsonArray ?? [];
                    if (!JsonNode.DeepEquals(_cachedRemoteTools, remoteTools))
                    {
                        _cachedRemoteTools = remoteTools;
                        Interlocked.Increment(ref _toolCatalogVersion);
                    }
                    _attachedProcess = processes.First(p => p.Id == pipeClient.ServerProcessId);
                    _sessionState = "ready";
                    _lastAttachUtc = DateTimeOffset.UtcNow;
                    ClearTrackedLaunch();
                    return new AttachResult(true, true, launchedThisRequest, "ready", null, null, false,
                        "Call Unreal tools directly through the adapter.", processes, _cachedInitializeResult,
                        _cachedRemoteTools, engineResolution);
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (PipeIdentityException exception)
        {
            return SetUnavailable("project_identity_mismatch", exception.Message, false,
                "Check selected project and pipe configuration; do not launch a duplicate editor.", processes);
        }
        catch (Exception exception)
        {
            transportException = exception;
        }

        processes = GetMatchingOrTrackedProcesses(_activeProject.ProjectPath);
        if (processes.Count == 0)
        {
            ClearTrackedLaunch();
            return SetUnavailable("unreal_not_running", "Unreal Editor exited before UnrealMCP became ready.", true, "LaunchUnrealProject", []);
        }
        if (_launchStartedUtc.HasValue && DateTimeOffset.UtcNow - _launchStartedUtc.Value < _options.LaunchReadyTimeout)
            return SetEditorStarting(processes, engineResolution);

        ClearTrackedLaunch();
        if (nativeError is not null)
            return SetUnavailable("unreal_mcp_not_ready", nativeError["message"]?.GetValue<string>() ??
                "UnrealMCP did not initialize successfully.", true, "ReconnectUnreal", processes, nativeError);
        var lastMessage = transportException?.Message ?? "The UnrealMCP named pipe did not become ready before the timeout elapsed.";
        return SetUnavailable("unreal_mcp_unavailable", $"Unreal Editor is running, but the UnrealMCP pipe is unavailable: {lastMessage}", true, "ReconnectUnreal", processes);
    }

    // Identifies the caller to the plugin. The adapter reconnects per request, so this is the only
    // place the Unreal log can learn who is driving it; the plugin logs it once per distinct client.
    private JsonObject BuildInitializeParameters()
    {
        var parameters = new JsonObject
        {
            ["adapter"] = $"{McpProtocol.AdapterServerName} {McpProtocol.AdapterServerVersion}"
        };

        if (_clientName is not null)
        {
            parameters["clientInfo"] = new JsonObject
            {
                ["name"] = _clientName,
                ["version"] = _clientVersion
            };
        }

        return parameters;
    }

    private EngineResolutionResult ResolveEngineForSelectedProject(string? explicitEnginePath)
    {
        return _engineResolver.Resolve(explicitEnginePath, _selectedEngineExecutablePath, _options.DefaultEngineExecutablePath, _activeProject?.EngineAssociation);
    }

    private List<Process> GetMatchingOrTrackedProcesses(string projectPath)
    {
        // A successful attachment retains the exact process handle across tab/title and pipe changes.
        if (_attachedProcess is not null && IsRunning(_attachedProcess)) return [_attachedProcess];
        _attachedProcess = null;
        List<Process> matches;
        try { matches = FindRunningEditorProcesses(projectPath); }
        catch (InvalidOperationException) when (_launchedProcess is not null && IsRunning(_launchedProcess))
        { return [_launchedProcess]; }
        if (matches.Count == 0 && _launchedProcess is not null && IsRunning(_launchedProcess)) matches.Add(_launchedProcess);
        return matches;
    }

    private void ClearTrackedLaunch()
    {
        _launchStartedUtc = null;
        _launchedProcess = null;
    }

    private async Task<Process?> LaunchProjectInternalAsync(ProjectDescriptor project, EngineResolutionResult engineResolution, bool unattended, CancellationToken cancellationToken)
    {
        var existing = FindRunningEditorProcesses(project.ProjectPath);
        if (existing.Count > 0) return existing[0];
        if (_projectLauncherOverride is not null)
            return await _projectLauncherOverride(project, engineResolution, cancellationToken);

        var executablePath = engineResolution.EditorExecutablePath;
        if (string.IsNullOrWhiteSpace(executablePath))
        {
            throw new InvalidOperationException("Launch requested without a resolved Unreal Editor executable path.");
        }

        var startInfo = BuildEditorStartInfo(executablePath, project.ProjectPath, unattended);

        var process = Process.Start(startInfo);
        await Task.Delay(250, cancellationToken);
        return process;
    }
    internal static ProcessStartInfo BuildEditorStartInfo(string executablePath, string projectPath, bool unattended)
    {
        var startInfo = new ProcessStartInfo { FileName = executablePath, UseShellExecute = true };
        startInfo.ArgumentList.Add(projectPath);
        if (unattended) startInfo.ArgumentList.Add("-unattended");
        return startInfo;
    }
}
