using System.Diagnostics;
using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed partial class UnrealSessionManager
{
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

        return new AttachResult(false, false, false, errorCode, errorCode, message, canRetry, recommendedAction, processes, _cachedInitializeResult, _cachedRemoteTools, ResolveEngineForSelectedProject(null));
    }

    private AttachResult SetEditorStarting(IReadOnlyList<Process> processes, EngineResolutionResult engineResolution)
    {
        _sessionState = "editor_starting";
        var elapsed = _launchStartedUtc.HasValue ? DateTimeOffset.UtcNow - _launchStartedUtc.Value : TimeSpan.Zero;
        return new AttachResult(
            false, true, true, "editor_starting", null,
            $"Unreal Editor launch was accepted and the process is running; UnrealMCP is still starting ({elapsed.TotalSeconds:F1}s elapsed).",
            true, "Call GetUnrealStatus; do not launch another Editor process.", processes,
            _cachedInitializeResult, _cachedRemoteTools, engineResolution);
    }
}
