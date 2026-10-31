#include "HybridAcousticsRoom.h"
#include "HybridIrStore.h"
//#include "HybridFieldViewComponent.h"
#include "Baker.h"
#include "Hybrid.h"
#include "Convolver.h"
#include "GeometricBake.h"
#include "HybridBake.h"
#include "DirectPath.h"

#include "Components/BoxComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "AudioDevice.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "DrawDebugHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// Ergebnis des hybriden Bakes (Kern-Typ), geteilt zwischen Worker und Game-Thread
struct FHybridBakeResult
{
	fdtd::HybridBakeResult R;
};

namespace
{
	const TCHAR* T60SourceName(fdtd::T60Source S)
	{
		switch (S)
		{
		case fdtd::T60Source::Geometric: return TEXT("Steam Audio");
		case fdtd::T60Source::Eyring:    return TEXT("Eyring");
		default:                         return TEXT("FallbackT60");
		}
	}
} // namespace

AHybridAcousticsRoom::AHybridAcousticsRoom()
{
	PrimaryActorTick.bCanEverTick = true;

	RoomBounds = CreateDefaultSubobject<UBoxComponent>(TEXT("RoomBounds"));
	RoomBounds->SetBoxExtent(FVector(250.f, 200.f, 150.f)); // 5 x 4 x 3 m
	RoomBounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RootComponent = RoomBounds;

	SourcePoint = CreateDefaultSubobject<USceneComponent>(TEXT("SourcePoint"));
	SourcePoint->SetupAttachment(RoomBounds);
	SourcePoint->SetRelativeLocation(FVector(-130.f, -100.f, 0.f));
}

// ---------------------------------------------------------------- Koordinaten
FVector AHybridAcousticsRoom::WorldToRoomMeters(const FVector& World) const
{
	const FVector Local = RoomBounds->GetComponentTransform().InverseTransformPosition(World);
	return (Local + RoomBounds->GetUnscaledBoxExtent()) * RoomBounds->GetComponentScale() / 100.0;
}

FVector AHybridAcousticsRoom::RoomMetersToWorld(const FVector& Meters) const
{
	const FVector Local = Meters * 100.0 / RoomBounds->GetComponentScale() - RoomBounds->GetUnscaledBoxExtent();
	return RoomBounds->GetComponentTransform().TransformPosition(Local);
}

FVector AHybridAcousticsRoom::GetRoomSizeMeters() const
{
	// Box-Ausdehnung ist die halbe Kantenlänge in cm -> volle Länge in m
	return 2.0 * RoomBounds->GetScaledBoxExtent() / 100.0;
}

FVector AHybridAcousticsRoom::GetSourceMeters() const
{
	// Bake-Quelle: zugewiesener SoundSource-Actor, sonst die SourcePoint-Komponente
	const FVector SrcWorld = SoundSource ? SoundSource->GetActorLocation() : SourcePoint->GetComponentLocation();
	return WorldToRoomMeters(SrcWorld);
}


// ---------------------------------------------------------------- Abfragen
float AHybridAcousticsRoom::GetMeasuredT60() const
{
	return Bake.IsValid() ? Bake->R.t60 : -1.f;
}

float AHybridAcousticsRoom::GetCrossoverHz() const
{
	return Bake.IsValid() ? Bake->R.fc : MockSchroederHz;
}

FString AHybridAcousticsRoom::GetBakeCachePath() const
{
	// PIE-Welten heißen "UEDPIE_0_<Level>" -> Präfix entfernen, damit Editor und Play dieselbe Datei nutzen
	const FString Map = GetWorld() ? UWorld::RemovePIEPrefix(GetWorld()->GetMapName()) : FString(TEXT("NoWorld"));
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("HybridAcoustics"),
		FString::Printf(TEXT("%s_%s.hacbake"), *Map, *GetName()));
}

// ---------------------------------------------------------------- Lebenszyklus
void AHybridAcousticsRoom::BeginPlay()
{
	Super::BeginPlay();
	if (bBakeOnBeginPlay) BakeAsync(false);
}

