using System.Diagnostics;
using System.Text.Json.Nodes;
using UnrealMCP.Adapter;

internal static class LifecycleRegression
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException("FAIL: " + message);
        Console.WriteLine("PASS: " + message);
    }

    public static async Task Run(string workspace, string project)
    {
        var closedOptions = Options(workspace, project, "UnrealMCPClosed_" + Guid.NewGuid().ToString("N"), TimeSpan.FromSeconds(1));
        var closed = new UnrealSessionManager(closedOptions, _ => []);
        var timer = Stopwatch.StartNew();
        var closedStatus = await closed.GetUnrealStatusAsync(null, default);
        timer.Stop();
        Check(closedStatus["sessionState"]!.GetValue<string>() == "unreal_not_running" &&
              closedStatus["unrealRunning"]!.GetValue<bool>() == false &&
              timer.Elapsed < TimeSpan.FromMilliseconds(250),
            "Closed Editor reports unreal_not_running immediately without a pipe timeout");

        Process? delayedProcess = null;
        var delayedPipeName = "UnrealMCPStarting_" + Guid.NewGuid().ToString("N");
        var delayed = new UnrealSessionManager(
            Options(workspace, project, delayedPipeName, TimeSpan.FromSeconds(2)),
            _ => Running(delayedProcess),
            (_, _, _) => Task.FromResult<Process?>(delayedProcess = StartHoldProcess()));
        try
        {
            var starting = await delayed.LaunchProjectAsync(null, default);
            Check(starting["success"]!.GetValue<bool>() && starting["launchAccepted"]!.GetValue<bool>() &&
                  !starting["ready"]!.GetValue<bool>() && starting["sessionState"]!.GetValue<string>() == "editor_starting" &&
                  starting["startupElapsedSeconds"] is not null,
                "Accepted launch reports editor_starting instead of a request timeout");

            await using (var delayedPipe = new FakeNativePipe(delayedPipeName))
            {
                var ready = await delayed.GetUnrealStatusAsync(null, default);
                Check(ready["success"]!.GetValue<bool>() && ready["ready"]!.GetValue<bool>() &&
                      ready["sessionState"]!.GetValue<string>() == "ready",
                    "Later status transitions editor_starting to ready when the pipe appears");

                delayedPipe.StallReadTool = true;
                var (timedOutRead, isError) = await delayed.InvokeUnrealToolAsync(
                    "GetRegressionInitial", new JsonObject(), default);
                Check(isError && timedOutRead["errorCode"]!.GetValue<string>() == "unreal_request_timeout",
                    "Established native read timeout retains unreal_request_timeout identity");
            }
        }
        finally
        {
            StopHoldProcess(delayedProcess);
        }

        Process? exitingProcess = null;
        var exiting = new UnrealSessionManager(
            Options(workspace, project, "UnrealMCPExit_" + Guid.NewGuid().ToString("N"), TimeSpan.FromSeconds(2)),
            _ => Running(exitingProcess),
            (_, _, _) => Task.FromResult<Process?>(exitingProcess = StartHoldProcess()));
        try
        {
            Check((await exiting.LaunchProjectAsync(null, default))["sessionState"]!.GetValue<string>() == "editor_starting",
                "Exit fixture enters editor_starting");
            StopHoldProcess(exitingProcess);
            var exited = await exiting.GetUnrealStatusAsync(null, default);
            Check(exited["sessionState"]!.GetValue<string>() == "unreal_not_running" &&
                  !exited["success"]!.GetValue<bool>(),
                "Process exit during startup reports unreal_not_running");
        }
        finally
        {
            StopHoldProcess(exitingProcess);
        }

        Process? overdueProcess = null;
        var overdue = new UnrealSessionManager(
            Options(workspace, project, "UnrealMCPDeadline_" + Guid.NewGuid().ToString("N"), TimeSpan.FromMilliseconds(200)),
            _ => Running(overdueProcess),
            (_, _, _) => Task.FromResult<Process?>(overdueProcess = StartHoldProcess()));
        try
        {
            Check((await overdue.LaunchProjectAsync(null, default))["sessionState"]!.GetValue<string>() == "editor_starting",
                "Deadline fixture enters editor_starting");
            await Task.Delay(225);
            var expired = await overdue.GetUnrealStatusAsync(null, default);
            Check(expired["sessionState"]!.GetValue<string>() == "unreal_mcp_unavailable" &&
                  expired["unrealRunning"]!.GetValue<bool>() && !expired["success"]!.GetValue<bool>(),
                "Expired startup deadline reports unreal_mcp_unavailable");
        }
        finally
        {
            StopHoldProcess(overdueProcess);
        }
    }

    private static AdapterOptions Options(string workspace, string project, string pipeName, TimeSpan launchTimeout) => new()
    {
        Mode = AdapterMode.Serve,
        WorkspaceRoot = workspace,
        ProjectPath = project,
        PipeNameOverride = pipeName,
        DefaultEngineExecutablePath = Environment.ProcessPath,
        PipeConnectTimeout = TimeSpan.FromMilliseconds(50),
        RequestTimeout = TimeSpan.FromMilliseconds(100),
        LaunchReadyTimeout = launchTimeout
    };

    private static List<Process> Running(Process? process)
    {
        if (process is null) return [];
        try { return process.HasExited ? [] : [process]; }
        catch { return []; }
    }

    private static Process StartHoldProcess()
    {
        var start = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true };
        start.ArgumentList.Add("--hold");
        return Process.Start(start) ?? throw new InvalidOperationException("Could not start lifecycle fixture process.");
    }

    private static void StopHoldProcess(Process? process)
    {
        if (process is null) return;
        try
        {
            if (!process.HasExited) process.Kill();
            process.WaitForExit(5000);
        }
        catch { }
        process.Dispose();
    }
}
