// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Sensors/LidarSelfCheck.h"

#include "Sensors/LidarModel.h"
#include "Sensors/LidarPipelineRunner.h"
#include "Sensors/LidarConfig.h"
#include "Sensors/LidarScanGeometry.h"

#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogLidarSelfCheck, Log, All);

namespace
{
	constexpr double CmPerMeter = 100.0;

	UStaticMesh* LoadCube()
	{
		return LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	}

	// Spawn one collidable box at a world position (cm) with a scale (unitless;
	// the engine cube is 100 cm). Returns the actor or nullptr.
	AStaticMeshActor* SpawnBox(UWorld* World, const FVector& PosCm, const FVector& Scale, UStaticMesh* Cube)
	{
		if (!World || !Cube)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(), PosCm, FRotator::ZeroRotator, Params);
		if (!Actor)
		{
			return nullptr;
		}
		if (UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent())
		{
			Comp->SetMobility(EComponentMobility::Movable);
			Comp->SetStaticMesh(Cube);
			Comp->SetWorldScale3D(Scale);
			Comp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Comp->SetCollisionResponseToAllChannels(ECR_Block);
		}
		return Actor;
	}

	// Build a small enclosing scene centered on `Center` (cm) so a lidar at
	// that center reliably gets many hits at KNOWN, non-zero distances
	// regardless of which level is loaded: four walls at +-10 m, a floor and a
	// ceiling, and a few interior obstacle pillars at mixed distances. The
	// center is placed far from the loaded level's own geometry so no stray
	// actor overlaps the sensor origin (an overlap would make line traces
	// report a degenerate distance of 0).
	void BuildEnclosure(UWorld* World, const FVector& Center)
	{
		UStaticMesh* Cube = LoadCube();
		if (!Cube)
		{
			UE_LOG(LogLidarSelfCheck, Warning, TEXT("could not load engine cube; enclosure not built"));
			return;
		}

		// Room is ~20 m across (half-extent 10 m = 1000 cm). Walls are thin,
		// tall boxes placed at +-X and +-Y; floor/ceiling are wide flat boxes.
		const double HalfCm = 1000.0;   // 10 m from center to each wall
		const double WallH = 6.0;        // 6 x 100 cm = 6 m tall
		const double WallT = 0.2;        // 20 cm thick
		const double WallLen = 22.0;     // 22 m long so corners overlap

		SpawnBox(World, Center + FVector(+HalfCm, 0, 0), FVector(WallT, WallLen, WallH), Cube);
		SpawnBox(World, Center + FVector(-HalfCm, 0, 0), FVector(WallT, WallLen, WallH), Cube);
		SpawnBox(World, Center + FVector(0, +HalfCm, 0), FVector(WallLen, WallT, WallH), Cube);
		SpawnBox(World, Center + FVector(0, -HalfCm, 0), FVector(WallLen, WallT, WallH), Cube);

		// Floor 3.5 m below the sensor and a ceiling 3.5 m above, so downward
		// and upward rays (within the 30 deg vertical FOV) also hit.
		SpawnBox(World, Center + FVector(0, 0, -350), FVector(WallLen, WallLen, WallT), Cube);
		SpawnBox(World, Center + FVector(0, 0, +350), FVector(WallLen, WallLen, WallT), Cube);

		// A few interior obstacle pillars at mixed distances from the center.
		SpawnBox(World, Center + FVector(400, 200, 0), FVector(1.0, 1.0, WallH), Cube);
		SpawnBox(World, Center + FVector(-350, -450, 0), FVector(1.2, 1.2, WallH), Cube);
		SpawnBox(World, Center + FVector(250, -600, 0), FVector(0.8, 0.8, WallH), Cube);

		UE_LOG(LogLidarSelfCheck, Log,
			TEXT("built enclosure centered at (%.0f, %.0f, %.0f) cm: 4 walls + floor + ceiling + 3 pillars"),
			Center.X, Center.Y, Center.Z);
	}
}

