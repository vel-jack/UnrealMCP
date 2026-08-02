namespace UnrealMCP.Adapter;

internal static class PipeNameUtility
{
    private const string BasePipeName = "UnrealMCP";

    public static string GetEffectivePipeName(string? explicitPipeName, string? projectName)
    {
        var sanitizedExplicitPipeName = Sanitize(explicitPipeName);
        if (!string.IsNullOrWhiteSpace(sanitizedExplicitPipeName))
        {
            return sanitizedExplicitPipeName;
        }

        var sanitizedProjectName = Sanitize(projectName);
        return string.IsNullOrWhiteSpace(sanitizedProjectName)
            ? BasePipeName
            : $"{BasePipeName}_{sanitizedProjectName}";
    }

    public static string Sanitize(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return string.Empty;
        }

        var buffer = new char[value.Length];
        var count = 0;
        foreach (var character in value)
        {
            if (char.IsLetterOrDigit(character) || character is '_' or '-')
            {
                buffer[count++] = character;
            }
        }

        return count == 0 ? string.Empty : new string(buffer, 0, count);
    }
}
