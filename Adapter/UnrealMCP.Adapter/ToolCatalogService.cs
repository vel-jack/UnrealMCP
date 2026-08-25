using System.Text.Json;
using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed class ToolCatalogService
{
    private readonly string _bundledPath = Path.Combine(AppContext.BaseDirectory, "unreal-tools.json");
    private readonly string _cachePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "UnrealMCP",
        "cache",
        "unreal-tools.json");

    public IReadOnlyList<JsonObject> Load()
    {
        foreach (var path in new[] { _cachePath, _bundledPath })
        {
            try
            {
                if (!File.Exists(path)) continue;
                var tools = JsonNode.Parse(File.ReadAllText(path))?["tools"] as JsonArray;
                if (tools is not null)
                    return tools.OfType<JsonObject>().Select(tool => (JsonObject)tool.DeepClone()).ToArray();
            }
            catch
            {
                // A damaged cache must not prevent lifecycle tools from loading.
            }
        }
        return [];
    }

    public void Save(IEnumerable<JsonNode?> tools)
    {
        var toolArray = tools.ToArray();
        if (toolArray.Length == 0) return;
        Directory.CreateDirectory(Path.GetDirectoryName(_cachePath)!);
        var payload = new JsonObject
        {
            ["formatVersion"] = 1,
            ["capturedUtc"] = DateTime.UtcNow.ToString("O"),
            ["tools"] = new JsonArray(toolArray.Select(tool => tool?.DeepClone()).ToArray())
        };
        var temporaryPath = _cachePath + ".tmp";
        File.WriteAllText(temporaryPath, payload.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        File.Move(temporaryPath, _cachePath, true);
    }
}
