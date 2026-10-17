// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

using UnrealBuildTool;
using System.Collections.Generic;

public class SensorSimSandboxEditorTarget : TargetRules
{
	public SensorSimSandboxEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("SensorSimSandbox");
	}
}
