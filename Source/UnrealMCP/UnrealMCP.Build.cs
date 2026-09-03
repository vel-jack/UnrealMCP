using UnrealBuildTool;

public class UnrealMCP : ModuleRules
{
    public UnrealMCP(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;
        // Keep non-unity builds predictable while improving Blueprint pin-default inspection.

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
                "InputCore",
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
