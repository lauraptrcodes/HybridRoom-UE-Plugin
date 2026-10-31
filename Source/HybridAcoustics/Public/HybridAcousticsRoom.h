// HybridAcousticsRoom.h — Box = Quaderraum. Hybrider Bake (Steam Audio + FDTD) im Hintergrund,
// Ergebnis wird unter Saved/HybridAcoustics zwischengespeichert und beim nächsten Start geladen,
// solange sich die Eingaben nicht geändert haben. Die IR des nächsten Empfängers geht an den Spatializer.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include <atomic>
#include "HybridAcousticsRoom.generated.h"

class UBoxComponent;
struct FHybridBakeResult; // nur im .cpp definiert -> keine Core-Typen im Public-Header

UENUM(BlueprintType)
enum class EHybridFdtdOrder : uint8
{
	Second UMETA(DisplayName = "2. Ordnung (7-Punkt)"),
	Fourth UMETA(DisplayName = "4. Ordnung (13-Punkt)")
};

UCLASS()
class HYBRIDACOUSTICS_API AHybridAcousticsRoom : public AActor
{
	GENERATED_BODY()

public:
	AHybridAcousticsRoom();

	// ================= CONFIG =================
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Hybrid Acoustics")
	TObjectPtr<UBoxComponent> RoomBounds;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Hybrid Acoustics")
	TObjectPtr<USceneComponent> SourcePoint;

	// Optional: Bake-Quelle = Position dieses Actors (z. B. dein Ambient Sound)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics")
	TObjectPtr<AActor> SoundSource;

	// Feste Übergangsfrequenz, nur wenn bAutoCrossover = false
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics", meta = (ClampMin = "50", ClampMax = "1000"))
	float MockSchroederHz = 200.f;

	// Übergangsfrequenz = Schroeder-Frequenz 2000*sqrt(T60/V), T60 aus Steam Audio (sonst Eyring)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics")
	bool bAutoCrossover = true;

	// Grenzen für die automatische Übergangsfrequenz (FDTD-Rechenzeit ~ fmax^4)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics", meta = (ClampMin = "30"))
	float MinCrossoverHz = 80.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics", meta = (ClampMin = "50"))
	float MaxCrossoverHz = 400.f;

	// FDTD rechnet bis FmaxFactor * Übergangsfrequenz, damit der Übergang sauber aufgelöst ist
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics", meta = (ClampMin = "1.0", ClampMax = "4.0"))
	float FmaxFactor = 2.f;

	// Gitterpunkte pro Wellenlänge bei fmax
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics", meta = (ClampMin = "6.0", ClampMax = "20.0"))
	float PointsPerLambda = 10.f;
	// Raumordnung der FDTD. 4. Ordnung: ~6 Punkte/Wellenlänge reichen für die Genauigkeit von 2. Ordnung mit 10
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics")
	EHybridFdtdOrder FdtdOrder = EHybridFdtdOrder::Second;
	// Absorptionsgrad der Wände (0 = schallhart, 1 = vollständig absorbierend)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WallAlpha = 0.17f;

	// Wird verwendet, wenn weder Steam Audio noch Eyring ein T60 liefern (z. B. WallAlpha = 0 ohne Steam Audio)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float FallbackT60 = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics", meta = (ClampMin = "0.1", ClampMax = "3.0"))
	float IrLengthSec = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics", meta = (ClampMin = "10"))
	float ReceiverSpacingCm = 50.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float DryGain = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float WetGain = 1.f;
	
		// Lage des Direktschalls in der IR (ms): Platz für den bandbegrenzten Impuls vor seiner Mitte
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics|Direktschall", meta = (ClampMin = "2", ClampMax = "20"))
	float DirectPreDelayMs = 5.f;
	
		// Durchgang des Hochton-Direktschalls durch Innenwände (dB); der Tiefton beugt in der FDTD-IR um die Wand
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics|Direktschall", meta = (ClampMin = "-100", ClampMax = "0"))
	float WallTransmissionDb = -25.f;
	
		// Quelle als Kugel für die Verdeckung: weicher Übergang an Wandkanten
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics|Direktschall", meta = (ClampMin = "1"))
	float SourceRadiusCm = 25.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics")
	bool bBakeOnBeginPlay = true;

