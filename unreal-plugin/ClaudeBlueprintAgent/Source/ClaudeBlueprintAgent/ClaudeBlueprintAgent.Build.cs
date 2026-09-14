// Claude Blueprint Agent - Editor-only module.
using UnrealBuildTool;

public class ClaudeBlueprintAgent : ModuleRules
{
	public ClaudeBlueprintAgent(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"InputCore",
			"Projects",
			"Json",
			"JsonUtilities",
			"HTTPServer",
			"Sockets",
			"AssetRegistry",
			"UnrealEd",
			"EditorSubsystem",
			"EditorFramework",
			"Kismet",
			"KismetCompiler",
			"BlueprintGraph",
			"GraphEditor",
			"UMG",
			"UMGEditor",
			"MovieScene",
			"MovieSceneTracks",
			"RenderCore",
			"RHI",
			"ImageWrapper",
			"ImageCore",
			"SourceControl",
			"DataValidation",
			"EditorScriptingUtilities",
			"ToolMenus",
			"DeveloperSettings",
		});

		// Keep engine-version specifics inside Private/Adapters.
		PrivateDefinitions.Add("CLAUDE_AGENT_PLUGIN_VERSION=TEXT(\"0.1.0\")");
	}
}
