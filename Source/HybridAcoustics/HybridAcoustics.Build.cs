// Plugins/HybridAcoustics/Source/HybridAcoustics/HybridAcoustics.Build.cs
using UnrealBuildTool;

public class HybridAcoustics : ModuleRules
{
    public HybridAcoustics(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // Was in Public/-Headern dieses Moduls vorkommt
        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "AudioExtensions"     // IAudioSpatialization, IAudioSpatializationFactory
			}
        );

        // Nur in Private/ gebraucht
        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                "AudioMixer",
                "SignalProcessing",   // Audio::FFT für die Faltung
				"HybridAcousticsCore" // FDTD-Kern
			}
        );
    }
}