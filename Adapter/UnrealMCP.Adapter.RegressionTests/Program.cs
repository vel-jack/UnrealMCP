using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json.Nodes;
using UnrealMCP.Adapter;

// Dependency-free integration runner. All fake projects/catalogs stay in its unique temp directory.
if (args.FirstOrDefault() == "--serve-test")
{
    var options = AdapterOptions.Parse(args.Skip(2).ToArray(), true);
    await SdkMcpServer.RunAsync(new McpRequestDispatcher(options,
        new ToolCatalogService(Path.Combine(args[1], "cache.json"), Path.Combine(args[1], "bundled.json"))), default);
    return;
}

if (args.FirstOrDefault() == "--live")
{
    // Read-only catalog verification against a running Editor, through the production adapter.
    await using var client = new StdioClient(args[1], ["serve", "--workspace", Path.GetDirectoryName(args[2])!]);
    await client.Initialize();
    var list = await client.Request("tools/list", new JsonObject());
    Check(list["result"]?["tools"]?.AsArray().Any(t => t?["name"]?.GetValue<string>() == "CreateInputAction") == true,
        "Live initial catalog contains Enhanced Input tools");
    Check(HasTool(list, "SetInputMappingContextMappingKey"), "Live initial catalog discovers the new key-edit tool without prior attach");
    var rejection = await client.Call("SetInputMappingContextMappingKey", new JsonObject { ["dryRun"] = true });
    Check(rejection["result"]?["structuredContent"]?["message"]?.GetValue<string>()?.StartsWith("Requires objectPath") == true,
        "New tool is callable through the adapter and rejects incomplete arguments");
    var result = await client.Call("ListUnrealMCPAutomationTests", new JsonObject());
    Console.WriteLine("Registered UnrealMCP tests: " + result["result"]?["structuredContent"]?["matchedCount"]);
    if (args.Length > 3)
    {
        var run = await client.Call("RunUnrealMCPAutomationTest", new JsonObject
        {
            ["testName"] = args[3], ["confirm"] = true
        });
        Console.WriteLine(run["result"]?["structuredContent"]?.ToJsonString());
        Check(run["result"]?["isError"]?.GetValue<bool>() != true &&
            run["result"]?["structuredContent"]?["passed"]?.GetValue<bool>() == true, "Live automation request succeeded");
    }
    return;
}

