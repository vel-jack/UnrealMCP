using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal sealed record ProjectDescriptor(string ProjectPath, string ProjectName, string? EngineAssociation)
{
    public JsonObject ToJson()
    {
        return new JsonObject
        {
            ["projectPath"] = ProjectPath,
            ["projectName"] = ProjectName,
            ["engineAssociation"] = EngineAssociation
        };
    }
}
