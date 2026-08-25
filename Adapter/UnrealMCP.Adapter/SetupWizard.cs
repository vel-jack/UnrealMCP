using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal static class SetupWizard
{
    public static async Task<JsonObject> RunAsync(AdapterInstallationService installer, CancellationToken cancellationToken)
    {
        Console.Error.WriteLine("UnrealMCP adapter setup");
        Console.Error.WriteLine("This registers the adapter from its current directory with selected MCP clients.");
        Console.Error.WriteLine("Use configure --install-dir <path> when you explicitly want a separate copy.");
        var launch = installer.GetCurrentLaunchDefinition();
        var configuration = new ClientConfigurationService(launch);
        var detected = configuration.DetectClients().Where(client => client.Detected).ToArray();
        if (detected.Length == 0)
            return new JsonObject { ["success"] = false, ["message"] = "No supported MCP clients were detected." };

        Console.Error.WriteLine();
        for (var index = 0; index < detected.Length; index++)
            Console.Error.WriteLine($"  {index + 1}. {detected[index].DisplayName}");
        Console.Error.Write("Configure all detected clients? [Y/n] ");
        if (Console.ReadLine()?.Trim().Equals("n", StringComparison.OrdinalIgnoreCase) == true)
            return new JsonObject { ["success"] = false, ["cancelled"] = true };

        var configured = await configuration.ConfigureAsync(["all"], false, false, true, cancellationToken);
        return new JsonObject
        {
            ["success"] = configured["success"]?.GetValue<bool>() == true,
            ["launchCommand"] = launch.Command,
            ["configuration"] = configured,
            ["nextAction"] = "Restart or reload each configured MCP client so it discovers UnrealMCP tools."
        };
    }
}
