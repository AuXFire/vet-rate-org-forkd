using UnrealBuildTool;
using System.Collections.Generic;

public class UndertowTarget : TargetRules
{
	public UndertowTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("Undertow");
	}
}
