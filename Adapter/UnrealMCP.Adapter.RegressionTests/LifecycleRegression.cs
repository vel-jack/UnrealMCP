using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
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
        CheckProjectCommandLines();
        await CheckLaunchOptions(workspace, project);
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
        var finderState = 0;
        var launchCalls = 0;
        var delayedPipeName = "UnrealMCPStarting_" + Guid.NewGuid().ToString("N");
        var delayed = new UnrealSessionManager(
            Options(workspace, project, delayedPipeName, TimeSpan.FromSeconds(2)),
            _ => finderState switch { 1 => [Process.GetCurrentProcess()], 2 => [], _ => Running(delayedProcess) },
            (_, _, _) => { launchCalls++; return Task.FromResult<Process?>(delayedProcess = StartHoldProcess()); });
        try
        {
            var starting = await delayed.LaunchProjectAsync(null, default);
            Check(starting["success"]!.GetValue<bool>() && starting["launchAccepted"]!.GetValue<bool>() &&
                  !starting["ready"]!.GetValue<bool>() && starting["sessionState"]!.GetValue<string>() == "editor_starting" &&
                  starting["startupElapsedSeconds"] is not null,
                "Accepted launch reports editor_starting instead of a request timeout");

            await using (var delayedPipe = new FakeNativePipe(delayedPipeName))
            {
                finderState = 1; // FakeNativePipe is served by this test process, not the startup child.
                var ready = await delayed.GetUnrealStatusAsync(null, default);
                Check(ready["success"]!.GetValue<bool>() && ready["ready"]!.GetValue<bool>() &&
                      ready["sessionState"]!.GetValue<string>() == "ready",
                    "Later status transitions editor_starting to ready when the pipe appears");

                Check(ready["attachedProcessId"]!.GetValue<int>() == Environment.ProcessId,
                    "Successful attachment pins the actual named-pipe server PID");
                finderState = 2; // Model editor window/title discovery disappearing after attachment.
                var retained = await delayed.GetUnrealStatusAsync(null, default);
                Check(retained["ready"]!.GetValue<bool>() && retained["attachedProcessId"]!.GetValue<int>() == Environment.ProcessId,
                    "Ready session retains verified PID when process finder loses the window");
                var alreadyRunning = await delayed.LaunchProjectAsync(null, default);
                Check(alreadyRunning["ready"]!.GetValue<bool>() && launchCalls == 1,
                    "LaunchProject reuses verified editor without invoking launcher again");
                var reconnected = await delayed.ReconnectAsync(default);
                Check(reconnected["ready"]!.GetValue<bool>() && reconnected["attachedProcessId"]!.GetValue<int>() == Environment.ProcessId,
                    "Reconnect retains and validates the same editor PID");
                delayedPipe.StallReadTool = true;
                var (timedOutRead, isError) = await delayed.InvokeUnrealToolAsync(
                    "GetRegressionInitial", new JsonObject(), default);
                Check(isError && timedOutRead["errorCode"]!.GetValue<string>() == "unreal_request_timeout",
                    "Established native read timeout retains unreal_request_timeout identity");
            }
            var pipeDown = await delayed.GetUnrealStatusAsync(null, default);
            Check(pipeDown["unrealRunning"]!.GetValue<bool>() && !pipeDown["ready"]!.GetValue<bool>() &&
                  pipeDown["attachedProcessId"]!.GetValue<int>() == Environment.ProcessId &&
                  pipeDown["recommendedAction"]!.GetValue<string>() == "ReconnectUnreal",
                "Lost pipe retains running PID and recommends reconnect instead of launching");
            var unavailableLaunch = await delayed.LaunchProjectAsync(null, default);
            Check(unavailableLaunch["unrealRunning"]!.GetValue<bool>() && launchCalls == 1,
                "Unavailable pipe never duplicates an already verified running editor");
            await delayed.SelectProjectAsync(new JsonObject { ["projectPath"] = project }, default);
            var selectedAgain = await delayed.GetUnrealStatusAsync(null, default);
            Check(selectedAgain["sessionState"]!.GetValue<string>() == "unreal_not_running" &&
                  selectedAgain["attachedProcessId"] is null,
                "Explicit project selection clears previously verified PID");
        }
        finally
        {
            StopHoldProcess(delayedProcess);
        }

        using var mismatchProcess = StartHoldProcess();
        var mismatchPipeName = "UnrealMCPIdentity_" + Guid.NewGuid().ToString("N");
        await using (var identityPipe = new IdentityProbePipe(mismatchPipeName))
        {
            var mismatchLaunchCalls = 0;
            var mismatch = new UnrealSessionManager(Options(workspace, project, mismatchPipeName, TimeSpan.FromSeconds(1)),
                _ => Running(mismatchProcess), (_, _, _) => { mismatchLaunchCalls++; return Task.FromResult<Process?>(null); });
            try
            {
                foreach (var result in new[] { await mismatch.GetUnrealStatusAsync(null, default), await mismatch.LaunchProjectAsync(null, default) })
                    Check(!result["success"]!.GetValue<bool>() && result["errorCode"]!.GetValue<string>() == "project_identity_mismatch",
                        "Named-pipe server outside selected project PID set is rejected");
                Check(identityPipe.RequestCount == 0 && mismatchLaunchCalls == 0,
                    "Identity mismatch sends no native handshake and launches no editor");
            }
            finally { StopHoldProcess(mismatchProcess); }
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

    private static async Task CheckLaunchOptions(string workspace, string project)
    {
        Check(new AdapterOptions().LaunchUnattended && AdapterOptions.Parse(["serve"], true).LaunchUnattended,
            "Automated editor launch defaults to unattended");
        Check(!AdapterOptions.Parse(["serve", "--interactive-editor"], true).LaunchUnattended,
            "Interactive-editor option explicitly enables startup dialogs");
        const string executable = @"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe";
        const string spacedProject = @"C:\Fixture Space\Project One\Shared.uproject";
        var automated = UnrealSessionManager.BuildEditorStartInfo(executable, spacedProject, true);
        Check(automated.FileName == executable && automated.Arguments.Length == 0 &&
              automated.ArgumentList.SequenceEqual(new[] { spacedProject, "-unattended" }),
            "Automated launch passes spaced project as one exact argument and adds unattended flag");
        var interactive = UnrealSessionManager.BuildEditorStartInfo(executable, spacedProject, false);
        Check(interactive.FileName == executable && interactive.Arguments.Length == 0 &&
              interactive.ArgumentList.SequenceEqual(new[] { spacedProject }),
            "Interactive launch preserves exact project argument without unattended flag");
        var launchCalls = 0;
        var manager = new UnrealSessionManager(Options(workspace, project, "UnrealMCPBadLaunch_" + Guid.NewGuid().ToString("N"), TimeSpan.FromSeconds(1)),
            _ => [], (_, _, _) => { launchCalls++; return Task.FromResult<Process?>(null); });
        var rejected = await manager.LaunchProjectAsync(new JsonObject { ["unattended"] = "false" }, default);
        Check(!rejected["success"]!.GetValue<bool>() && rejected["errorCode"]!.GetValue<string>() == "invalid_arguments" && launchCalls == 0,
            "Nonboolean unattended launch option is rejected before launcher invocation");
    }

    private static void CheckProjectCommandLines()
    {
        const string selected = @"C:\Fixture Space\Project One\Shared.uproject";
        const string editor = "\"C:\\Program Files\\Epic Games\\UnrealEditor.exe\"";
        bool Matches(string arguments) => UnrealSessionManager.MatchesProjectCommandLine(editor + " " + arguments, selected);
        Check(Matches("\"" + selected + "\" -log"), "Quoted absolute project path with spaces matches exactly");
        Check(Matches("-project=\"" + selected + "\""), "Quoted -project= path matches exactly");
        Check(Matches("\"-project=" + selected + "\""), "Whole quoted -project argument matches exactly");
        Check(Matches("-PROJECT=\"" + selected.ToUpperInvariant() + "\""), "Project argument and path matching are case insensitive");
        Check(UnrealSessionManager.MatchesProjectCommandLine("UnrealEditor.exe C:\\Fixture\\Shared.uproject", @"C:\Fixture\Shared.uproject"),
            "Unquoted absolute project argument matches");
        Check(!Matches("\"C:\\Other Folder\\Shared.uproject\""), "Same project filename in another folder is rejected");
        Check(!Matches("\"" + selected + ".backup\""), "Project filename prefix collision is rejected");
        Check(!Matches("\"C:\\Fixture Space\\Project One Extra\\Shared.uproject\""), "Project directory prefix collision is rejected");
        Check(!Matches("-log=\"" + selected + "\""), "Nonproject flag containing path is rejected");
        Check(!Matches("-description=\"Using " + selected + "\""), "Embedded project path in unrelated text is rejected");
        Check(!Matches("Shared.uproject"), "Relative project argument cannot identify a selected absolute project");
    }

    private sealed class IdentityProbePipe : IAsyncDisposable
    {
        private readonly CancellationTokenSource stop = new();
        private readonly Task loop;
        public int RequestCount;
        public IdentityProbePipe(string name) => loop = Task.Run(async () =>
        {
            while (!stop.IsCancellationRequested)
            {
                await using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
                await server.WaitForConnectionAsync(stop.Token);
                using var reader = new StreamReader(server, new UTF8Encoding(false), leaveOpen: true);
                var line = await reader.ReadLineAsync(stop.Token);
                if (line is null) continue; // Expected: PID rejected before any native request is sent.
                Interlocked.Increment(ref RequestCount);
                var request = JsonNode.Parse(line)!;
                using var writer = new StreamWriter(server, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
                await writer.WriteLineAsync(new JsonObject { ["jsonrpc"] = "2.0", ["id"] = request["id"]!.DeepClone(),
                    ["result"] = new JsonObject { ["tools"] = new JsonArray() } }.ToJsonString());
            }
        });
        public async ValueTask DisposeAsync()
        {
            stop.Cancel();
            try { await loop; } catch (OperationCanceledException) { }
            stop.Dispose();
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
