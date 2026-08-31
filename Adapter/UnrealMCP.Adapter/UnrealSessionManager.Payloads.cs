using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed partial class UnrealSessionManager
{
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

    private IReadOnlyList<ProjectDescriptor> DiscoverProjects(string? workspaceRoot)
    {
        var projects = _projectDiscovery.DiscoverProjects(workspaceRoot);
        if (IsDefaultWorkspaceRoot(workspaceRoot))
        {
            _lastDiscoveredProjects = projects;
        }

        return projects;
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
}
