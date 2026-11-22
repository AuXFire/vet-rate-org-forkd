using UnrealBuildTool;
using System.Collections.Generic;

public class UndertowEditorTarget : TargetRules
{
	public UndertowEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("Undertow");
	}
}