	// Bake unter Saved/HybridAcoustics speichern und beim Start laden, wenn die Eingaben gleich sind
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics")
	bool bUseBakeCache = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics")
	bool bDrawDebug = true;

	// Schallharte Innenwände als Quader in Metern ab Raumecke
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics")
	TArray<FBox> InnerWallsM;
	UPROPERTY(VisibleAnywhere, Category = "Hybrid Acoustics")
	TArray<FBox> WallActorBoxesM;

	// Oder bequemer: Actors im Level (z. B. Würfel); ihre Bounding Box wird zur Wand
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics")
	TArray<TObjectPtr<AActor>> InnerWallActors;

	// Hochton aus Steam-Audio-Raytracing statt Rauschnachhall (SDK unter Source/ThirdParty/SteamAudio)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics|Steam Audio")
	bool bUseSteamAudio = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics|Steam Audio", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SteamScattering = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics|Steam Audio", meta = (ClampMin = "1024"))
	int32 SteamRays = 8192;
	// Direktschall über Steam-Audio-HRTF statt Panning (A/B-Vergleich)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics|Direktschall")
	bool bUseHrtf = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hybrid Acoustics|Direktschall")
	bool bDecorrelateReverb = true;


	// ==========================================

	// Bake starten; mit bForceRebake wird der Cache ignoriert und überschrieben
	UFUNCTION(BlueprintCallable, Category = "Hybrid Acoustics")
	void BakeAsync(bool bForceRebake = false);

	// Knopf im Details-Panel: im Editor backen und speichern -> Play lädt danach sofort
	UFUNCTION(CallInEditor, Category = "Hybrid Acoustics")
	void BakeNow();

	UFUNCTION(BlueprintPure, Category = "Hybrid Acoustics")
	bool IsBaked() const { return Bake.IsValid(); }

	UFUNCTION(CallInEditor, Category = "Hybrid Acoustics")
	void CaptureWalls();

	UFUNCTION(BlueprintPure, Category = "Hybrid Acoustics")
	float GetBakeProgress() const { return Progress->load(); }

	// T60 aus Steam Audio (Median 283-566 Hz), sonst Eyring/FallbackT60; <= 0 vor dem ersten Bake
	UFUNCTION(BlueprintPure, Category = "Hybrid Acoustics")
	float GetMeasuredT60() const;

	// Tatsächlich verwendete Übergangsfrequenz (vor dem ersten Bake: MockSchroederHz)
	UFUNCTION(BlueprintPure, Category = "Hybrid Acoustics")
	float GetCrossoverHz() const;

	UFUNCTION(BlueprintPure, Category = "Hybrid Acoustics")
	int32 GetCurrentReceiver() const { return CurrentReceiver; }

	// Pfad der Cache-Datei dieses Raums (Saved/HybridAcoustics/<Level>_<Actor>.hacbake)
	UFUNCTION(BlueprintPure, Category = "Hybrid Acoustics")
	FString GetBakeCachePath() const;

	// Raumgröße (m) und Quellposition in Raummetern (Ursprung = Raumecke)
	FVector GetRoomSizeMeters() const;
	FVector GetSourceMeters() const;
	FVector WorldToRoomMeters(const FVector& World) const;
	FVector RoomMetersToWorld(const FVector& Meters) const;
	// Alle Innenwände (Boxen + Actors) in Raummetern; gemeinsame Quelle für Bake und Visualisierung
	TArray<FBox> GetInnerWallsMeters() const;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	void PublishReceiver(int32 Receiver);
	FBox ActorBoxMeters(const AActor* A) const;

	TSharedPtr<FHybridBakeResult, ESPMode::ThreadSafe> Bake;
	std::atomic<bool> bBaking{ false };
	int32 CurrentReceiver = INDEX_NONE;
	bool bLastDecorrelate = true;

	// Mit dem Worker-Thread geteilt; überleben den Actor, falls der Bake noch läuft
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe>  CancelFlag = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	TSharedRef<std::atomic<float>, ESPMode::ThreadSafe> Progress = MakeShared<std::atomic<float>, ESPMode::ThreadSafe>(0.f);
};
