using UnrealBuildTool;

public class Undertow : ModuleRules
{
	public Undertow(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "ChaosVehicles", "PhysicsCore", "UMG"
		});
		PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// "Sim/..." (engine-independent simulation) and "Game/..." (Unreal classes) are included relative to the module root.
		PublicIncludePaths.Add(ModuleDirectory);
	}
}
