using UnrealBuildTool;

public class UAssetAgent : ModuleRules
{
    public UAssetAgent(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bEnableExceptions = true;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "UnrealEd", "BlueprintGraph", "KismetCompiler", "Json", "AssetRegistry"
        });
    }
}
