// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Sensors/LidarModel.h"

#include "Engine/World.h"
#include "Components/SceneComponent.h"
#include "CollisionQueryParams.h"

DEFINE_LOG_CATEGORY_STATIC(LogLidarModel, Log, All);

namespace
{
	// Unreal works in centimeters; the config and all logged ranges are meters.
	constexpr double CmPerMeter = 100.0;
}

ALidarModel::ALidarModel()
{
	PrimaryActorTick.bCanEverTick = false;

	// An actor needs a root scene component for its transform (and the spawn
	// location) to be honored; without one GetActorLocation stays at the origin
	// and every line trace would start from (0,0,0). The lidar casts from this
	// component's world location.
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("LidarRoot"));
}

FSensorConfigValidationResult ALidarModel::Initialise(int64 InRunSeed, const FLidarConfig& Config, const FString& InSensorName)
{
	NextSeq = 0;
	const FSensorConfigValidationResult Result = Runner.Initialise(InRunSeed, Config, InSensorName);
	if (Result.bValid)
	{
		UE_LOG(LogLidarModel, Log,
			TEXT("Lidar actor ready: %d rays/scan, max_range=%.2f m, rate=%.2f Hz."),
			FLidarScanGeometry::NumRays(Runner.GetConfig()), Runner.GetConfig().MaxRangeM, Runner.GetConfig().RateHz);
	}
	return Result;
}

FSensorConfigValidationResult ALidarModel::ApplyConfig(const FLidarConfig& Candidate)
{
	return Runner.ApplyConfig(Candidate);
}

void ALidarModel::CaptureTruthRanges(const TArray<FLidarRayDir>& Rays, TArray<FSensorValue>& OutTruth) const
{
	OutTruth.Reset();
	OutTruth.SetNum(Rays.Num());

	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		UE_LOG(LogLidarModel, Warning, TEXT("Lidar CaptureTruthRanges called with no World; all rays null."));
		return;
	}

	const FTransform SensorXform = GetActorTransform();
	const FVector Origin = SensorXform.GetLocation();
	const double MaxRangeCm = Runner.GetConfig().MaxRangeM * CmPerMeter;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(LidarScan), /*bTraceComplex*/ false);
	Params.AddIgnoredActor(this);
	if (AActor* Mount = GetAttachParentActor())
	{
		// Do not let the platform the sensor is mounted on (the rover) occlude it.
		Params.AddIgnoredActor(Mount);
	}

	for (int32 i = 0; i < Rays.Num(); ++i)
	{
		const FVector WorldDir = SensorXform.TransformVectorNoScale(Rays[i].Direction).GetSafeNormal();
		const FVector End = Origin + WorldDir * MaxRangeCm;

		FHitResult Hit;
		const bool bHit = World->LineTraceSingleByChannel(Hit, Origin, End, ECC_Visibility, Params);
		if (bHit && Hit.Distance <= MaxRangeCm)
		{
			// Pristine Ground_Truth range in meters, captured BEFORE the pipeline.
			OutTruth[i] = FSensorValue(Hit.Distance / CmPerMeter);
		}
		else
		{
			// No hit within max_range: logged as null, excluded from stats.
			OutTruth[i] = FSensorValue();
		}
	}
}

FLidarScan ALidarModel::PerformScan(double CaptureTime)
{
	const FLidarConfig& Config = Runner.GetConfig();
	TArray<FLidarRayDir> Rays = FLidarScanGeometry::GenerateRays(Config);

	// --- 1. Capture Ground_Truth BEFORE the pipeline runs ------------------
	TArray<FSensorValue> Truth;
	CaptureTruthRanges(Rays, Truth);

	// --- 2+3. Seeded noise then per-ray dropout (no clamp), via the runner --
	return Runner.ProcessCapturedScan(NextSeq++, CaptureTime, Truth, Rays);
}
