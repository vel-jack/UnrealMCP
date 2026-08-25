using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed record McpLaunchDefinition(string Command, IReadOnlyList<string> Arguments);

internal sealed class AdapterInstallationService
{
    public McpLaunchDefinition GetCurrentLaunchDefinition()
    {
        var appHost = Path.Combine(AppContext.BaseDirectory, "UnrealMCP.Adapter.exe");
        if (File.Exists(appHost)) return new(appHost, []);

        var assemblyPath = typeof(AdapterInstallationService).Assembly.Location;
        var dotnet = Environment.ProcessPath ?? "dotnet";
        return new(dotnet, [assemblyPath]);
    }

    public static McpLaunchDefinition GetDestinationLaunchDefinition(string destinationDirectory) =>
        new(Path.Combine(Path.GetFullPath(destinationDirectory), "unrealmcp.exe"), []);

    public (McpLaunchDefinition Launch, JsonObject Result) Install(string destinationDirectory, bool dryRun)
    {
        var sourceDirectory = Path.GetFullPath(AppContext.BaseDirectory);
        var installDirectory = Path.GetFullPath(destinationDirectory);
        var launch = GetDestinationLaunchDefinition(installDirectory);
        if (dryRun)
        {
            return (launch, Payload("planned", sourceDirectory, installDirectory, launch.Command));
        }

        Directory.CreateDirectory(installDirectory);
        foreach (var sourcePath in Directory.EnumerateFiles(sourceDirectory, "*", SearchOption.AllDirectories))
        {
            var relativePath = Path.GetRelativePath(sourceDirectory, sourcePath);
            var destinationPath = Path.Combine(installDirectory, relativePath);
            Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);
            File.Copy(sourcePath, destinationPath, true);
        }

        var appHost = Path.Combine(installDirectory, "UnrealMCP.Adapter.exe");
        if (!File.Exists(appHost))
        {
            throw new InvalidOperationException($"The adapter app host was not found after installation: {appHost}");
        }

        File.Copy(appHost, launch.Command, true);
        return (launch, Payload("installed", sourceDirectory, installDirectory, launch.Command));
    }

    private static JsonObject Payload(string action, string sourceDirectory, string installDirectory, string command) => new()
    {
        ["success"] = true,
        ["action"] = action,
        ["sourceDirectory"] = sourceDirectory,
        ["installDirectory"] = installDirectory,
        ["command"] = command,
        ["arguments"] = new JsonArray("serve")
    };
}
