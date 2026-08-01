using Microsoft.Win32;
using System.Runtime.Versioning;
using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed record EngineInstallCandidate(string RootPath, string EditorExecutablePath, string? AssociationKey, string DisplayVersion, string Source)
{
    public JsonObject ToJson()
    {
        return new JsonObject
        {
            ["rootPath"] = RootPath,
            ["editorExecutablePath"] = EditorExecutablePath,
            ["associationKey"] = AssociationKey,
            ["displayVersion"] = DisplayVersion,
            ["source"] = Source
        };
    }
}

internal sealed record EngineResolutionResult(
    bool Success,
    string? ErrorCode,
    string Message,
    string? EditorExecutablePath,
    string? ResolutionSource,
    string? EngineAssociation,
    IReadOnlyList<EngineInstallCandidate> Candidates)
{
    public JsonObject ToJson()
    {
        return new JsonObject
        {
            ["success"] = Success,
            ["errorCode"] = ErrorCode,
            ["message"] = Message,
            ["editorExecutablePath"] = EditorExecutablePath,
            ["resolutionSource"] = ResolutionSource,
            ["engineAssociation"] = EngineAssociation,
            ["candidates"] = new JsonArray(Candidates.Select(candidate => candidate.ToJson()).ToArray())
        };
    }
}

[SupportedOSPlatform("windows")]
internal sealed class EngineInstallationResolver
{
    public EngineResolutionResult Resolve(string? explicitEnginePath, string? selectedOverridePath, string? defaultEnginePath, string? engineAssociation)
    {
        if (!string.IsNullOrWhiteSpace(explicitEnginePath))
        {
            return ResolveExplicit(explicitEnginePath, "tool_argument");
        }

        if (!string.IsNullOrWhiteSpace(selectedOverridePath))
        {
            return ResolveExplicit(selectedOverridePath, "selected_project_override");
        }

        if (!string.IsNullOrWhiteSpace(defaultEnginePath))
        {
            return ResolveExplicit(defaultEnginePath, "adapter_default");
        }

        if (string.IsNullOrWhiteSpace(engineAssociation))
        {
            return new EngineResolutionResult(false, "engine_not_found", "No explicit Unreal Editor path was provided and the selected project has no EngineAssociation.", null, null, null, []);
        }

        var candidates = DiscoverInstalledEngines()
            .Where(candidate => MatchesAssociation(candidate, engineAssociation))
            .ToList();

        if (candidates.Count == 0)
        {
            return new EngineResolutionResult(false, "engine_not_found", $"No installed Unreal Editor matches EngineAssociation '{engineAssociation}'.", null, null, engineAssociation, []);
        }

        var exactAssociationMatches = candidates
            .Where(candidate => string.Equals(candidate.AssociationKey, engineAssociation, StringComparison.OrdinalIgnoreCase))
            .ToList();
        if (exactAssociationMatches.Count == 1)
        {
            return SuccessResult(exactAssociationMatches[0], engineAssociation, "engine_association_exact");
        }

        if (exactAssociationMatches.Count > 1)
        {
            return new EngineResolutionResult(false, "engine_resolution_ambiguous", $"Multiple Unreal Editor installs match EngineAssociation '{engineAssociation}'.", null, null, engineAssociation, exactAssociationMatches);
        }

        var normalizedAssociation = NormalizeAssociation(engineAssociation);
        var prefixMatches = candidates
            .Where(candidate => NormalizeAssociation(candidate.DisplayVersion).StartsWith(normalizedAssociation, StringComparison.OrdinalIgnoreCase))
            .OrderByDescending(candidate => candidate.DisplayVersion, StringComparer.OrdinalIgnoreCase)
            .ToList();

        if (prefixMatches.Count == 1)
        {
            return SuccessResult(prefixMatches[0], engineAssociation, "engine_association_prefix");
        }

        if (prefixMatches.Count > 1)
        {
            return new EngineResolutionResult(false, "engine_resolution_ambiguous", $"Multiple Unreal Editor installs satisfy EngineAssociation '{engineAssociation}'. Provide an explicit engine path.", null, null, engineAssociation, prefixMatches);
        }

        return new EngineResolutionResult(false, "engine_not_found", $"No installed Unreal Editor matches EngineAssociation '{engineAssociation}'.", null, null, engineAssociation, []);
    }