var root = Path.Combine(Path.GetTempPath(), "UnrealMCPRegression_" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
try
{
    var workspace = Path.Combine(root, "workspace");
    Directory.CreateDirectory(workspace);
    var project = Path.Combine(workspace, "Regression.uproject");
    await File.WriteAllTextAsync(project, "{\"EngineAssociation\":\"5.4\"}");
    var pipeName = "UnrealMCPRegression_" + Guid.NewGuid().ToString("N");
    await using var pipe = new FakeNativePipe(pipeName);
    string[] ServerArgs() => ["--serve-test", root, "serve", "--workspace", workspace,
        "--pipe", pipeName, "--engine-exe", Environment.ProcessPath!,
        "--pipe-timeout-seconds", "0.2", "--request-timeout-seconds", "0.5"];

    // Warm editor: first tools/list must contain a tool absent from any on-disk catalog.
    await using (var client = new StdioClient(Environment.ProcessPath!, ServerArgs()))
    {
        await client.Initialize();
        var list = await client.Request("tools/list", new JsonObject());
        Check(HasTool(list, "GetRegressionInitial"), "First handshake auto-selects the sole project and reads live catalog");
        Check(list["result"]!["tools"]!.AsArray().Single(t => t!["name"]!.GetValue<string>() == "RemoveInputMappingContextMapping")!
            ["inputSchema"]!["properties"]?["operationId"] is not null, "Remove schema advertises operationId");

        pipe.IncludeNewTool = true;
        var attach = await client.Call("unreal.adapter.AttachToUnrealProject", new JsonObject());
        Check(attach["result"]?["isError"]?.GetValue<bool>() != true, "Attach succeeds in the same session");
        Check(client.Notifications > 0, "Changed native catalog emits tools/list_changed");
        Check(JsonNode.Parse(await File.ReadAllTextAsync(Path.Combine(root, "cache.json")))!["tools"]!.AsArray()
            .Any(t => t!["name"]!.GetValue<string>() == "GetRegressionNew"), "Attach persists the refreshed catalog before the client re-lists");
        list = await client.Request("tools/list", new JsonObject());
        Check(HasTool(list, "GetRegressionNew"), "Re-list exposes newly registered tool without restarting");
        var call = await client.Call("GetRegressionNew", new JsonObject());
        Check(call["result"]?["structuredContent"]?["success"]?.GetValue<bool>() == true, "New tool is callable in the same session");
        var notifications = client.Notifications;
        await client.Call("unreal.adapter.RefreshToolManifest", new JsonObject());
        Check(client.Notifications == notifications, "Unchanged explicit refresh does not notify again");

        var timeout = await client.Call("RemoveInputMappingContextMapping", new JsonObject { ["operationId"] = "stable-removal" });
        var payload = timeout["result"]!["structuredContent"]!;
        Check(payload["errorCode"]!.GetValue<string>() == "mutation_request_timeout" &&
              payload["operationId"]!.GetValue<string>() == "stable-removal" &&
              payload["canRetry"]!.GetValue<bool>() == false &&
              payload["mutationMayStillBeRunning"]!.GetValue<bool>(), "Remove timeout preserves uncertain mutation identity and forbids blind retry");
        Check(pipe.LastOperationId == "stable-removal", "Remove forwards the supplied operationId");
        await client.Call("RemoveInputMappingContextMapping", new JsonObject());
        Check(!string.IsNullOrWhiteSpace(pipe.LastOperationId) && pipe.LastOperationId != "stable-removal", "Remove generates an operationId when omitted");

        pipe.Available = false;
        list = await client.Request("tools/list", new JsonObject());
        Check(HasTool(list, "GetRegressionNew"), "Failed refresh retains the last authoritative catalog");
    }

    // Cold editor: tools/list first, native endpoint appears later, then refresh within same client.
    File.Delete(Path.Combine(root, "cache.json"));
    await using (var client = new StdioClient(Environment.ProcessPath!, ServerArgs()))
    {
        await client.Initialize();
        var list = await client.Request("tools/list", new JsonObject());
        Check(!HasTool(list, "GetRegressionNew") && HasTool(list, "unreal.adapter.RefreshToolManifest"), "Cold startup retains lifecycle tools without inventing native tools");
        pipe.Available = true;
        await client.Call("unreal.adapter.RefreshToolManifest", new JsonObject());
        Check(client.Notifications > 0, "Editor appearing after handshake triggers manifest notification");
        Check(HasTool(await client.Request("tools/list", new JsonObject()), "GetRegressionNew"), "Cold-start session discovers newly available tool without restart");
    }

    await File.WriteAllTextAsync(Path.Combine(workspace, "Other.uproject"), "{}");
    File.Delete(Path.Combine(root, "cache.json"));
    await using (var client = new StdioClient(Environment.ProcessPath!, ServerArgs()))
    {
        await client.Initialize();
        Check(!HasTool(await client.Request("tools/list", new JsonObject()), "GetRegressionInitial"), "Ambiguous project discovery does not auto-attach");
        await client.Call("unreal.adapter.AttachToUnrealProject", new JsonObject { ["projectPath"] = project });
        Check(HasTool(await client.Request("tools/list", new JsonObject()), "GetRegressionInitial"), "Explicit project selection resolves ambiguity");
    }
    Console.WriteLine("All adapter regression checks passed.");
}
finally
{
    // root was generated by this runner; never recursively delete a caller-provided path.
    Directory.Delete(root, true);
}

static bool HasTool(JsonObject response, string name) => response["result"]?["tools"]?.AsArray()
    .Any(t => t?["name"]?.GetValue<string>() == name) == true;
static void Check(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException("FAIL: " + message);
    Console.WriteLine("PASS: " + message);
}

sealed class StdioClient : IAsyncDisposable
{
    private readonly Process process;
    private readonly Task<string> stderr;
    private int id;
    public int Notifications { get; private set; }

    public StdioClient(string executable, string[] arguments)
    {
        var start = new ProcessStartInfo(executable)
        {
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true
        };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        process = Process.Start(start)!;
        stderr = process.StandardError.ReadToEndAsync();
    }

    public async Task Initialize()
    {
        var result = await Request("initialize", new JsonObject
        {
            ["protocolVersion"] = "2025-06-18", ["capabilities"] = new JsonObject(),
            ["clientInfo"] = new JsonObject { ["name"] = "UnrealMCPRegression", ["version"] = "1.0" }
        });
        if (result["result"]?["capabilities"]?["tools"]?["listChanged"]?.GetValue<bool>() != true)
            throw new InvalidOperationException("Server did not advertise tools.listChanged");
        await process.StandardInput.WriteLineAsync("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
        await process.StandardInput.FlushAsync();
    }

    public Task<JsonObject> Call(string name, JsonObject args) => Request("tools/call", new JsonObject { ["name"] = name, ["arguments"] = args });

    public async Task<JsonObject> Request(string method, JsonObject parameters)
    {
        var requestId = ++id;
        await process.StandardInput.WriteLineAsync(new JsonObject
        {
            ["jsonrpc"] = "2.0", ["id"] = requestId, ["method"] = method, ["params"] = parameters
        }.ToJsonString());
        await process.StandardInput.FlushAsync();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(60));
        while (true)
        {
            var line = await process.StandardOutput.ReadLineAsync(timeout.Token);
            if (line is null) throw new IOException("Adapter exited: " + await stderr);
            var response = JsonNode.Parse(line)!.AsObject();
            if (response["method"]?.GetValue<string>() == "notifications/tools/list_changed") ++Notifications;
            if (response["id"]?.GetValue<int>() != requestId) continue;
            if (response["error"] is not null) throw new InvalidOperationException(response.ToJsonString());
            return response;
        }
    }

    public async ValueTask DisposeAsync()
    {
        process.StandardInput.Close();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        try { await process.WaitForExitAsync(timeout.Token); }
        catch (OperationCanceledException) { process.Kill(); await process.WaitForExitAsync(); }
        process.Dispose();
    }
}

sealed class FakeNativePipe : IAsyncDisposable
{
    private readonly CancellationTokenSource stop = new();
    private readonly Task loop;
    public volatile bool IncludeNewTool;
    public volatile bool Available = true;
    public string? LastOperationId;

    public FakeNativePipe(string name) => loop = Task.Run(async () =>
    {
        while (!stop.IsCancellationRequested)
        {
            await using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1,
                PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
            await server.WaitForConnectionAsync(stop.Token);
            using var reader = new StreamReader(server, new UTF8Encoding(false), leaveOpen: true);
            using var writer = new StreamWriter(server, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
            var request = JsonNode.Parse((await reader.ReadLineAsync(stop.Token))!)!;
            var method = request["method"]!.GetValue<string>();
            JsonObject response = new() { ["jsonrpc"] = "2.0", ["id"] = request["id"]!.DeepClone() };
            if (!Available)
                response["error"] = new JsonObject { ["code"] = -32000, ["message"] = "Simulated editor unavailable" };
            else if (method == "tools/list")
            {
                var names = new List<string> { "GetRegressionInitial", "RemoveInputMappingContextMapping" };
                if (IncludeNewTool) names.Add("GetRegressionNew");
                response["result"] = new JsonObject { ["tools"] = new JsonArray(names.Select(n => (JsonNode)new JsonObject
                {
                    ["name"] = n, ["description"] = "Regression fixture", ["inputSchema"] = new JsonObject { ["type"] = "object" }
                }).ToArray()) };
            }
            else if (method == "RemoveInputMappingContextMapping")
            {
                LastOperationId = request["params"]?["operationId"]?.GetValue<string>();
                // Wait for the client's timeout to disconnect, then serve its next connection.
                await reader.ReadLineAsync(stop.Token);
                continue;
            }
            else response["result"] = new JsonObject { ["success"] = true };
            await writer.WriteLineAsync(response.ToJsonString());
        }
    });

    public async ValueTask DisposeAsync()
    {
        stop.Cancel();
        try { await loop; } catch (OperationCanceledException) { }
        stop.Dispose();
    }
}
