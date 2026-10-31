

#pragma once

#include "CoreMinimal.h"
#include <IAudioExtensionPlugin.h>
#include <memory>
#include "Convolver.h"
#include "DirectPath.h"
#include "Binaural.h"
/**
 *  HYBRIDACOUSTICS_API 
 */
class HybridSpatialization : public IAudioSpatialization
{
public:
    HybridSpatialization() = default;
    HybridSpatialization(const HybridSpatialization&) = delete;
    HybridSpatialization & operator=(const HybridSpatialization&) = delete;
    virtual void Initialize(const FAudioPluginInitializationParams InitParams) override;
    virtual void OnReleaseSource(const uint32 SourceId) override;
    virtual void ProcessAudio(const FAudioPluginSourceInputData& In,
        FAudioPluginSourceOutputData& Out) override;

private:
    struct FSourceState
    {
        std::unique_ptr<fdtd::Convolver> Conv;   // std::unique_ptr: vollständiger Typ, kein Pimpl-Problem
        std::unique_ptr<fdtd::Convolver> HfConv[2];   // Hochton-Nachhall links / rechts
        TArray<float> HfOut[2];
        int32 NumHf = 0;
        uint32 IrVersion = 0;
        TArray<float> Mono;
        TArray<float> Wet;
        TArray<float> DrySig;          // Dry-Pfad nach Hochpass und Verzögerung
        TArray<float> DryDelay;        // Ringpuffer: Faltungslatenz + Vorlauf
        int32 DelayWrite = 0;
        fdtd::LinkwitzRiley4 DryHp;    // Hochpass bei fc, ergänzt den Tiefpass der FDTD-IR
        float HpFc = 0.f;
        float DryOcc = 1.f;
        fdtd::BinauralEffect Bin;      // HRTF des Direktschalls (Steam Audio)
        TArray<float> DryIn, BinL, BinR;
        float GainL = 0.70710678f;   // Panning-Pegel des letzten Blocks (für Überblendung)
        float GainR = 0.70710678f;
        bool  bGainsInit = false;
    };
    fdtd::HrtfContext Hrtf;
    TArray<FSourceState> Sources;
    int32 InitialBufferLength = 1024;
    float SampleRate = 48000.f;
};

class FHybridSpatializationFactory : public IAudioSpatializationFactory
{
public:
    virtual FString GetDisplayName() override { return TEXT("Hybrid Acoustics"); }
    virtual bool SupportsPlatform(const FString& PlatformName) override { return PlatformName == TEXT("Windows"); }
    virtual TAudioSpatializationPtr CreateNewSpatializationPlugin(FAudioDevice* OwningDevice) override
    {
        return TAudioSpatializationPtr(new HybridSpatialization());
    }
};