void AHybridAcousticsRoom::EndPlay(const EEndPlayReason::Type Reason)
{
	CancelFlag->store(true);         // laufenden Bake abbrechen
	HybridIrStore::Get().Clear();   // Spatializer fällt auf "nur dry" zurück
	Super::EndPlay(Reason);
}

// ---------------------------------------------------------------- Bake
void AHybridAcousticsRoom::BakeAsync(bool bForceRebake)
{
	if (bBaking.exchange(true)) return;
	CancelFlag->store(false);
	Progress->store(0.f);

	// --- alle Eingaben des Bakes (bestimmen auch den Cache-Schlüssel) ---
	const FVector SizeM = GetRoomSizeMeters();
	const FVector SrcM = GetSourceMeters();

	fdtd::HybridBakeInput In;
	In.cfg.Lx = SizeM.X; In.cfg.Ly = SizeM.Y; In.cfg.Lz = SizeM.Z;
	In.cfg.pointsPerLambda = PointsPerLambda;
	In.cfg.pmlThickness = 0;            // geschlossener Raum -> keine PML
	In.cfg.spatialOrder = FdtdOrder == EHybridFdtdOrder::Fourth ? 4 : 2;
	In.settings.source = fdtd::Vec3{ SrcM.X, SrcM.Y, SrcM.Z };
	In.settings.irLength = IrLengthSec;
	In.settings.receiverSpacing = ReceiverSpacingCm / 100.0;
	In.settings.wallAlpha = WallAlpha;
	In.settings.outRate = 48000;
	for (const FBox& W : GetInnerWallsMeters())
	{
		In.settings.innerWalls.push_back(fdtd::Box{ fdtd::Vec3{ W.Min.X, W.Min.Y, W.Min.Z },
													fdtd::Vec3{ W.Max.X, W.Max.Y, W.Max.Z } });
	}
	// IR muss mit der Samplerate des Audio-Device übereinstimmen
	if (UWorld* World = GetWorld())
	{
		if (FAudioDeviceHandle Device = World->GetAudioDevice())
		{
			In.settings.outRate = (int)Device->GetSampleRate();
		}
	}

	In.useGeo = bUseSteamAudio;
	In.geo.scattering = SteamScattering;
	In.geo.numRays = SteamRays;
	In.geo.numThreads = FMath::Max(1, FPlatformMisc::NumberOfCoresIncludingHyperthreads() - 1);

	In.crossover.autoFc = bAutoCrossover;
	In.crossover.fixedFc = MockSchroederHz;
	In.crossover.fcMin = MinCrossoverHz;
	In.crossover.fcMax = FMath::Max(MinCrossoverHz, MaxCrossoverHz);
	In.crossover.fmaxFactor = FmaxFactor;
	In.crossover.fallbackT60 = FallbackT60;

	const uint64 Key = fdtd::hashInput(In);
	const FString CachePath = GetBakeCachePath();
	const FString InputDesc = UTF8_TO_TCHAR(fdtd::describeInput(In).c_str());
	const bool bTryLoad = bUseBakeCache && !bForceRebake;
	const bool bSave = bUseBakeCache || bForceRebake;

	TWeakObjectPtr<AHybridAcousticsRoom> WeakThis(this);
	auto Cancel = CancelFlag;   // geteilte Flags in den Worker kopieren, nicht den Actor
	auto Prog = Progress;

	Async(EAsyncExecution::Thread, [WeakThis, In, Key, CachePath, InputDesc, bTryLoad, bSave, Cancel, Prog]()
		{
			TSharedPtr<FHybridBakeResult, ESPMode::ThreadSafe> Result = MakeShared<FHybridBakeResult, ESPMode::ThreadSafe>();
			fdtd::HybridBakeResult& R = Result->R;
			const double T0 = FPlatformTime::Seconds();

			// 1) Cache: gleiche Eingaben (Schlüssel) -> laden statt rechnen
			bool bLoaded = false;
			if (bTryLoad)
			{
				TArray64<uint8> Bytes;
				if (FFileHelper::LoadFileToArray(Bytes, *CachePath, FILEREAD_Silent))
				{
					std::string Why;
					bLoaded = fdtd::deserializeBake(Bytes.GetData(), (size_t)Bytes.Num(), Key, R, &Why);
					if (!bLoaded)
					{
						// Diagnose: welche Eingabe weicht ab? Gespeicherte Beschreibung liegt neben dem Cache (.txt)
						FString SavedDesc;
						FFileHelper::LoadFileToString(SavedDesc, *(CachePath + TEXT(".txt")));
						UE_LOG(LogTemp, Warning, TEXT("HybridAcoustics: Cache nicht verwendbar: %s (Datei %lld Bytes, Schlüssel Datei %llu, aktuell %llu) -> neu backen"),
						UTF8_TO_TCHAR(Why.c_str()), (int64)Bytes.Num(),
						(uint64)fdtd::peekBakeKey(Bytes.GetData(), (size_t)Bytes.Num()), Key);
						UE_LOG(LogTemp, Warning, TEXT("--- gespeichert:\n%s--- aktuell:\n%s"), *SavedDesc, *InputDesc);
					}
				}
				else
				{
					UE_LOG(LogTemp, Log, TEXT("HybridAcoustics: kein Cache unter %s"), *CachePath);
				}
			}

			// 2) sonst backen (Steam Audio -> T60 -> Schroeder-Frequenz -> FDTD) und speichern
			if (!bLoaded)
			{
				auto PFor = [](int N, const std::function<void(int)>& Body)
					{
						ParallelFor(N, [&Body](int32 i) { Body(i); });
					};
				auto OnProgress = [Cancel, Prog](float X)
					{
						Prog->store(X);
						return !Cancel->load();   // false -> Bake bricht ab
					};
				R = fdtd::runHybridBake(In, PFor, OnProgress);

				if (!R.aborted && R.grid.numReceivers() > 0 && bSave)
				{
					const std::vector<uint8_t> Buf = fdtd::serializeBake(Key, R);
					IFileManager::Get().MakeDirectory(*FPaths::GetPath(CachePath), true);
					const bool bOk = FFileHelper::SaveArrayToFile(TArrayView64<const uint8>(Buf.data(), (int64)Buf.size()), *CachePath);
					FFileHelper::SaveStringToFile(InputDesc, *(CachePath + TEXT(".txt")));
					UE_LOG(LogTemp, Log, TEXT("HybridAcoustics: Bake-Cache %s: %s (%.1f MB)"),
						bOk ? TEXT("gespeichert") : TEXT("konnte NICHT gespeichert werden"), *CachePath,
						Buf.size() / (1024.0 * 1024.0));
				}
			}

			const bool bAborted = R.aborted || R.grid.numReceivers() == 0;
			if (!bAborted)
			{
				const double V = R.cfg.Lx * R.cfg.Ly * R.cfg.Lz;
				UE_LOG(LogTemp, Log, TEXT("HybridAcoustics: %s in %.1f s. T60=%.2f s (%s), V=%.1f m^3 -> f_S=%.0f Hz -> Übergang %.0f Hz, FDTD bis %.0f Hz"),
					bLoaded ? TEXT("aus Cache geladen") : TEXT("neu gebacken"), FPlatformTime::Seconds() - T0,
					R.t60, T60SourceName(R.t60Source), V, R.fs, R.fc, R.cfg.fmax);
				//UE_LOG(LogTemp, Log, TEXT("HybridAcoustics: FDTD %d. Ordnung, %dx%dx%d, dx=%.3f m, %d Empfänger; Steam Audio %d Probes, %d Reflexionen/Strahl"),
					//R.cfg.Nx, R.cfg.Ny, R.cfg.Nz, R.cfg.dx, R.grid.numReceivers(), R.geo.numReceivers(), R.geoStats.bounces);
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("HybridAcoustics bake: abgebrochen"));
			}

			AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, bAborted]()
				{
					AHybridAcousticsRoom* Self = WeakThis.Get();
					if (!Self) return;

					Self->bBaking = false;
					Self->Progress->store(bAborted ? 0.f : 1.f);
					if (bAborted) return;

					// Im Editor (Knopf "Bake Now") nur speichern, nichts im Speicher halten
					UWorld* World = Self->GetWorld();
					if (!World || !World->IsGameWorld()) return;

					Self->Bake = Result;
					Self->CurrentReceiver = INDEX_NONE; // erzwingt Publish im nächsten Tick

					// Visualisierung mit der neuen Übergangsfrequenz neu aufsetzen
					//if (UHybridFieldViewComponent* View = Self->FindComponentByClass<UHybridFieldViewComponent>())
					//{
					//	View->Rebuild();
					//}
				});
		});
}

