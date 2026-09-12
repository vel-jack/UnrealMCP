using System.Text;
using System.Text.Json.Nodes;
using UnrealMCP.Adapter;

internal static class DiscoveryRegression
{
    private static JsonNode Payload(JsonObject response) => response["result"]!["structuredContent"]!;
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException("FAIL: " + message);
        Console.WriteLine("PASS: " + message);
    }

    public static async Task Run(string[] fullArgs, FakeNativePipe pipe)
    {
        Check(AdapterOptions.Parse(["serve"], true).ToolSurface == "compact", "Compact discovery is the default");
        var compactArgs = fullArgs.ToArray();
        compactArgs[Array.IndexOf(compactArgs, "full")] = "compact";
        pipe.Available = true;
        pipe.IncludeNewTool = false;
        await using var client = new StdioClient(Environment.ProcessPath!, compactArgs);
        await client.Initialize();
        var list = await client.Request("tools/list", new JsonObject());
        Check(list["result"]!["tools"]!.AsArray().Count == 14, "Compact surface contains only 11 lifecycle and three discovery tools");
        var search = Payload(await client.Call("unreal.adapter.SearchTools", new JsonObject { ["query"] = "regression", ["limit"] = 1 }));
        Check(search["tools"]!.AsArray()[0]!["inputSchema"] is null && search["matchedCount"]!.GetValue<int>() == 4,
            "Search omits schemas and reports exact catalog coverage");
        var page = Payload(await client.Call("unreal.adapter.SearchTools", new JsonObject { ["query"] = "regression", ["offset"] = 1, ["limit"] = 1,
            ["expectedCatalogRevision"] = search["catalogRevision"]!.DeepClone() }));
        Check(page["returnedCount"]!.GetValue<int>() == 1, "Catalog pagination preserves revision");
        var schema = Payload(await client.Call("unreal.adapter.GetToolSchema", new JsonObject { ["name"] = "RemoveInputMappingContextMapping" }));
        Check(schema["inputSchema"]!["properties"]!["operationId"] is not null, "On-demand mutation schema includes operationId");
        var call = Payload(await client.Call("unreal.adapter.CallTool", new JsonObject { ["name"] = "GetRegressionInitial", ["arguments"] = new JsonObject() }));
        Check(call["success"]!.GetValue<bool>(), "Gateway invokes a native tool absent from tools/list");
        var timeout = Payload(await client.Call("unreal.adapter.CallTool", new JsonObject { ["name"] = "RemoveInputMappingContextMapping",
            ["arguments"] = new JsonObject { ["operationId"] = "gateway-removal" } }));
        Check(timeout["errorCode"]!.GetValue<string>() == "mutation_request_timeout" &&
            timeout["operationId"]!.GetValue<string>() == "gateway-removal" && !timeout["canRetry"]!.GetValue<bool>() &&
            pipe.LastOperationId == "gateway-removal", "Gateway preserves uncertain mutation identity and retry prohibition");
        foreach (var name in new[] { "unknown", "unreal.adapter.CallTool", "unreal.adapter.RequestUnrealShutdown" })
            Check(Payload(await client.Call("unreal.adapter.CallTool", new JsonObject { ["name"] = name, ["arguments"] = new JsonObject() }))
                ["errorCode"]!.GetValue<string>() == "tool_not_in_catalog", "Gateway rejects unknown/nested name: " + name);
        Check(Payload(await client.Call("unreal.adapter.CallTool", new JsonObject { ["name"] = "GetRegressionInitial" }))
            ["errorCode"]!.GetValue<string>() == "adapter_invalid_request", "Gateway rejects missing native arguments");
        Check(Payload(await client.Call("unreal.adapter.SearchTools", new JsonObject { ["limit"] = 21 }))
            ["errorCode"]!.GetValue<string>() == "adapter_invalid_request", "Search rejects oversized page");
        pipe.IncludeNewTool = true;
        var changed = Payload(await client.Call("unreal.adapter.SearchTools", new JsonObject { ["expectedCatalogRevision"] = search["catalogRevision"]!.DeepClone() }));
        Check(changed["errorCode"]!.GetValue<string>() == "catalog_revision_mismatch", "Changed catalog rejects stale pagination");
        pipe.Available = false;
        var offline = Payload(await client.Call("unreal.adapter.SearchTools", new JsonObject { ["query"] = "GetRegressionNew" }));
        Check(offline["matchedCount"]!.GetValue<int>() == 1 && !offline["availabilityVerified"]!.GetValue<bool>(), "Offline search retains catalog without claiming availability");
        Check((await client.Call("unreal.adapter.CallTool", new JsonObject { ["name"] = "GetRegressionNew", ["arguments"] = new JsonObject() }))
            ["result"]!["isError"]!.GetValue<bool>(), "Offline gateway returns native availability failure");
        pipe.Available = true;
        await client.Call("unreal.adapter.RequestUnrealShutdown", new JsonObject());
        Check(pipe.SaveAllCalls == 0, "Shutdown does not issue an implicit save-all operation");
    }

    public static async Task Live(string executable, string project, JsonObject fullList)
    {
        await using var client = new StdioClient(executable, ["serve", "--project", project]);
        await client.Initialize();
        var compactList = await client.Request("tools/list", new JsonObject());
        var fullBytes = Encoding.UTF8.GetByteCount(fullList["result"]!.ToJsonString());
        var compactBytes = Encoding.UTF8.GetByteCount(compactList["result"]!.ToJsonString());
        Check(compactBytes < fullBytes / 4, "Live compact tool catalog is at least 75% smaller in serialized UTF-8 bytes");
        Console.WriteLine($"CATALOG_BYTES full={fullBytes} compact={compactBytes} reduction={100.0 * (fullBytes - compactBytes) / fullBytes:F1}% (not model-token measurements)");
        var search = Payload(await client.Call("unreal.adapter.SearchTools", new JsonObject { ["query"] = "GetBlueprintOverview" }));
        Check(search["matchedCount"]!.GetValue<int>() == 1, "Live discovery finds rebuilt Blueprint overview");
        var schema = Payload(await client.Call("unreal.adapter.GetToolSchema", new JsonObject { ["name"] = "GetBlueprintOverview" }));
        Check(schema["inputSchema"]!["properties"]!["objectPath"] is not null, "Live schema is available on demand");
        var invalid = await client.Call("unreal.adapter.CallTool", new JsonObject { ["name"] = "GetBlueprintOverview", ["arguments"] = new JsonObject() });
        Check(invalid["result"]!["isError"]!.GetValue<bool>(), "Gateway preserves native invalid-parameter errors");
        var test = Payload(await client.Call("unreal.adapter.CallTool", new JsonObject { ["name"] = "RunUnrealMCPAutomationTest",
            ["arguments"] = new JsonObject { ["testName"] = "UnrealMCP.Blueprint.Overview.ReadOnly", ["confirm"] = true } }));
        Console.WriteLine(test.ToJsonString());
        Check(test["passed"]!.GetValue<bool>(), "Native overview regression passes through compact MCP gateway");
    }
}
