#include "HybridSpatialization.h"
#include "HybridIrStore.h"

// Signalfluss pro Quelle (ProcessAudio):
//   Mono --+--> Low-Faltung (Tiefton: FDTD-Direktschall + Reflexionen, mono) -------------> beide Ohren
//          +--> Hf-Faltung links / Hf-Faltung rechts (Hochton-Nachhall, unkorreliert) -----> L / R
//          +--> Hochpass fc --> Verzögerung (Latenz + Vorlauf) --> Verdeckung --> HRTF --> L / R
// Ohne Hf-IRs (NumHf = 0): Low enthält die komplette Wet-IR (mono). Ohne HRTF: Equal-Power-Panning.

namespace
{
	constexpr float kInvSqrt2 = 0.70710678f;
}

void HybridSpatialization::Initialize(const FAudioPluginInitializationParams InitParams)
{
	InitialBufferLength = InitParams.BufferLength;
	SampleRate = (float)InitParams.SampleRate;

	// HRTF einmal anlegen (teuer, nicht im Audio-Thread); Blockgröße = Callback-Größe
	const bool bHrtfOk = Hrtf.create((int)SampleRate, InitialBufferLength);
	UE_LOG(LogTemp, Log, TEXT("HybridSpatialization: HRTF %s (fs=%d, Block=%d)"),
		bHrtfOk ? TEXT("aktiv") : TEXT("nicht verfügbar -> Panning"), (int)SampleRate, InitialBufferLength);

	Sources.SetNum(InitParams.NumSources);
	for (FSourceState& S : Sources)
	{
		const int32 N = InitialBufferLength;
		S.Mono.SetNumZeroed(N);
		S.Wet.SetNumZeroed(N);
		for (TArray<float>& H : S.HfOut) H.SetNumZeroed(N);
		S.DrySig.SetNumZeroed(N);
		S.DryDelay.SetNumZeroed(8192);   // 512 Latenz + bis 150 ms Vorlauf bei 48 kHz
		S.DryIn.SetNumZeroed(N);
		S.BinL.SetNumZeroed(N);
		S.BinR.SetNumZeroed(N);
		if (bHrtfOk) S.Bin.create(Hrtf);
	}
}

void HybridSpatialization::OnReleaseSource(const uint32 SourceId)
{
	if (!Sources.IsValidIndex(SourceId)) return;
	FSourceState& S = Sources[SourceId];

	// Nächste Quelle in diesem Slot startet ohne Reste der vorherigen
	if (S.Conv) S.Conv->reset();
	for (auto& C : S.HfConv) if (C) C->reset();
	S.bGainsInit = false;
	S.DryHp.reset();
	FMemory::Memzero(S.DryDelay.GetData(), S.DryDelay.Num() * sizeof(float));
	S.DelayWrite = 0;
	S.DryOcc = 1.f;
	S.Bin.reset();
}

