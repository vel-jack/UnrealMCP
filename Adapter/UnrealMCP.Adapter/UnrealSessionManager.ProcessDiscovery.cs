using System.Diagnostics;
using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed partial class UnrealSessionManager
{
    private List<Process> FindRunningEditorProcesses(string? projectName)
    {
        return _processFinderOverride?.Invoke(projectName) ?? FindRunningEditorProcessesCore(projectName);
    }

    private static List<Process> FindRunningEditorProcessesCore(string? projectName)
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

    private static JsonObject ToProcessJson(Process process)
    {
        return new JsonObject
        {
            ["processId"] = process.Id,
            ["processName"] = process.ProcessName,
            ["windowTitle"] = process.MainWindowTitle
        };
    }
}
