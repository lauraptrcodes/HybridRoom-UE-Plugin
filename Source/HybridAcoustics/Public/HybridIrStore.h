// HybridIrStore.h — Übergabe der aktuellen IRs und Parameter vom Game-Thread an den Audio-Render-Thread
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Convolver.h"
#include <atomic>
#include <memory>

// Ein Satz IRs für einen Empfänger:
//  - NumHf = 0: Low ist die komplette Wet-IR (mono, auf beide Ohren)
//  - NumHf = 2: Low = Tiefton unter fc (mono), Hf[0]/Hf[1] = Hochton-Nachhall linkes/rechtes Ohr (dekorreliert)
struct FHybridIrSet
{
	std::shared_ptr<const fdtd::PartitionedIr> Low;
	std::shared_ptr<const fdtd::PartitionedIr> Hf[2];
	int32 NumHf = 0;
};

class HybridIrStore
{
public:
	static HybridIrStore& Get()
	{
		static HybridIrStore Instance;
		return Instance;
	}

	// ---------------------------------------------------------------- Game-Thread
	void Publish(std::shared_ptr<const FHybridIrSet> Set)
	{
		{
			FScopeLock Lock(&Cs);
			Current = MoveTemp(Set);
		}
		Version.fetch_add(1, std::memory_order_release);
	}

	// Mono-IR (ohne Ambisonics)
	void Publish(std::shared_ptr<const fdtd::PartitionedIr> Ir)
	{
		if (!Ir) { Publish(std::shared_ptr<const FHybridIrSet>()); return; }
		auto Set = std::make_shared<FHybridIrSet>();
		Set->Low = MoveTemp(Ir);
		Publish(std::shared_ptr<const FHybridIrSet>(MoveTemp(Set)));
	}

	void Clear() { Publish(std::shared_ptr<const FHybridIrSet>()); }

	void SetGains(float Dry, float Wet)
	{
		DryGain.store(Dry, std::memory_order_relaxed);
		WetGain.store(Wet, std::memory_order_relaxed);
	}

	// Direktschall-Pfad: Übergangsfrequenz und Vorlauf der IR (Samples); Verdeckungs-Verstärkung des Dry-Pfads
	void SetDirectParams(float Fc, int32 PreDelaySamples)
	{
		CrossoverHz.store(Fc, std::memory_order_relaxed);
		PreDelay.store(PreDelaySamples, std::memory_order_relaxed);
	}
	void SetDryOcclusionGain(float G) { DryOcclusion.store(G, std::memory_order_relaxed); }
	void SetUseHrtf(bool b) { bUseHrtf.store(b, std::memory_order_relaxed); }

	// ---------------------------------------------------------------- Audio-Thread
	// Lock nur, wenn sich die Version geändert hat
	uint32 GetVersion() const { return Version.load(std::memory_order_acquire); }

	std::shared_ptr<const FHybridIrSet> GetCurrent()
	{
		FScopeLock Lock(&Cs);
		return Current;
	}

	float GetDryGain() const { return DryGain.load(std::memory_order_relaxed); }
	float GetWetGain() const { return WetGain.load(std::memory_order_relaxed); }
	float GetCrossoverHz() const { return CrossoverHz.load(std::memory_order_relaxed); }
	int32 GetPreDelaySamples() const { return PreDelay.load(std::memory_order_relaxed); }
	float GetDryOcclusionGain() const { return DryOcclusion.load(std::memory_order_relaxed); }
	bool GetUseHrtf() const { return bUseHrtf.load(std::memory_order_relaxed); }

	static constexpr int32 BlockSize = 512; // Partitionsgröße = Latenz der Faltung in Samples

private:
	FCriticalSection Cs;
	std::shared_ptr<const FHybridIrSet> Current;
	std::atomic<uint32> Version{ 0 };
	std::atomic<float> DryGain{ 1.f };
	std::atomic<float> WetGain{ 1.f };
	std::atomic<float> CrossoverHz{ 200.f };
	std::atomic<int32> PreDelay{ 240 };
	std::atomic<float> DryOcclusion{ 1.f };
	std::atomic<bool> bUseHrtf{ true };
};