void HybridSpatialization::ProcessAudio(const FAudioPluginSourceInputData& In, FAudioPluginSourceOutputData& Out)
{
	const int32 InCh = FMath::Max(1, In.NumChannels);
	const int32 Frames = In.AudioBuffer->Num() / InCh;
	const int32 OutCh = Frames > 0 ? Out.AudioBuffer.Num() / Frames : 2;
	if (Frames == 0 || !Sources.IsValidIndex(In.SourceId)) return;

	FSourceState& S = Sources[In.SourceId];
	HybridIrStore& Store = HybridIrStore::Get();

	// 1) Neue IRs übernehmen (nur wenn sich die Version geändert hat)
	const uint32 Version = Store.GetVersion();
	if (Version != S.IrVersion)
	{
		S.IrVersion = Version;
		std::shared_ptr<const FHybridIrSet> Set = Store.GetCurrent();
		if (Set && Set->Low)
		{
			if (!S.Conv) S.Conv = std::make_unique<fdtd::Convolver>(HybridIrStore::BlockSize);
			S.Conv->setIr(Set->Low);
			S.NumHf = Set->NumHf == 2 ? 2 : 0;
			for (int32 c = 0; c < S.NumHf; ++c)
			{
				if (!S.HfConv[c]) S.HfConv[c] = std::make_unique<fdtd::Convolver>(HybridIrStore::BlockSize);
				S.HfConv[c]->setIr(Set->Hf[c]);
			}
		}
		else
		{
			S.Conv.reset();          // unique_ptr::reset -> Convolver freigeben; danach nur noch Dry
			for (auto& C : S.HfConv) C.reset();
			S.NumHf = 0;
		}
	}

	// Puffer wachsen nur, falls der Callback größer ist als bei Initialize (Ausnahmefall)
	if (S.Mono.Num() < Frames)
	{
		S.Mono.SetNumZeroed(Frames); S.Wet.SetNumZeroed(Frames); S.DrySig.SetNumZeroed(Frames);
		for (TArray<float>& H : S.HfOut) H.SetNumZeroed(Frames);
		S.DryIn.SetNumZeroed(Frames); S.BinL.SetNumZeroed(Frames); S.BinR.SetNumZeroed(Frames);
	}

	// 2) Mono-Downmix
	const float* Src = In.AudioBuffer->GetData();
	for (int32 i = 0; i < Frames; ++i)
	{
		float Sum = 0.f;
		for (int32 c = 0; c < InCh; ++c) Sum += Src[i * InCh + c];
		S.Mono[i] = Sum / (float)InCh;
	}

	// 3) Faltung: Low (mono) und ggf. Hochton-Nachhall je Ohr
	if (S.Conv) S.Conv->process(S.Mono.GetData(), S.Wet.GetData(), Frames);
	else FMemory::Memzero(S.Wet.GetData(), Frames * sizeof(float));

	const bool bStereoWet = S.Conv && S.NumHf == 2;
	if (bStereoWet)
	{
		for (int32 c = 0; c < 2; ++c) S.HfConv[c]->process(S.Mono.GetData(), S.HfOut[c].GetData(), Frames);
	}

	// 4) Dry-Pfad: mit IR nur noch der Hochton-Direktschall (Tiefton inkl. Beugung steckt in der FDTD-IR),
	//    verzögert um Faltungslatenz + Vorlauf, damit Dry und Direktschall in der IR zusammenfallen.
	//    Ohne IR (vor dem Bake): Dry unverändert, voller Frequenzbereich.
	const bool bSplit = S.Conv != nullptr;
	const float Fc = Store.GetCrossoverHz();
	if (bSplit && Fc != S.HpFc) { S.DryHp.setup(Fc, SampleRate, true); S.HpFc = Fc; }
	const int32 DelayLen = S.DryDelay.Num();
	const int32 Delay = bSplit ? FMath::Clamp(HybridIrStore::BlockSize + Store.GetPreDelaySamples(), 0, DelayLen - 1) : 0;
	for (int32 i = 0; i < Frames; ++i)
	{
		const float X = bSplit ? S.DryHp.process(S.Mono[i]) : S.Mono[i];
		S.DryDelay[S.DelayWrite] = X;
		S.DrySig[i] = S.DryDelay[(S.DelayWrite - Delay + DelayLen) % DelayLen];
		S.DelayWrite = (S.DelayWrite + 1) % DelayLen;
	}
	const float OccTarget = bSplit ? Store.GetDryOcclusionGain() : 1.f;

	// 5) Richtung des Direktschalls (Hörerraum: x vorn, y rechts, z oben)
	float TargetL = kInvSqrt2, TargetR = kInvSqrt2;
	FVector Rel = FVector::ForwardVector;
	if (In.SpatializationParams)
	{
		Rel = In.SpatializationParams->EmitterPosition;
		const double Len = Rel.Size();
		if (Len > UE_KINDA_SMALL_NUMBER)
		{
			const float Pan = FMath::Clamp((float)(Rel.Y / Len), -1.f, 1.f);
			const float Theta = (Pan + 1.f) * 0.25f * UE_PI;
			TargetL = FMath::Cos(Theta);
			TargetR = FMath::Sin(Theta);
		}
	}
	if (!S.bGainsInit) { S.GainL = TargetL; S.GainR = TargetR; S.bGainsInit = true; }

	const float Dry = Store.GetDryGain(), WetG = Store.GetWetGain();
	float* Dst = Out.AudioBuffer.GetData();

	// 6a) HRTF für den Direktschall
	bool bHrtfDone = false;
	if (OutCh >= 2 && S.Bin.ok() && Store.GetUseHrtf() && In.SpatializationParams)
	{
		for (int32 i = 0; i < Frames; ++i)
		{
			const float T = (float)(i + 1) / (float)Frames;
			S.DryIn[i] = Dry * (S.DryOcc + (OccTarget - S.DryOcc) * T) * S.DrySig[i];
		}
		bHrtfDone = S.Bin.process(S.DryIn.GetData(), S.BinL.GetData(), S.BinR.GetData(), Frames,
			fdtd::Vec3{ Rel.X, Rel.Y, Rel.Z });
	}

	// 6b) Mischen
	for (int32 i = 0; i < Frames; ++i)
	{
		const float T = (float)(i + 1) / (float)Frames;
		const float W = WetG * S.Wet[i];                          // Tiefton (bzw. komplette Wet-IR ohne Hf)
		const float HL = bStereoWet ? WetG * S.HfOut[0][i] : 0.f;  // Hochton-Nachhall links
		const float HR = bStereoWet ? WetG * S.HfOut[1][i] : 0.f;  // Hochton-Nachhall rechts (unkorreliert)

		float DL, DR;
		if (bHrtfDone)
		{
			DL = S.BinL[i];
			DR = S.BinR[i];
		}
		else
		{
			const float gL = S.GainL + (TargetL - S.GainL) * T;
			const float gR = S.GainR + (TargetR - S.GainR) * T;
			const float Occ = S.DryOcc + (OccTarget - S.DryOcc) * T;
			const float D = Dry * Occ * S.DrySig[i];
			DL = gL * D;
			DR = gR * D;
		}

		if (OutCh >= 2)
		{
			Dst[i * OutCh + 0] = DL + W + HL;
			Dst[i * OutCh + 1] = DR + W + HR;
			for (int32 c = 2; c < OutCh; ++c) Dst[i * OutCh + c] = 0.f;
		}
		else
		{
			Dst[i] = (DL + DR) * kInvSqrt2 + W + (HL + HR) * kInvSqrt2;
		}
	}
	S.GainL = TargetL;
	S.GainR = TargetR;
	S.DryOcc = OccTarget;
}
