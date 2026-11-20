using UnrealBuildTool;

public class UAssetAgent : ModuleRules
{
    public UAssetAgent(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bEnableExceptions = true;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "UnrealEd", "BlueprintGraph", "Kismet", "KismetCompiler", "Json", "AssetRegistry",
            "AnimGraph", "AnimGraphRuntime", "UMG", "UMGEditor", "SlateCore", "MovieScene", "AssetTools", "RHI"
        });
    }
}
