// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

using UnrealBuildTool;

public class SensorSimSandbox : ModuleRules
{
	public SensorSimSandbox(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// This module uses a flat layout (sources in subfolders directly under
		// the module root, no Public/Private split). Under the UE 5.8 build
		// settings (bLegacyPublicIncludePaths = false) the module root is no
		// longer an implicit include path, so includes like
		// "Bridge/SmokePublisher.h" would not resolve. Add the module root
		// explicitly so subfolder-relative includes work.
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			// Camera_Model scene capture + render-target GPU readback (task 13).
			"RenderCore",
			"RHI",
			// Camera CompressedImage JPEG encoding via the image wrapper (task 14).
			"ImageWrapper",
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
