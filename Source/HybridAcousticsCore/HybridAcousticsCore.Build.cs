using System.IO;
using UnrealBuildTool;

public class HybridAcousticsCore : ModuleRules
{
    public HybridAcousticsCore(ReadOnlyTargetRules Target) : base(Target)
    {
        // Kein UE-PCH -> fehlende Includes fallen sofort auf, keine versteckte UE-Abhängigkeit
        PCHUsage = PCHUsageMode.NoPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        bUseRTTI = false;
        bEnableExceptions = false;

        bUseUnity = false;   // jede .cpp einzeln kompilieren (wie in CMake), keine Unity-Kollisionen

        PublicDependencyModuleNames.Add("Core");
        PrivateDependencyModuleNames.Add("Projects");   // IPluginManager: Pfad zu phonon.dll

        // ---- Steam Audio SDK (Source/ThirdParty/SteamAudio/{include, lib/windows-x64}) ----
        string SteamDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "ThirdParty", "SteamAudio"));
        string LibDir = Path.Combine(SteamDir, "lib", "windows-x64");
        bool bHasSteam = Target.Platform == UnrealTargetPlatform.Win64
                       && File.Exists(Path.Combine(SteamDir, "include", "phonon.h"))
                       && File.Exists(Path.Combine(LibDir, "phonon.lib"));

        if (bHasSteam)
        {
            PublicSystemIncludePaths.Add(Path.Combine(SteamDir, "include"));   // System -> keine Warnungen aus phonon.h
            PublicAdditionalLibraries.Add(Path.Combine(LibDir, "phonon.lib"));
            PublicDelayLoadDLLs.Add("phonon.dll");                               // erst beim Modulstart laden
            RuntimeDependencies.Add("$(BinaryOutputDir)/phonon.dll", Path.Combine(LibDir, "phonon.dll"));
            PublicDefinitions.Add("HAC_WITH_STEAMAUDIO=1");
        }
        else
        {
            PublicDefinitions.Add("HAC_WITH_STEAMAUDIO=0");
            System.Console.WriteLine("HybridAcousticsCore: Steam Audio nicht gefunden unter " + SteamDir + " -> geometrischer Bake deaktiviert");
        }
    }
}