// ---------------------------------------------------------------- IR an den Spatializer
void AHybridAcousticsRoom::PublishReceiver(int32 Receiver)
{
	const fdtd::HybridBakeResult& R = Bake->R;
	const double PreDelay = DirectPreDelayMs / 1000.0;
	
		// IR relativ zum Direktschall: Tiefton-Direktschall (FDTD, inkl. Beugung) = Einheitsimpuls bei PreDelay,
		// dazu die Reflexionen. Der Hochton-Direktschall kommt aus dem Dry-Pfad (Hochpass bei fc, gleich verzögert).
	//const std::vector<float> Ir = fdtd::receiverIr(R, Receiver, PreDelay);
	
	HybridIrStore & Store = HybridIrStore::Get();
	Store.SetDirectParams(R.fc, (int32)FMath::RoundToInt(PreDelay * R.grid.sampleRate));
		// Dekorreliert: Tiefton mono + Hochton-Nachhall je Ohr
	fdtd::StereoIrSet St;
	if (bDecorrelateReverb && fdtd::receiverStereoIr(R, Receiver, PreDelay, St))
	{
		auto Set = std::make_shared<FHybridIrSet>();
		Set->Low = fdtd::partitionIr(St.low.data(), (int)St.low.size(), HybridIrStore::BlockSize);
		for (int32 c = 0; c < 2; ++c)
		{
			Set->Hf[c] = fdtd::partitionIr(St.hf[c].data(), (int)St.hf[c].size(), HybridIrStore::BlockSize);
		}
		Set->NumHf = 2;
		Store.Publish(std::shared_ptr<const FHybridIrSet>(MoveTemp(Set)));
		return;
	}
	
	// sonst mono wie bisher (auch ohne Steam Audio)
	const std::vector<float> Ir = fdtd::receiverIr(R, Receiver, PreDelay);
	Store.Publish(fdtd::partitionIr(Ir.data(), (int)Ir.size(), HybridIrStore::BlockSize));
}

