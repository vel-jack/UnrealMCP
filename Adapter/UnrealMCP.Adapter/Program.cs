using System.Text.Json.Nodes;
using UnrealMCP.Adapter;

try
{
    var options = AdapterOptions.Parse(args);
    using var cancellation = new CancellationTokenSource();
    Console.CancelKeyPress += (_, eventArgs) =>
    {
        eventArgs.Cancel = true;
        cancellation.Cancel();
    };

    var dispatcher = new McpRequestDispatcher(options);
    switch (options.Mode)
    {
        case AdapterMode.Serve:
        {
            var server = new McpFramedStdioServer(dispatcher);
            await server.RunAsync(cancellation.Token);
            break;
        }
        case AdapterMode.Discover:
            await WriteJsonAsync(dispatcher.SessionManager.GetStandaloneDiscoverPayload());
            break;
        case AdapterMode.Launch:
            await WriteJsonAsync(await dispatcher.SessionManager.GetStandaloneLaunchPayloadAsync(cancellation.Token));
            break;
        case AdapterMode.Status:
            await WriteJsonAsync(await dispatcher.SessionManager.GetStandaloneStatusPayloadAsync(cancellation.Token));
            break;
        default:
            throw new InvalidOperationException($"Unsupported adapter mode '{options.Mode}'.");
    }
}
catch (AdapterOptionsException exception)
{
    Console.Error.WriteLine(exception.Message);
    Environment.ExitCode = 2;
}
catch (OperationCanceledException)
{
    Environment.ExitCode = 0;
}
catch (Exception exception)
{
    Console.Error.WriteLine(exception);
    Environment.ExitCode = 1;
}

static Task WriteJsonAsync(JsonObject payload)
{
    Console.Out.WriteLine(payload.ToJsonString(McpProtocol.JsonOptions));
    return Task.CompletedTask;
}
