using System.Text.Json.Nodes;

namespace UnrealMCP.Adapter;

internal static class JsonArgumentReaders
{
    public static string? ReadOptionalString(JsonNode? arguments, string propertyName)
    {
        return arguments is JsonObject argumentObject && argumentObject[propertyName] is JsonValue value
            ? value.GetValue<string>()
            : null;
    }

    public static string ReadRequiredString(JsonNode? arguments, string propertyName)
    {
        var value = ReadOptionalString(arguments, propertyName);
        if (string.IsNullOrWhiteSpace(value))
        {
            throw new InvalidOperationException($"Missing required adapter argument '{propertyName}'.");
        }

        return value;
    }
}