void AHybridAcousticsRoom::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	HybridIrStore::Get().SetGains(DryGain, WetGain);
	HybridIrStore::Get().SetUseHrtf(bUseHrtf);

	if (bDrawDebug)
	{
		const FVector Src = SoundSource ? SoundSource->GetActorLocation() : SourcePoint->GetComponentLocation();
		DrawDebugSphere(GetWorld(), Src, 20.f, 12, FColor::Orange);
	}

	if (!Bake.IsValid()) return;

	APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0);
	if (!Cam) return;

	const fdtd::IrGrid& G = Bake->R.grid;
	const FVector P = WorldToRoomMeters(Cam->GetCameraLocation());
	// Verdeckung des Direktschalls durch die gebackenen Innenwände (wirkt nur auf den Hochton im Dry-Pfad)
	const FVector SrcM = GetSourceMeters();
	const float Vis = fdtd::directVisibility(fdtd::Vec3{ SrcM.X, SrcM.Y, SrcM.Z }, fdtd::Vec3{ P.X, P.Y, P.Z },
	Bake->R.settings.innerWalls, SourceRadiusCm / 100.0, 32);
	const float Transmission = FMath::Pow(10.f, WallTransmissionDb / 20.f);
	HybridIrStore::Get().SetDryOcclusionGain(Vis + (1.f - Vis) * Transmission);
	const int32 Rcv = G.nearest((float)P.X, (float)P.Y, (float)P.Z);

	if (bDrawDebug)
	{
		float X, Y, Z;
		G.position(Rcv, X, Y, Z);
		DrawDebugPoint(GetWorld(), RoomMetersToWorld(FVector(X, Y, Z)), 12.f, FColor::Green);
	}
	if (bDecorrelateReverb != bLastDecorrelate) { bLastDecorrelate = bDecorrelateReverb; CurrentReceiver = INDEX_NONE; }
	if (Rcv != CurrentReceiver)
	{
		CurrentReceiver = Rcv;
		PublishReceiver(Rcv);
	}
}

