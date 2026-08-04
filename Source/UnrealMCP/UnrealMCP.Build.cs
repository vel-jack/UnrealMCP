using UnrealBuildTool;

public class UnrealMCP : ModuleRules
{
    public UnrealMCP(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;
        // Keep non-unity builds predictable while adding, splitting, and testing tool source files.

        CppStandard = CppStandardVersion.Cpp20;

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "DeveloperSettings"
            });

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "AssetRegistry",
                "BlueprintGraph",
                "Json",
                "JsonUtilities",
                "LevelEditor",
                "Projects",
                "SQLiteCore",
                "Slate",
                "SlateCore",
                "Sockets",
                "ToolMenus",
                "UnrealEd"
            });
    }
}
