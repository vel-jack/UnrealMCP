using System.Diagnostics;
using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed class AdapterDoctorService(McpLaunchDefinition launch, ClientConfigurationService clients)
{
    public async Task<JsonObject> RunAsync(CancellationToken cancellationToken)
    {
        var protocol = await ProbeProtocolAsync(cancellationToken);
        var clientPayload = clients.GetDetectionPayload();
        return new JsonObject
        {
            ["success"] = protocol["success"]?.GetValue<bool>() == true,
            ["adapterVersion"] = typeof(AdapterDoctorService).Assembly.GetName().Version?.ToString(),
            ["launchCommand"] = launch.Command,
            ["launchArguments"] = new JsonArray(launch.Arguments.Select(value => (JsonNode?)JsonValue.Create(value)).ToArray()),
            ["stdioProtocol"] = protocol,
            ["clients"] = clientPayload["clients"]?.DeepClone()
        };
    }

    private async Task<JsonObject> ProbeProtocolAsync(CancellationToken cancellationToken)
    {
        if (!File.Exists(launch.Command))
            return new JsonObject { ["success"] = false, ["message"] = $"Adapter executable not found: {launch.Command}" };

        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(10));
        try
        {
            var startInfo = new ProcessStartInfo(launch.Command)
            {
                UseShellExecute = false,
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true
            };
            foreach (var argument in launch.Arguments.Append("serve")) startInfo.ArgumentList.Add(argument);
            using var process = Process.Start(startInfo) ?? throw new InvalidOperationException("Failed to start the adapter protocol probe.");
            await process.StandardInput.WriteLineAsync("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"unrealmcp-doctor\",\"version\":\"1.0\"}}}");
            await process.StandardInput.FlushAsync(timeout.Token);
            var line = await process.StandardOutput.ReadLineAsync(timeout.Token);
            if (!process.HasExited) process.Kill(true);
            var response = JsonNode.Parse(line ?? string.Empty) as JsonObject;
            var serverName = response?["result"]?["serverInfo"]?["name"]?.GetValue<string>();
            return new JsonObject
            {
                ["success"] = response?["jsonrpc"]?.GetValue<string>() == "2.0" && serverName is not null,
                ["transport"] = "stdio",
                ["framing"] = "newline-delimited-json",
                ["serverName"] = serverName,
                ["protocolVersion"] = response?["result"]?["protocolVersion"]?.GetValue<string>()
            };
        }
        catch (Exception exception)
        {
            return new JsonObject { ["success"] = false, ["message"] = exception.Message };
        }
    }
}
