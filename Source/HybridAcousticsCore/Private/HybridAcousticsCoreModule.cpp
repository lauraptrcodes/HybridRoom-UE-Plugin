#include "HybridAcousticsCoreModule.h"
#include "Modules/ModuleManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace
{
	// Handle auf phonon.dll (Steam Audio); nur in dieser Datei sichtbar -> Header bleibt unverändert
	void* GPhononHandle = nullptr;
}

void FHybridAcousticsCore::StartupModule()
{
	// phonon.dll ist "delay-loaded": ohne diesen Schritt sucht Windows sie nur neben der .exe.
	// Hier wird sie aus Plugins/HybridAcoustics/Binaries/Win64 geladen, bevor der Kern sie benutzt.
#if HAC_WITH_STEAMAUDIO
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HybridAcoustics"));
	if (Plugin.IsValid())
	{
		const FString Dll = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries/Win64/phonon.dll"));
		GPhononHandle = FPlatformProcess::GetDllHandle(*Dll);
	}
	if (!GPhononHandle)
	{
		UE_LOG(LogTemp, Error, TEXT("HybridAcousticsCore: phonon.dll nicht geladen -> geometrischer Bake schlägt fehl"));
	}
#endif
}

void FHybridAcousticsCore::ShutdownModule()
{
	if (GPhononHandle)
	{
		FPlatformProcess::FreeDllHandle(GPhononHandle);
		GPhononHandle = nullptr;
	}
}

IMPLEMENT_MODULE(FHybridAcousticsCore, HybridAcousticsCore);
