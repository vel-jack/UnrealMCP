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

        return new AttachResult(false, errorCode, errorCode, message, canRetry, recommendedAction, processes, _cachedInitializeResult, _cachedRemoteTools, ResolveEngineForSelectedProject(null));
    }
}