FBox AHybridAcousticsRoom::ActorBoxMeters(const AActor * A) const
{
	// Alle Komponenten inkl. nicht kollidierender und Kind-Actors
	const FBox WorldBox = A->GetComponentsBoundingBox(true, true);
	if (!WorldBox.IsValid || WorldBox.GetExtent().GetMin() <= KINDA_SMALL_NUMBER && WorldBox.GetExtent().GetMax() <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogTemp, Warning, TEXT("HybridAcoustics: Wand-Actor '%s' (%s) hat keine Ausdehnung -> ignoriert"),
		*A->GetActorNameOrLabel(), *A->GetClass()->GetName());
		return FBox(ForceInit);   // IsValid = false
	}
	const FVector Origin = WorldBox.GetCenter();
	const FVector Extent = WorldBox.GetExtent();
	// alle 8 Ecken in Raummeter umrechnen (Raum darf gedreht sein), dann umhüllende Box
	FBox B(ForceInit);
	for (int32 c = 0; c < 8; ++c)
	{
		const FVector Sign((c & 1) ? 1.0 : -1.0, (c & 2) ? 1.0 : -1.0, (c & 4) ? 1.0 : -1.0);
		B += WorldToRoomMeters(Origin + Extent * Sign);
	}
	 return B;
}

TArray<FBox> AHybridAcousticsRoom::GetInnerWallsMeters() const
{
	TArray<FBox> Walls = InnerWallsM;
	
	TArray<FBox> Live;
	bool bAllValid = true;
	for (int32 i = 0; i < InnerWallActors.Num(); ++i)
	{
		const AActor * A = InnerWallActors[i];
		if (!A)
		{
			bAllValid = false;
			UE_LOG(LogTemp, Log, TEXT("HybridAcoustics: InnerWallActors[%d] existiert in dieser Welt nicht"), i);
			continue;
		}
		const FBox Box = ActorBoxMeters(A);
		if (Box.IsValid) Live.Add(Box);
	}
	
	// Live-Actors bevorzugen; fehlt einer, die im Editor erfassten Boxen (gleiche Werte wie beim Bake Now)
	if (!bAllValid && WallActorBoxesM.Num() == InnerWallActors.Num())
	{
		for (const FBox& Box : WallActorBoxesM) if (Box.IsValid) Walls.Add(Box);
	}
	else
	{
		Walls.Append(Live);
	}
	 return Walls;
}

void AHybridAcousticsRoom::CaptureWalls()
{
	TArray<FBox> Boxes;
	for (const AActor* A : InnerWallActors)
	{
		Boxes.Add(A ? ActorBoxMeters(A) : FBox(ForceInit));
	}
	Modify();                 // Level als geändert markieren -> beim Speichern bleibt die Liste erhalten
	WallActorBoxesM = Boxes;
	UE_LOG(LogTemp, Log, TEXT("HybridAcoustics: %d Wand-Boxen erfasst"), Boxes.Num());
}

void AHybridAcousticsRoom::BakeNow()
{
	CaptureWalls();
	BakeAsync(true);
}
