using System.Globalization;

namespace UnrealMCP.Adapter;

internal enum AdapterMode
{
    Help,
    Setup,
    Serve,
    Discover,
    Launch,
    Status,
    Doctor,
    Configure,
    Uninstall
}

internal sealed class AdapterOptions
{
    public AdapterMode Mode { get; init; } = AdapterMode.Serve;
    public string? WorkspaceRoot { get; init; }
    public string? PipeNameOverride { get; init; }
    public string? DefaultEngineExecutablePath { get; init; }
    public string? ProjectPath { get; init; }
    public string? InstallDirectory { get; init; }
    public TimeSpan PipeConnectTimeout { get; init; } = TimeSpan.FromSeconds(5);
    public TimeSpan RequestTimeout { get; init; } = TimeSpan.FromSeconds(10);
    public TimeSpan LaunchReadyTimeout { get; init; } = TimeSpan.FromSeconds(45);
    public IReadOnlyList<string> Clients { get; init; } = [];
    public bool DryRun { get; init; }
    public bool AssumeYes { get; init; }
    public bool JsonOutput { get; init; }
    public string ToolSurface { get; init; } = "compact";

    public static AdapterOptions Parse(string[] args, bool? inputRedirected = null)
    {
        if (args.Any(argument => argument is "--help" or "-h"))
        {
            return new AdapterOptions { Mode = AdapterMode.Help };
        }

        var mode = args.Length == 0 && !(inputRedirected ?? Console.IsInputRedirected)
            ? AdapterMode.Setup
            : AdapterMode.Serve;
        var index = 0;
        if (args.Length > 0 && (args[0].Length == 0 || args[0][0] != '-'))
        {
            mode = ParseMode(args[0]);
            index = 1;
        }

        string? workspaceRoot = null;
        string? pipeName = null;
        string? defaultEngineExecutablePath = null;
        string? projectPath = null;
        string? installDirectory = null;
        TimeSpan pipeConnectTimeout = TimeSpan.FromSeconds(5);
        TimeSpan requestTimeout = TimeSpan.FromSeconds(10);
        TimeSpan launchReadyTimeout = TimeSpan.FromSeconds(45);
        var clients = new List<string>();
        var dryRun = false;
        var assumeYes = false;
        var jsonOutput = false;
        var toolSurface = "compact";

        for (; index < args.Length; index++)
        {
            var argument = args[index];
            switch (argument)
            {
                case "--tool-surface":
                    toolSurface = ReadValue(args, ref index, argument);
                    if (toolSurface is not ("compact" or "full"))
                        throw new AdapterOptionsException("--tool-surface must be compact or full.");
                    break;
                case "--workspace":
                    workspaceRoot = ReadValue(args, ref index, argument);
                    break;
                case "--pipe":
                    pipeName = ReadValue(args, ref index, argument);
                    break;
                case "--engine-exe":
                    defaultEngineExecutablePath = ReadValue(args, ref index, argument);
                    break;
                case "--project":
                    projectPath = ReadValue(args, ref index, argument);
                    break;
                case "--install-dir":
                    installDirectory = Path.GetFullPath(ReadValue(args, ref index, argument));
                    break;
                case "--pipe-timeout-seconds":
                    pipeConnectTimeout = TimeSpan.FromSeconds(ParsePositiveDouble(ReadValue(args, ref index, argument), argument));
                    break;
                case "--request-timeout-seconds":
                    requestTimeout = TimeSpan.FromSeconds(ParsePositiveDouble(ReadValue(args, ref index, argument), argument));
                    break;
                case "--launch-timeout-seconds":
                    launchReadyTimeout = TimeSpan.FromSeconds(ParsePositiveDouble(ReadValue(args, ref index, argument), argument));
                    break;
                case "--client":
                    clients.AddRange(ReadValue(args, ref index, argument)
                        .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));
                    break;
                case "--all":
                    clients.Add("all");
                    break;
                case "--dry-run":
                    dryRun = true;
                    break;
                case "--yes":
                case "-y":
                    assumeYes = true;
                    break;
                case "--json":
                    jsonOutput = true;
                    break;
                default:
                    throw new AdapterOptionsException($"Unknown argument '{argument}'.{Environment.NewLine}{BuildUsage()}");
            }
        }

        if (!string.IsNullOrWhiteSpace(workspaceRoot))
        {
            workspaceRoot = Path.GetFullPath(workspaceRoot);
            if (!Directory.Exists(workspaceRoot))
            {
                throw new AdapterOptionsException($"The --workspace directory does not exist: {workspaceRoot}");
            }
        }

        if (pipeName is not null && string.IsNullOrWhiteSpace(PipeNameUtility.Sanitize(pipeName)))
        {
            throw new AdapterOptionsException("The --pipe value must be a non-empty named pipe identifier.");
        }

        if (!string.IsNullOrWhiteSpace(defaultEngineExecutablePath))
        {
            defaultEngineExecutablePath = NormalizeExecutablePath(defaultEngineExecutablePath, "--engine-exe");
        }

