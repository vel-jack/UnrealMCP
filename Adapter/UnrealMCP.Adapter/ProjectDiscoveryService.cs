using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed class ProjectDiscoveryService
{
    private static readonly HashSet<string> IgnoredDirectoryNames = new(StringComparer.OrdinalIgnoreCase)
    {
        ".git",
        ".vs",
        "Binaries",
        "DerivedDataCache",
        "Intermediate",
        "Saved",
        "obj",
        "bin"
    };

    public IReadOnlyList<ProjectDescriptor> DiscoverProjects(string? workspaceRoot)
    {
        if (string.IsNullOrWhiteSpace(workspaceRoot) || !Directory.Exists(workspaceRoot))
        {
            return [];
        }

        var results = new List<ProjectDescriptor>();
        DiscoverProjectsRecursive(Path.GetFullPath(workspaceRoot), results);
        return results
            .OrderBy(project => project.ProjectPath, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    public ProjectDescriptor LoadProject(string projectPath)
    {
        var normalizedProjectPath = AdapterOptions.NormalizeProjectPath(projectPath, "--project");
        var projectNode = JsonNode.Parse(File.ReadAllText(normalizedProjectPath)) as JsonObject;
        var engineAssociation = projectNode?["EngineAssociation"]?.GetValue<string>();
        return new ProjectDescriptor(
            normalizedProjectPath,
            Path.GetFileNameWithoutExtension(normalizedProjectPath),
            string.IsNullOrWhiteSpace(engineAssociation) ? null : engineAssociation);
    }

    private void DiscoverProjectsRecursive(string directoryPath, ICollection<ProjectDescriptor> results)
    {
        try
        {
            foreach (var filePath in Directory.EnumerateFiles(directoryPath, "*.uproject", SearchOption.TopDirectoryOnly))
            {
                try
                {
                    results.Add(LoadProject(filePath));
                }
                catch
                {
                    // Ignore malformed or inaccessible project descriptors; surfaced only when explicitly selected.
                }
            }
        }
        catch (UnauthorizedAccessException)
        {
            return;
        }
        catch (IOException)
        {
            return;
        }

        try
        {
            foreach (var childDirectory in Directory.EnumerateDirectories(directoryPath))
            {
                var directoryName = Path.GetFileName(childDirectory);
                if (IgnoredDirectoryNames.Contains(directoryName))
                {
                    continue;
                }

                DiscoverProjectsRecursive(childDirectory, results);
            }
        }
        catch (UnauthorizedAccessException)
        {
            return;
        }
        catch (IOException)
        {
            return;
        }

    }
}
