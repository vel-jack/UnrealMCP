using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace UnrealMCP.Adapter;

internal sealed partial class McpRequestDispatcher
{
    private static readonly string[] DiscoveryNames =
        ["unreal.adapter.SearchTools", "unreal.adapter.GetToolSchema", "unreal.adapter.CallTool"];

    private static IReadOnlyList<McpToolDefinition> BuildDiscoveryTools() =>
    [
        new(DiscoveryNames[0], "Search native capabilities by task keywords. Returns bounded descriptions without schemas; empty query lists all. Offline results are cached hints.",
            CreateObjectSchema(("query", "string", "Keywords matched across tool name and description (all words).", false),
                ("offset", "integer", "Zero-based offset, default 0.", false),
                ("limit", "integer", "Page size 1-20, default 8.", false),
                ("expectedCatalogRevision", "string", "Optional revision from the previous page; rejects changed catalogs.", false))),
        new(DiscoveryNames[1], "Get the exact authoritative input schema for one native tool before calling it. Includes mutation operationId when applicable.",
            CreateObjectSchema(("name", "string", "Exact native tool name from SearchTools.", true))),
        new(DiscoveryNames[2], "Execute one native tool with its exact schema arguments. Preserves native validation, errors, confirmations, and mutation operationId/replay; no automatic retries.",
            CreateObjectSchema(("name", "string", "Exact native tool name; adapter tools cannot be nested.", true),
                ("arguments", "object", "Native arguments from GetToolSchema, including confirmations and operationId when needed.", true)))
    ];

    private async Task<(JsonObject Payload, bool IsError)?> TryInvokeDiscoveryAsync(
        string toolName, JsonNode? arguments, CancellationToken cancellationToken)
    {
        if (!DiscoveryNames.Contains(toolName, StringComparer.Ordinal)) return null;
        try
        {
            if (arguments is not JsonObject args) throw new ArgumentException("Arguments must be an object.");
            var catalog = (await GetAllToolDefinitionsAsync(cancellationToken))
                .Where(t => !t.Name.StartsWith("unreal.adapter.", StringComparison.Ordinal))
                .OrderBy(t => t.Name, StringComparer.Ordinal).ToArray();
            var serialized = JsonSerializer.Serialize(catalog.Select(t => new { name = t.Name, description = t.Description, inputSchema = t.InputSchema }));
            var revision = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(serialized))).ToLowerInvariant();
            var source = _sessionManager.GetCachedToolCatalog().Count > 0 ? "session_catalog" : "disk_cache";
            if (toolName == DiscoveryNames[0])
            {
                var expected = args["expectedCatalogRevision"]?.GetValue<string>();
                if (expected is not null && expected != revision)
                    return (DiscoveryError("catalog_revision_mismatch", "Catalog changed; restart search at offset 0."), true);
                var query = args["query"]?.GetValue<string>() ?? "";
                if (query.Length > 256) throw new ArgumentException("query must be at most 256 characters.");
                var offset = args["offset"]?.GetValue<int>() ?? 0;
                var limit = args["limit"]?.GetValue<int>() ?? 8;
                if (offset < 0 || limit < 1 || limit > 20) throw new ArgumentException("offset must be nonnegative and limit must be 1-20.");
                var words = Regex.Split(query.Trim(), @"\s+").Where(w => w.Length > 0).ToArray();
                var matches = catalog.Where(t => words.All(w =>
                    (t.Name + " " + t.Description).Contains(w, StringComparison.OrdinalIgnoreCase)))
                    .OrderByDescending(t => t.Name.Equals(query.Trim(), StringComparison.OrdinalIgnoreCase))
                    .ThenByDescending(t => words.Count(w => t.Name.Contains(w, StringComparison.OrdinalIgnoreCase)))
                    .ThenBy(t => t.Name, StringComparer.Ordinal).ToArray();
                var page = matches.Skip(offset).Take(limit).ToArray();
                var next = (long)offset + page.Length;
                return (new JsonObject
                {
                    ["success"] = true, ["source"] = source, ["catalogRevision"] = revision,
                    ["availabilityVerified"] = false, ["scannedCount"] = catalog.Length, ["matchedCount"] = matches.Length,
                    ["returnedCount"] = page.Length, ["offset"] = offset, ["hasMore"] = next < matches.Length,
                    ["nextOffset"] = next < matches.Length ? JsonValue.Create(next) : null,
                    ["coverageComplete"] = catalog.Length > 0, ["coverageScope"] = "available_catalog_only",
                    ["tools"] = new JsonArray(page.Select(t => (JsonNode)new JsonObject
                    {
                        ["name"] = t.Name, ["description"] = t.Description,
                        ["mutationTracked"] = UnrealSessionManager.IsMutationTool(t.Name)
                    }).ToArray())
                }, false);
            }

            var name = JsonArgumentReaders.ReadRequiredString(args, "name");
            var definition = catalog.FirstOrDefault(t => t.Name == name);
            if (definition is null)
                return (DiscoveryError("tool_not_in_catalog", "SearchTools or RefreshToolManifest before calling this exact native tool."), true);
            if (toolName == DiscoveryNames[1])
                return (new JsonObject
                {
                    ["success"] = true, ["source"] = source, ["catalogRevision"] = revision,
                    ["name"] = definition.Name, ["description"] = definition.Description,
                    ["inputSchema"] = definition.InputSchema.DeepClone(),
                    ["mutationTracked"] = UnrealSessionManager.IsMutationTool(name)
                }, false);

            if (args["arguments"] is not JsonObject nativeArgs)
                throw new ArgumentException("CallTool requires an arguments object, including for zero-argument tools.");
            // Forward through the existing dispatcher, preserving tracking, native validation and error payloads.
            // Only catalogued native names reach this point, so discovery/lifecycle recursion is impossible.
            return await InvokeToolPayloadAsync(name, nativeArgs, cancellationToken);
        }
        catch (Exception exception) when (exception is ArgumentException or InvalidOperationException or FormatException or JsonException)
        {
            return (DiscoveryError("adapter_invalid_request", exception.Message), true);
        }
    }

    private static JsonObject DiscoveryError(string code, string message) => new()
    {
        ["success"] = false, ["errorCode"] = code, ["message"] = message
    };
}
