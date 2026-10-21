// Copyright (c) 2026 OctaviusLeo. MIT License.

using UnrealBuildTool;
using System.Collections.Generic;

public class SensorSimSandboxTarget : TargetRules
{
	public SensorSimSandboxTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		// UE 5.8 defaults. V5 (5.4 defaults) left newer warning-level defaults
		// unset, which UBT escalated to errors and which conflict with the
		// shared UnrealEditor build environment; V7 matches the 5.8 engine.
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("SensorSimSandbox");
	}
}
