using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed partial class UnrealSessionManager
{
    private List<Process> FindRunningEditorProcesses(string? projectPath) =>
        _processFinderOverride?.Invoke(projectPath) ?? FindRunningEditorProcessesCore(projectPath);

    private static List<Process> FindRunningEditorProcessesCore(string? projectPath)
    {
        var processes = Process.GetProcessesByName("UnrealEditor").Where(IsRunning).OrderBy(p => p.Id).ToList();
        if (string.IsNullOrWhiteSpace(projectPath) || processes.Count == 0) return processes;
        var commandLines = ReadEditorCommandLines();
        var matches = processes.Where(p => commandLines.TryGetValue(p.Id, out var commandLine)
            && MatchesProjectCommandLine(commandLine, projectPath)).ToList();
        if (matches.Count == 0 && processes.Any(p => !commandLines.ContainsKey(p.Id) && IsRunning(p)))
            throw new InvalidOperationException("Cannot verify the project identity of a running Unreal Editor. No additional editor will be launched.");
        return matches;
    }

    private static Dictionary<int, string> ReadEditorCommandLines()
    {
        object? service = null, results = null;
        var commandLines = new Dictionary<int, string>();
        try
        {
            // WMI reads exact project arguments even when the editor has no main window or changes tabs.
            service = Marshal.BindToMoniker("winmgmts:root\\cimv2");
            results = ((dynamic)service).ExecQuery("SELECT ProcessId, CommandLine FROM Win32_Process WHERE Name='UnrealEditor.exe'");
            foreach (var row in (dynamic)results)
            {
                try
                {
                    dynamic properties = row.Properties_;
                    dynamic commandLineProperty = properties.Item("CommandLine");
                    dynamic idProperty = properties.Item("ProcessId");
                    try
                    {
                        string? commandLine = commandLineProperty.Value;
                        if (!string.IsNullOrWhiteSpace(commandLine)) commandLines[Convert.ToInt32(idProperty.Value)] = commandLine;
                    }
                    finally
                    {
                        Marshal.ReleaseComObject(idProperty); Marshal.ReleaseComObject(commandLineProperty); Marshal.ReleaseComObject(properties);
                    }
                }
                finally { if (Marshal.IsComObject(row)) Marshal.ReleaseComObject(row); }
            }
            return commandLines;
        }
        catch (Exception exception)
        {
            throw new InvalidOperationException("Unreal Editor project identity could not be inspected. No additional editor will be launched.", exception);
        }
        finally
        {
            if (results is not null && Marshal.IsComObject(results)) Marshal.ReleaseComObject(results);
            if (service is not null && Marshal.IsComObject(service)) Marshal.ReleaseComObject(service);
        }
    }

    internal static bool MatchesProjectCommandLine(string commandLine, string projectPath)
    {
        var argv = CommandLineToArgvW(commandLine, out var count);
        if (argv == IntPtr.Zero) return false;
        try
        {
            for (var i = 1; i < count; i++)
            {
                var argument = Marshal.PtrToStringUni(Marshal.ReadIntPtr(argv, i * IntPtr.Size)) ?? "";
                if (argument.StartsWith("-project=", StringComparison.OrdinalIgnoreCase)) argument = argument[9..];
                if (!argument.EndsWith(".uproject", StringComparison.OrdinalIgnoreCase) || !Path.IsPathFullyQualified(argument)) continue;
                return string.Equals(Path.GetFullPath(argument), Path.GetFullPath(projectPath), StringComparison.OrdinalIgnoreCase);
            }
            return false;
        }
        catch (ArgumentException) { return false; }
        finally { LocalFree(argv); }
    }

    private static bool IsRunning(Process process)
    {
        try { return !process.HasExited; }
        catch { return false; }
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CommandLineToArgvW(string commandLine, out int count);
    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr memory);

    private static JsonObject ToProcessJson(Process process)
    {
        var result = new JsonObject { ["processId"] = process.Id };
        try
        {
            process.Refresh();
            result["processName"] = process.ProcessName;
            result["windowTitle"] = process.MainWindowTitle;
        }
        catch { result["exited"] = true; }
        return result;
    }
}
