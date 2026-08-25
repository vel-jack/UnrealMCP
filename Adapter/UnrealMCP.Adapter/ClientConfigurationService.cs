using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed record McpClientTarget(string Id, string DisplayName, bool Detected, string ConfigurationMethod, IReadOnlyList<string> ConfigurationPaths);

internal sealed class ClientConfigurationService(McpLaunchDefinition launch)
{
    private const string ServerName = "unreal-mcp-adapter";

    public IReadOnlyList<McpClientTarget> DetectClients()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        var clinePaths = GetClinePaths(home, appData);
        return
        [
            new("codex", "Codex", FindCommand("codex") is not null, "official_cli", []),
            new("claude", "Claude Code", FindCommand("claude") is not null, "official_cli", []),
            JsonTarget("cursor", "Cursor", FindCommand("cursor") is not null || Directory.Exists(Path.Combine(home, ".cursor")), Path.Combine(home, ".cursor", "mcp.json")),
            JsonTarget("cline", "Cline", FindCommand("cline") is not null || clinePaths.Any(path => Directory.Exists(Path.GetDirectoryName(path))), clinePaths),
            JsonTarget("antigravity", "Antigravity", FindCommand("agy") is not null || Directory.Exists(Path.Combine(home, ".gemini")), ResolveAntigravityPath(home))
        ];
    }

    public JsonObject GetDetectionPayload() => new()
    {
        ["success"] = true,
        ["command"] = launch.Command,
        ["arguments"] = StringArray(launch.Arguments),
        ["clients"] = new JsonArray(DetectClients().Select(target => (JsonNode)new JsonObject
        {
            ["id"] = target.Id,
            ["displayName"] = target.DisplayName,
            ["detected"] = target.Detected,
            ["configurationMethod"] = target.ConfigurationMethod,
            ["configurationPaths"] = StringArray(target.ConfigurationPaths)
        }).ToArray())
    };

    public async Task<JsonObject> ConfigureAsync(IReadOnlyList<string> requestedClients, bool remove, bool dryRun, bool assumeYes, CancellationToken cancellationToken)
    {
        var selected = ResolveTargets(requestedClients);
        if (selected.Count == 0) throw new AdapterOptionsException("No clients selected. Use --client <codex|claude|cursor|cline|antigravity|all>.");
        if (!dryRun && !assumeYes && !Confirm(remove ? "Remove UnrealMCP from the selected clients?" : "Configure UnrealMCP for the selected clients?"))
            return new JsonObject { ["success"] = false, ["cancelled"] = true };

        var results = new JsonArray();
        foreach (var target in selected)
        {
            results.Add(target.ConfigurationMethod == "official_cli"
                ? await ConfigureWithCliAsync(target, remove, dryRun, cancellationToken)
                : ConfigureJson(target, remove, dryRun));
        }
        return new JsonObject
        {
            ["success"] = results.All(node => node?["success"]?.GetValue<bool>() == true),
            ["operation"] = remove ? "uninstall" : "configure",
            ["dryRun"] = dryRun,
            ["clients"] = results
        };
    }

    private IReadOnlyList<McpClientTarget> ResolveTargets(IReadOnlyList<string> requested)
    {
        var targets = DetectClients();
        if (requested.Any(value => value.Equals("all", StringComparison.OrdinalIgnoreCase))) return targets.Where(target => target.Detected).ToArray();
        var ids = requested.ToHashSet(StringComparer.OrdinalIgnoreCase);
        var unknown = ids.Where(id => targets.All(target => !target.Id.Equals(id, StringComparison.OrdinalIgnoreCase))).ToArray();
        if (unknown.Length > 0) throw new AdapterOptionsException($"Unknown MCP client(s): {string.Join(", ", unknown)}.");
        return targets.Where(target => ids.Contains(target.Id)).ToArray();
    }

    private async Task<JsonObject> ConfigureWithCliAsync(McpClientTarget target, bool remove, bool dryRun, CancellationToken cancellationToken)
    {
        var command = FindCommand(target.Id == "codex" ? "codex" : "claude");
        if (command is null) return Failure(target, "Client command was not found on PATH.");
        var arguments = target.Id == "codex" ? CodexArguments(remove) : ClaudeArguments(remove);
        if (dryRun) return Success(target, "planned", command, arguments);

        var getArguments = target.Id == "codex"
            ? new[] { "mcp", "get", ServerName, "--json" }
            : ["mcp", "get", ServerName];
        var existing = await RunProcessAsync(command, getArguments, cancellationToken, true);
        if (existing.ExitCode == 0)
        {
            var normalizedOutput = existing.StandardOutput.Replace("\\\\", "\\", StringComparison.Ordinal);
            if (!normalizedOutput.Contains(launch.Command, StringComparison.OrdinalIgnoreCase)
                || !normalizedOutput.Contains("serve", StringComparison.OrdinalIgnoreCase))
            {
                return Failure(target, "A different unreal-mcp-adapter entry already exists; it was left untouched.");
            }
            if (!remove) return Success(target, "unchanged", command, getArguments);
        }
        else if (remove) return Success(target, "unchanged", command, getArguments);

        var result = await RunProcessAsync(command, arguments, cancellationToken, remove);
        return result.ExitCode == 0
            ? Success(target, remove ? "removed" : "configured", command, arguments)
            : Failure(target, string.IsNullOrWhiteSpace(result.StandardError) ? result.StandardOutput : result.StandardError);
    }

    private JsonObject ConfigureJson(McpClientTarget target, bool remove, bool dryRun)
    {
        var path = target.ConfigurationPaths.FirstOrDefault(File.Exists) ?? target.ConfigurationPaths[0];
        try
        {
            var root = File.Exists(path)
                ? JsonNode.Parse(File.ReadAllText(path)) as JsonObject ?? throw new InvalidDataException("The MCP configuration root must be a JSON object.")
                : new JsonObject();
            var servers = root["mcpServers"] as JsonObject ?? new JsonObject();
            root["mcpServers"] = servers;
            var expected = BuildServerDefinition(target.Id);
            var existing = servers[ServerName];
            if (remove)
            {
                if (existing is null) return Success(target, "unchanged", path, []);
                if (!JsonNode.DeepEquals(existing, expected)) return Failure(target, "The existing UnrealMCP entry was modified; it was left untouched.");
                servers.Remove(ServerName);
            }
            else if (existing is not null)
            {
                return JsonNode.DeepEquals(existing, expected)
                    ? Success(target, "unchanged", path, [])
                    : Failure(target, "A different unreal-mcp-adapter entry already exists. Remove or rename it explicitly before configuring.");
            }
            else servers[ServerName] = expected;

            if (!dryRun) AtomicWrite(path, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
            return Success(target, dryRun ? "planned" : remove ? "removed" : "configured", path, []);
        }
        catch (Exception exception)
        {
            return Failure(target, $"{path}: {exception.Message}");
        }
    }

    private JsonObject BuildServerDefinition(string clientId)
    {
        var definition = new JsonObject { ["command"] = launch.Command, ["args"] = StringArray(launch.Arguments.Append("serve")) };
        if (clientId == "cline")
        {
            definition["disabled"] = false;
            definition["autoApprove"] = new JsonArray();
        }
        return definition;
    }

    private string[] CodexArguments(bool remove) => remove
        ? ["mcp", "remove", ServerName]
        : ["mcp", "add", ServerName, "--", launch.Command, .. launch.Arguments, "serve"];

    private string[] ClaudeArguments(bool remove) => remove
        ? ["mcp", "remove", ServerName, "--scope", "user"]
        : ["mcp", "add", "--transport", "stdio", "--scope", "user", ServerName, "--", launch.Command, .. launch.Arguments, "serve"];

    private static void AtomicWrite(string path, string contents)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporaryPath = path + ".unrealmcp.tmp";
        File.WriteAllText(temporaryPath, contents);
        if (File.Exists(path))
        {
            File.Copy(path, path + ".unrealmcp.bak", true);
            File.Move(temporaryPath, path, true);
        }
        else File.Move(temporaryPath, path);
    }

    private static string[] GetClinePaths(string home, string appData)
    {
        var paths = new List<string>();
        var pathOverride = Environment.GetEnvironmentVariable("CLINE_MCP_SETTINGS_PATH");
        var dataOverride = Environment.GetEnvironmentVariable("CLINE_DATA_DIR");
        if (!string.IsNullOrWhiteSpace(pathOverride)) paths.Add(Path.GetFullPath(pathOverride));
        if (!string.IsNullOrWhiteSpace(dataOverride)) paths.Add(Path.Combine(dataOverride, "settings", "cline_mcp_settings.json"));
        paths.Add(Path.Combine(home, ".cline", "data", "settings", "cline_mcp_settings.json"));
        paths.Add(Path.Combine(appData, "Code", "User", "globalStorage", "saoudrizwan.claude-dev", "settings", "cline_mcp_settings.json"));
        paths.Add(Path.Combine(appData, "Cursor", "User", "globalStorage", "saoudrizwan.claude-dev", "settings", "cline_mcp_settings.json"));
        return paths.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    private static string ResolveAntigravityPath(string home)
    {
        var current = Path.Combine(home, ".gemini", "config", "mcp_config.json");
        var legacy = Path.Combine(home, ".gemini", "antigravity", "mcp_config.json");
        return File.Exists(current) || File.Exists(Path.Combine(home, ".gemini", "config", ".migrated")) || !File.Exists(legacy) ? current : legacy;
    }

    private static McpClientTarget JsonTarget(string id, string name, bool detected, params string[] paths) => new(id, name, detected, "json_merge", paths);
    private static JsonArray StringArray(IEnumerable<string> values) => new(values.Select(value => (JsonNode?)JsonValue.Create(value)).ToArray());

    private static string? FindCommand(string command)
    {
        var extensions = OperatingSystem.IsWindows() ? (Environment.GetEnvironmentVariable("PATHEXT") ?? ".EXE;.CMD;.BAT").Split(';', StringSplitOptions.RemoveEmptyEntries) : [string.Empty];
        foreach (var directory in (Environment.GetEnvironmentVariable("PATH") ?? string.Empty).Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        foreach (var extension in extensions.Prepend(string.Empty))
        {
            var candidate = Path.Combine(directory.Trim(), command + extension);
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }

    private static bool Confirm(string message)
    {
        Console.Error.Write($"{message} [y/N] ");
        return Console.ReadLine()?.Trim().Equals("y", StringComparison.OrdinalIgnoreCase) == true;
    }

    private static async Task<(int ExitCode, string StandardOutput, string StandardError)> RunProcessAsync(string command, IReadOnlyList<string> arguments, CancellationToken cancellationToken, bool allowFailure)
    {
        var startInfo = new ProcessStartInfo(command) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var argument in arguments) startInfo.ArgumentList.Add(argument);
        using var process = Process.Start(startInfo) ?? throw new InvalidOperationException($"Failed to start {command}.");
        var stdout = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var stderr = process.StandardError.ReadToEndAsync(cancellationToken);
        await process.WaitForExitAsync(cancellationToken);
        var result = (process.ExitCode, await stdout, await stderr);
        if (!allowFailure && result.ExitCode != 0) throw new InvalidOperationException($"{Path.GetFileName(command)} exited with code {result.ExitCode}: {result.Item3}");
        return result;
    }

    private static JsonObject Success(McpClientTarget target, string action, string command, IReadOnlyList<string> arguments) => new()
    {
        ["success"] = true, ["client"] = target.Id, ["displayName"] = target.DisplayName, ["action"] = action,
        ["command"] = command, ["arguments"] = StringArray(arguments)
    };

    private static JsonObject Failure(McpClientTarget target, string message) => new()
    {
        ["success"] = false, ["client"] = target.Id, ["displayName"] = target.DisplayName, ["message"] = message
    };
}