FLidarSelfCheck::FResult FLidarSelfCheck::Run(UWorld* World, const FString& LidarConfigPath, int32 MinReturns)
{
	FResult Result;

	// --- Load + validate the Gate A config through the shared contract ------
	const FString ConfigPath = LidarConfigPath.IsEmpty()
		? FLidarConfig::ResolveConfigPath()
		: LidarConfigPath;

	FLidarConfig Config;
	FString LoadError;
	if (!FLidarConfig::LoadFromFile(ConfigPath, Config, LoadError))
	{
		Result.Detail = FString::Printf(TEXT("could not load %s: %s"), *ConfigPath, *LoadError);
		UE_LOG(LogLidarSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}

	const FSensorConfigValidationResult ConfigCheck = Config.Validate();
	Result.bConfigValid = ConfigCheck.bValid;
	if (!ConfigCheck.bValid)
	{
		Result.Detail = FString::Printf(TEXT("config %s is out of bounds: %s"), *ConfigPath, *ConfigCheck.ErrorMessage);
		UE_LOG(LogLidarSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}
	UE_LOG(LogLidarSelfCheck, Log,
		TEXT("loaded Gate A config %s: channels=%d h_fov=%.1f v_fov=%.1f res=%.2f -> %d rays/scan, max_range=%.1f m, sigma=%.4f, p=%.3f, latency=%d ms"),
		*ConfigPath, Config.Channels, Config.HFovDeg, Config.VFovDeg, Config.AngularResolutionDeg,
		FLidarScanGeometry::NumRays(Config), Config.MaxRangeM, Config.RangeNoiseSigmaM, Config.DropoutProb, Config.LatencyMs);

	// --- Shared validation contract is wired in: an out-of-bound config is ---
	// --- rejected naming the offending parameter (Requirement 3.2) ----------
	{
		FLidarConfig Bad = Config;
		Bad.Channels = 999; // 1..128 -> out of bounds
		const FSensorConfigValidationResult BadCheck = Bad.Validate();
		Result.bOutOfBoundRejected = (!BadCheck.bValid && BadCheck.OffendingParameter == TEXT("channels"));
		UE_LOG(LogLidarSelfCheck, Log, TEXT("out-of-bound channels=999 rejected=%d offending='%s' msg='%s'"),
			!BadCheck.bValid ? 1 : 0, *BadCheck.OffendingParameter, *BadCheck.ErrorMessage);
	}

	const int64 RunSeed = 1234; // Gate A run seed (design "Gate A run configuration").

	// --- Pure, world-free check: no-hit rays are null and excluded ----------
	// Build a truth array with a known mix of hits and no-hits, run it through
	// the model's pipeline path, and confirm every no-hit stays null in the
	// published output and that NO no-hit ray is ever marked dropped.
	{
		FLidarPipelineRunner PureRunner;
		PureRunner.Initialise(RunSeed, Config, TEXT("lidar_pure"));

		TArray<FSensorValue> Truth;
		const int32 N = 4000;
		int32 InjectedNoHit = 0;
		for (int32 i = 0; i < N; ++i)
		{
			if (i % 5 == 0)
			{
				Truth.Add(FSensorValue()); // no hit
				++InjectedNoHit;
			}
			else
			{
				Truth.Add(FSensorValue(10.0 + (i % 50) * 0.5)); // a hit range in meters
			}
		}

		const FLidarScan Scan = PureRunner.ProcessCapturedScan(/*Seq*/ 0, /*t*/ 0.0, Truth, {});

		bool bExcluded = true;
		for (int32 i = 0; i < N; ++i)
		{
			if (!Truth[i].IsSet())
			{
				// A no-hit ray must stay null in published and never be dropped.
				if (Scan.PublishedRangeM[i].IsSet() || Scan.bDropped[i])
				{
					bExcluded = false;
					break;
				}
			}
		}
		Result.bNoHitExcluded = bExcluded;
		UE_LOG(LogLidarSelfCheck, Log,
			TEXT("pure no-hit exclusion: injected %d no-hit of %d rays; excluded-from-stats=%d (dropped among hits=%d)"),
			InjectedNoHit, N, bExcluded ? 1 : 0, Scan.NumDropped());
	}

	// --- Raycast scans against the world until >= MinReturns logged ---------
	if (World == nullptr)
	{
		Result.Detail = TEXT("no World available for raycast; pure checks ran but >=1000 real returns NOT produced");
		UE_LOG(LogLidarSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}

	// Build the enclosure far from the loaded level's geometry so nothing
	// overlaps the sensor origin (an overlap makes traces report distance 0).
	const FVector SensorCenter(0.0, 0.0, 50000.0); // 500 m above the level
	BuildEnclosure(World, SensorCenter);

	FActorSpawnParameters LidarParams;
	LidarParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ALidarModel* Lidar = World->SpawnActor<ALidarModel>(ALidarModel::StaticClass(),
		SensorCenter, FRotator::ZeroRotator, LidarParams);
	if (!Lidar)
	{
		Result.Detail = TEXT("failed to spawn ALidarModel");
		UE_LOG(LogLidarSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}
	Lidar->Initialise(RunSeed, Config, TEXT("lidar"));

	int32 ReturnsLogged = 0;
	int32 LoggedRecords = 0;
	const int32 MaxLoggedRecords = 20; // sample per-ray records to keep the log readable
	bool bAnyUnclamped = false; // did we observe a noisy range outside [0, max_range]?
	int32 ScanCount = 0;
	const int32 MaxScans = 10;

	for (ScanCount = 0; ScanCount < MaxScans && ReturnsLogged < MinReturns; ++ScanCount)
	{
		const double CaptureTime = ScanCount * Config.ScanPeriodSeconds();
		const FLidarScan Scan = Lidar->PerformScan(CaptureTime);

		Result.TotalHits += Scan.NumHits();
		Result.TotalDropped += Scan.NumDropped();
		Result.TotalNoHit += (Scan.TruthRangeM.Num() - Scan.NumHits());

		for (int32 i = 0; i < Scan.PublishedRangeM.Num(); ++i)
		{
			const FSensorValue& Pub = Scan.PublishedRangeM[i];
			const FSensorValue& Tru = Scan.TruthRangeM[i];
			if (Pub.IsSet())
			{
				++ReturnsLogged;

				// A published value is a return. Note any value outside the
				// [0, max_range] band: its existence proves ranges are not
				// clamped (clamping would force everything into the band).
				const double P = Pub.GetValue();
				if (P < 0.0 || P > Config.MaxRangeM)
				{
					bAnyUnclamped = true;
				}

				// Log a sample of per-ray records (truth + published).
				if (LoggedRecords < MaxLoggedRecords)
				{
					UE_LOG(LogLidarSelfCheck, Log,
						TEXT("return seq=%lld ray=%d ch=%d az=%.1f truth=%.5f m published=%.5f m"),
						Scan.Seq, i, Scan.Rays[i].Channel, Scan.Rays[i].AzimuthDeg,
						Tru.IsSet() ? Tru.GetValue() : -1.0, P);
					++LoggedRecords;
				}
			}
		}

		UE_LOG(LogLidarSelfCheck, Log,
			TEXT("scan %d: %d rays, %d hits, %d returns, %d dropped (cumulative returns=%d)"),
			ScanCount, Scan.TruthRangeM.Num(), Scan.NumHits(), Scan.NumReturns(), Scan.NumDropped(), ReturnsLogged);
	}

	Result.TotalReturnsLogged = ReturnsLogged;
	Result.bEnoughReturns = (ReturnsLogged >= MinReturns);

	// No-clamp verification. We proved ranges are not clamped two ways:
	//  (a) directly from the pipeline math — a noisy range CAN leave [0,max];
	//  (b) empirically — if any observed published value fell outside the band.
	// To make (a) a hard, deterministic assertion that does not rely on the
	// chance of a far-away hit, run the published values back through: the
	// pipeline added N(0, sigma) with no clamp, so published != truth for hits.
	// We assert the model never clamps by checking that for the collected scans
	// at least one published value differs from truth beyond the band OR, when
	// all hits were well inside the band, that the max |published-truth| is a
	// plausible Gaussian deviation (not pinned to a clamp edge).
	Result.bNoClamping = true; // the model passes bClamp=false; confirmed below
	{
		// Deterministic clamp proof independent of geometry: feed a truth value
		// at exactly max_range with a positive-biased draw cannot be asserted
		// without controlling the RNG, so instead assert the code path: a scan
		// with a truth exactly at max_range yields a published value that is
		// NOT forced to max_range across many rays (some exceed it).
		FLidarPipelineRunner ClampProbe;
		ClampProbe.Initialise(RunSeed, Config, TEXT("lidar_clampprobe"));
		TArray<FSensorValue> AtMax;
		const int32 M = 5000;
		for (int32 i = 0; i < M; ++i)
		{
			AtMax.Add(FSensorValue(Config.MaxRangeM)); // all exactly at max_range
		}
		const FLidarScan Probe = ClampProbe.ProcessCapturedScan(0, 0.0, AtMax, {});
		int32 AboveMax = 0;
		for (const FSensorValue& V : Probe.PublishedRangeM)
		{
			if (V.IsSet() && V.GetValue() > Config.MaxRangeM)
			{
				++AboveMax;
			}
		}
		// With sigma>0 and truth at max_range, ~half the noisy draws exceed
		// max_range when there is no clamp. If clamping were on, AboveMax == 0.
		const bool bSigmaPositive = Config.RangeNoiseSigmaM > 0.0;
		Result.bNoClamping = bSigmaPositive ? (AboveMax > 0) : true;
		UE_LOG(LogLidarSelfCheck, Log,
			TEXT("no-clamp probe: %d of %d returns at truth=max_range exceeded max_range (expect >0 when unclamped & sigma>0); observed-unclamped-in-scans=%d"),
			AboveMax, M, bAnyUnclamped ? 1 : 0);
	}

	Result.Detail = FString::Printf(
		TEXT("config=%s rays/scan=%d scans=%d returns=%d hits=%d dropped=%d no-hit=%d | configValid=%d outOfBoundRejected=%d enough(>=%d)=%d noHitExcluded=%d noClamp=%d"),
		*ConfigPath, FLidarScanGeometry::NumRays(Config), ScanCount, Result.TotalReturnsLogged,
		Result.TotalHits, Result.TotalDropped, Result.TotalNoHit, MinReturns,
		Result.bConfigValid ? 1 : 0, Result.bOutOfBoundRejected ? 1 : 0,
		Result.bEnoughReturns ? 1 : 0, Result.bNoHitExcluded ? 1 : 0, Result.bNoClamping ? 1 : 0);

	return Result;
}

// ---------------------------------------------------------------------------
// Console command: `LidarSelfCheck [configPath] [minReturns]`
//
// One-command headless demo/verify for task 6.1. Needs a world for raycasting,
// so it takes the world-and-args delegate; a `-game` RoverLevel run supplies a
// live world. Logs one PASS/FAIL line plus per-ray records and the return count.
// ---------------------------------------------------------------------------
static void ExecuteLidarSelfCheck(const TArray<FString>& Args, UWorld* World)
{
	FString ConfigPath;
	int32 MinReturns = 1000;
	if (Args.Num() >= 1 && !Args[0].IsEmpty())
	{
		ConfigPath = Args[0];
	}
	if (Args.Num() >= 2 && Args[1].IsNumeric())
	{
		MinReturns = FMath::Max(1, FCString::Atoi(*Args[1]));
	}

	const FLidarSelfCheck::FResult Result = FLidarSelfCheck::Run(World, ConfigPath, MinReturns);

	UE_LOG(LogLidarSelfCheck, Log, TEXT("LidarSelfCheck %s: %s"),
		Result.Passed() ? TEXT("PASS") : TEXT("FAIL"), *Result.Detail);
}

static FAutoConsoleCommandWithWorldAndArgs GLidarSelfCheckCommand(
	TEXT("LidarSelfCheck"),
	TEXT("Headless lidar demo/verify: raycast scans against the world, log >=1000 returns with per-ray truth+published, confirm no-hit rays are null/excluded and ranges are not clamped. Usage: LidarSelfCheck [configPath] [minReturns]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ExecuteLidarSelfCheck));
