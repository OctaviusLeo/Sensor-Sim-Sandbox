// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

using UnrealBuildTool;

public class SensorSimSandbox : ModuleRules
{
	public SensorSimSandbox(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			// Rosbridge WebSocket transport (Requirement 6.2) and JSON op framing.
			"WebSockets",
			"Json",
			"JsonUtilities"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});
	}
}