        if (!string.IsNullOrWhiteSpace(projectPath))
        {
            projectPath = NormalizeProjectPath(projectPath, "--project");
        }
        return new AdapterOptions
        {
            Mode = mode,
            WorkspaceRoot = workspaceRoot,
            PipeNameOverride = pipeName,
            DefaultEngineExecutablePath = defaultEngineExecutablePath,
            ProjectPath = projectPath,
            InstallDirectory = installDirectory,
            PipeConnectTimeout = pipeConnectTimeout,
            RequestTimeout = requestTimeout,
            LaunchReadyTimeout = launchReadyTimeout,
            Clients = clients,
            DryRun = dryRun,
            AssumeYes = assumeYes,
            JsonOutput = jsonOutput,
            ToolSurface = toolSurface
        };
    }

    public static string NormalizeProjectPath(string projectPath, string optionName)
    {
        var normalizedProjectPath = Path.GetFullPath(projectPath);
        if (!normalizedProjectPath.EndsWith(".uproject", StringComparison.OrdinalIgnoreCase))
        {
            throw new AdapterOptionsException($"The {optionName} value must point to a .uproject file: {normalizedProjectPath}");
        }

        if (!File.Exists(normalizedProjectPath))
        {
            throw new AdapterOptionsException($"The {optionName} file does not exist: {normalizedProjectPath}");
        }

        return normalizedProjectPath;
    }

    public static string NormalizeExecutablePath(string executablePath, string optionName)
    {
        var normalizedExecutablePath = Path.GetFullPath(executablePath);
        if (!File.Exists(normalizedExecutablePath))
        {
            throw new AdapterOptionsException($"The {optionName} file does not exist: {normalizedExecutablePath}");
        }

        return normalizedExecutablePath;
    }

    public static string BuildUsage()
    {
        return string.Join(
            Environment.NewLine,
            "Usage:",
            "  unrealmcp help",
            "  unrealmcp                              Interactive setup when run in a terminal; MCP serve when stdin is redirected.",
            "  unrealmcp [serve] [--workspace <path>] [--engine-exe <UnrealEditor.exe path>] [--pipe <name>]",
            "  unrealmcp discover [--workspace <path>] [--engine-exe <UnrealEditor.exe path>]",
            "  unrealmcp launch --project <absolute .uproject path> [--engine-exe <UnrealEditor.exe path>] [--workspace <path>] [--pipe <name>]",
            "  unrealmcp status [--workspace <path>] [--engine-exe <UnrealEditor.exe path>] [--pipe <name>]",
            "  unrealmcp doctor [--json]",
            "  unrealmcp configure --client <name|all> [--install-dir <path>] [--dry-run] [--yes] [--json]",
            "  unrealmcp uninstall --client <name|all> [--install-dir <path>] [--dry-run] [--yes] [--json]",
            "",
            "Modes:",
            "  help       Print this help text.",
            "  setup      Run the interactive client setup wizard.",
            "  serve      Start the stdio MCP server. Default when omitted.",
            "  discover   Print discovered Unreal projects and engine association info as JSON.",
            "  launch     Launch one Unreal project and print JSON status.",
            "  status     Print adapter/discovery status as JSON.",
            "  doctor     Diagnose executable, protocol, projects, clients, and Unreal session state.",
            "  configure  Add UnrealMCP to selected MCP clients.",
            "  uninstall  Remove only UnrealMCP's entry from selected MCP clients.",
            "",
            "Options:",
            "  --tool-surface <compact|full>    Compact discovery/schema/call gateway (default), or all native schemas for legacy clients.",
            "  --workspace <path>              Optional discovery-root override. Otherwise the current working directory is used.",
            "  --engine-exe <path>             Optional default UnrealEditor.exe path for launch requests.",
            "  --project <path>                Optional .uproject path. Required for launch mode unless one project is discoverable.",
            "  --pipe <name>                   Optional named pipe override. Otherwise the adapter derives UnrealMCP_<ProjectName>.",
            "  --pipe-timeout-seconds <n>      Pipe connection timeout. Defaults to 5.",
            "  --request-timeout-seconds <n>   Pipe request timeout. Defaults to 10.",
            "  --launch-timeout-seconds <n>    Launch-and-ready timeout. Defaults to 45.",
            "  --install-dir <path>             Optional directory to copy the adapter into before client configuration.",
            "  --client <name[,name...]>       Client(s): codex, claude, cursor, cline, antigravity.",
            "  --all                           Select every detected MCP client.",
            "  --dry-run                       Show planned changes without modifying client configuration.",
            "  --yes, -y                       Apply configuration without an interactive confirmation.",
            "  --json                          Print machine-readable command output.");
    }

    private static AdapterMode ParseMode(string value)
    {
        return value.ToLowerInvariant() switch
        {
            "help" => AdapterMode.Help,
            "setup" => AdapterMode.Setup,
            "serve" => AdapterMode.Serve,
            "discover" => AdapterMode.Discover,
            "launch" => AdapterMode.Launch,
            "status" => AdapterMode.Status,
            "doctor" => AdapterMode.Doctor,
            "configure" => AdapterMode.Configure,
            "uninstall" => AdapterMode.Uninstall,
            _ => throw new AdapterOptionsException($"Unknown mode '{value}'.{Environment.NewLine}{BuildUsage()}")
        };
    }

    private static string ReadValue(string[] args, ref int index, string optionName)
    {
        if (index + 1 >= args.Length)
        {
            throw new AdapterOptionsException($"Missing value for {optionName}.{Environment.NewLine}{BuildUsage()}");
        }

        index++;
        return args[index];
    }

    private static double ParsePositiveDouble(string value, string optionName)
    {
        if (!double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var parsed) || parsed <= 0)
        {
            throw new AdapterOptionsException($"The value for {optionName} must be a positive number.");
        }

        return parsed;
    }
}

internal sealed class AdapterOptionsException(string message) : Exception(message);
