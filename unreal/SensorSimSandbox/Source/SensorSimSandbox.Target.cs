// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

using UnrealBuildTool;
using System.Collections.Generic;

public class SensorSimSandboxTarget : TargetRules
{
	public SensorSimSandboxTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("SensorSimSandbox");
	}
}
