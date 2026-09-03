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
                // Degrade to an empty catalog rather than fail the whole attach: a missing/malformed
                // tools array must not turn a healthy pipe round-trip into a hard "unavailable" error,
                // especially since a non-launch attach (allowLaunch=false) has no retry window left.
                var remoteTools = toolsResponse["result"]?["tools"] as JsonArray ?? [];
                if (!JsonNode.DeepEquals(_cachedRemoteTools, remoteTools))
                {
                    _cachedRemoteTools = remoteTools;
                    Interlocked.Increment(ref _toolCatalogVersion);
                }
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

    private EngineResolutionResult ResolveEngineForSelectedProject(string? explicitEnginePath)
    {
        return _engineResolver.Resolve(explicitEnginePath, _selectedEngineExecutablePath, _options.DefaultEngineExecutablePath, _activeProject?.EngineAssociation);
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
}
