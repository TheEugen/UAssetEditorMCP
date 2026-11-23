using UnrealBuildTool;

public class UAssetAgentNiagara : ModuleRules
{
    public UAssetAgentNiagara(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Json", "Niagara", "UAssetAgent" });
    }
}
