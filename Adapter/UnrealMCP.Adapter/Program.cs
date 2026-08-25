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

    switch (options.Mode)
    {
        case AdapterMode.Help:
            Console.Out.WriteLine(AdapterOptions.BuildUsage());
            break;
        case AdapterMode.Setup:
        {
            var installer = new AdapterInstallationService();
            await WriteJsonAsync(await SetupWizard.RunAsync(installer, cancellation.Token));
            break;
        }
        case AdapterMode.Serve:
        {
            var dispatcher = new McpRequestDispatcher(options);
            await SdkMcpServer.RunAsync(dispatcher, cancellation.Token);
            break;
        }
        case AdapterMode.Discover:
        {
            var dispatcher = new McpRequestDispatcher(options);
            await WriteJsonAsync(dispatcher.SessionManager.GetStandaloneDiscoverPayload());
            break;
        }
        case AdapterMode.Launch:
        {
            var dispatcher = new McpRequestDispatcher(options);
            await WriteJsonAsync(await dispatcher.SessionManager.GetStandaloneLaunchPayloadAsync(cancellation.Token));
            break;
        }
        case AdapterMode.Status:
        {
            var dispatcher = new McpRequestDispatcher(options);
            await WriteJsonAsync(await dispatcher.SessionManager.GetStandaloneStatusPayloadAsync(cancellation.Token));
            break;
        }
        case AdapterMode.Configure:
        case AdapterMode.Uninstall:
        {
            var installer = new AdapterInstallationService();
            JsonObject? installation = null;
            var launch = installer.GetCurrentLaunchDefinition();
            if (options.Mode == AdapterMode.Configure && options.InstallDirectory is not null)
            {
                var installed = installer.Install(options.InstallDirectory, options.DryRun);
                launch = installed.Launch;
                installation = installed.Result;
            }
            else if (options.Mode == AdapterMode.Uninstall && options.InstallDirectory is not null)
            {
                launch = AdapterInstallationService.GetDestinationLaunchDefinition(options.InstallDirectory);
            }
            var configuration = new ClientConfigurationService(launch);
            var result = await configuration.ConfigureAsync(
                options.Clients,
                options.Mode == AdapterMode.Uninstall,
                options.DryRun,
                options.AssumeYes,
                cancellation.Token);
            if (installation is not null) result["installation"] = installation;
            await WriteJsonAsync(result);
            break;
        }
        case AdapterMode.Doctor:
        {
            var installer = new AdapterInstallationService();
            var launch = installer.GetCurrentLaunchDefinition();
            var clients = new ClientConfigurationService(launch);
            await WriteJsonAsync(await new AdapterDoctorService(launch, clients).RunAsync(cancellation.Token));
            break;
        }
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