    public IReadOnlyList<EngineInstallCandidate> DiscoverInstalledEngines()
    {
        var candidates = new Dictionary<string, EngineInstallCandidate>(StringComparer.OrdinalIgnoreCase);
        AddRegistryCandidates(candidates);
        AddDefaultInstallCandidates(candidates);

        return candidates.Values
            .OrderBy(candidate => candidate.DisplayVersion, StringComparer.OrdinalIgnoreCase)
            .ThenBy(candidate => candidate.EditorExecutablePath, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    private static EngineResolutionResult ResolveExplicit(string enginePath, string source)
    {
        try
        {
            var normalizedExecutablePath = AdapterOptions.NormalizeExecutablePath(enginePath, "--engine-exe");
            var candidate = BuildCandidateFromExecutable(normalizedExecutablePath, null, source);
            return SuccessResult(candidate, null, source);
        }
        catch (AdapterOptionsException exception)
        {
            return new EngineResolutionResult(false, "engine_not_found", exception.Message, null, null, null, []);
        }
    }

    private static EngineResolutionResult SuccessResult(EngineInstallCandidate candidate, string? engineAssociation, string source)
    {
        return new EngineResolutionResult(true, null, $"Resolved Unreal Editor to '{candidate.EditorExecutablePath}'.", candidate.EditorExecutablePath, source, engineAssociation, [candidate]);
    }

    private static void AddRegistryCandidates(IDictionary<string, EngineInstallCandidate> candidates)
    {
        TryAddRegistryHive(candidates, Registry.CurrentUser, @"SOFTWARE\Epic Games\Unreal Engine\Builds", "registry_current_user_builds");
        TryAddRegistryHive(candidates, Registry.LocalMachine, @"SOFTWARE\EpicGames\Unreal Engine", "registry_local_machine");
        TryAddRegistryHive(candidates, Registry.LocalMachine, @"SOFTWARE\WOW6432Node\EpicGames\Unreal Engine", "registry_local_machine_wow6432");
    }

    private static void TryAddRegistryHive(IDictionary<string, EngineInstallCandidate> candidates, RegistryKey hive, string subKeyPath, string source)
    {
        try
        {
            using var key = hive.OpenSubKey(subKeyPath);
            if (key is null)
            {
                return;
            }

            foreach (var valueName in key.GetValueNames())
            {
                if (key.GetValue(valueName) is string stringValue)
                {
                    TryAddRootPathCandidate(candidates, stringValue, valueName, source);
                }
            }

            foreach (var subKeyName in key.GetSubKeyNames())
            {
                using var child = key.OpenSubKey(subKeyName);
                if (child is null)
                {
                    continue;
                }

                if (child.GetValue("InstalledDirectory") is string installedDirectory)
                {
                    TryAddRootPathCandidate(candidates, installedDirectory, subKeyName, source);
                }
                else if (child.GetValue(string.Empty) is string defaultValue)
                {
                    TryAddRootPathCandidate(candidates, defaultValue, subKeyName, source);
                }
            }
        }
        catch
        {
            // Registry lookups are best effort.
        }
    }

    private static void AddDefaultInstallCandidates(IDictionary<string, EngineInstallCandidate> candidates)
    {
        var roots = new[]
        {
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86)
        }
        .Where(path => !string.IsNullOrWhiteSpace(path))
        .Distinct(StringComparer.OrdinalIgnoreCase);

        foreach (var root in roots)
        {
            var epicGamesRoot = Path.Combine(root, "Epic Games");
            if (!Directory.Exists(epicGamesRoot))
            {
                continue;
            }

            foreach (var child in Directory.EnumerateDirectories(epicGamesRoot, "UE_*", SearchOption.TopDirectoryOnly))
            {
                TryAddRootPathCandidate(candidates, child, Path.GetFileName(child), "default_install_directory");
            }
        }
    }

    private static void TryAddRootPathCandidate(IDictionary<string, EngineInstallCandidate> candidates, string rootPath, string? associationKey, string source)
    {
        if (string.IsNullOrWhiteSpace(rootPath))
        {
            return;
        }

        var normalizedRootPath = Path.GetFullPath(rootPath);
        var executablePath = Path.Combine(normalizedRootPath, "Engine", "Binaries", "Win64", "UnrealEditor.exe");
        if (!File.Exists(executablePath))
        {
            return;
        }

        var candidate = BuildCandidateFromExecutable(executablePath, associationKey, source);
        candidates[candidate.EditorExecutablePath] = candidate;
    }

    private static EngineInstallCandidate BuildCandidateFromExecutable(string executablePath, string? associationKey, string source)
    {
        var rootPath = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(executablePath)!, "..", "..", ".."));
        var displayVersion = ExtractDisplayVersion(rootPath, associationKey);
        return new EngineInstallCandidate(rootPath, executablePath, associationKey, displayVersion, source);
    }

    private static bool MatchesAssociation(EngineInstallCandidate candidate, string engineAssociation)
    {
        return string.Equals(candidate.AssociationKey, engineAssociation, StringComparison.OrdinalIgnoreCase) ||
               NormalizeAssociation(candidate.DisplayVersion).StartsWith(NormalizeAssociation(engineAssociation), StringComparison.OrdinalIgnoreCase);
    }

    private static string ExtractDisplayVersion(string rootPath, string? associationKey)
    {
        if (!string.IsNullOrWhiteSpace(associationKey))
        {
            return associationKey;
        }

        var rootName = Path.GetFileName(rootPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        if (rootName.StartsWith("UE_", StringComparison.OrdinalIgnoreCase))
        {
            return rootName["UE_".Length..];
        }

        return rootName;
    }

    private static string NormalizeAssociation(string value)
    {
        return value
            .Trim()
            .Replace("UE_", string.Empty, StringComparison.OrdinalIgnoreCase)
            .Replace(".0", ".", StringComparison.OrdinalIgnoreCase)
            .TrimEnd('.');
    }
}
