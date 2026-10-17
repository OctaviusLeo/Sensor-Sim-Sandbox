// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Rover/RoverLevelBuilder.h"

#include "Rover/Rover.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoverLevel, Log, All);

ARoverLevelBuilder::ARoverLevelBuilder()
{
	// This game mode spawns the rover itself (from config placement), so no
	// default pawn is auto-spawned.
	DefaultPawnClass = nullptr;
}

FString ARoverLevelBuilder::ResolveConfigPath()
{
	FString ConfigPath;
	if (FParse::Value(FCommandLine::Get(), TEXT("RoverConfig="), ConfigPath) && !ConfigPath.IsEmpty())
	{
		return ConfigPath;
	}

	// Default: repo-root config/level.yaml. The uproject lives at
	// <repo>/unreal/SensorSimSandbox/, so the repo root is two levels up from
	// the project directory.
	const FString ProjectDir = FPaths::ProjectDir(); // .../unreal/SensorSimSandbox/
	const FString RepoRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(ProjectDir, TEXT("../../")));
	return FPaths::Combine(RepoRoot, TEXT("config"), TEXT("level.yaml"));
}

void ARoverLevelBuilder::BeginPlay()
{
	Super::BeginPlay();

	const FString ConfigPath = ResolveConfigPath();
	FString Error;
	if (!FRoverLevelConfig::LoadFromFile(ConfigPath, LoadedConfig, Error))
	{
		UE_LOG(LogRoverLevel, Error, TEXT("RoverLevelBuilder could not load %s: %s"), *ConfigPath, *Error);
		return;
	}

	FString ContentReason;
	if (!LoadedConfig.MeetsMinimumContent(ContentReason))
	{
		UE_LOG(LogRoverLevel, Error, TEXT("RoverLevelBuilder config does not meet minimum content: %s"), *ContentReason);
		// Still build what we have so the failure is visible in the scene.
	}

	UE_LOG(LogRoverLevel, Log,
		TEXT("RoverLevelBuilder building from %s: %d obstacles, %d landmarks (%d distinguishable), %d waypoints"),
		*ConfigPath, LoadedConfig.Obstacles.Num(), LoadedConfig.Landmarks.Num(),
		LoadedConfig.CountDistinguishableLandmarks(), LoadedConfig.Waypoints.RouteCm.Num());

	BuildLevel(LoadedConfig);
}

UStaticMesh* ARoverLevelBuilder::LoadShapeMesh(const FString& ShapeName)
{
	FString Asset;
	const FString S = ShapeName.ToLower();
	if (S == TEXT("sphere")) { Asset = TEXT("/Engine/BasicShapes/Sphere.Sphere"); }
	else if (S == TEXT("cylinder")) { Asset = TEXT("/Engine/BasicShapes/Cylinder.Cylinder"); }
	else if (S == TEXT("cone")) { Asset = TEXT("/Engine/BasicShapes/Cone.Cone"); }
	else if (S == TEXT("plane")) { Asset = TEXT("/Engine/BasicShapes/Plane.Plane"); }
	else { Asset = TEXT("/Engine/BasicShapes/Cube.Cube"); }

	return LoadObject<UStaticMesh>(nullptr, *Asset);
}

void ARoverLevelBuilder::BuildLevel(const FRoverLevelConfig& Config)
{
	if (bBuilt)
	{
		return;
	}
	bBuilt = true;

	SpawnGround(Config.Ground);
	SpawnLighting(Config.Lighting);

	for (const FRoverPlacedActor& Obstacle : Config.Obstacles)
	{
		SpawnShapeActor(Obstacle);
	}
	for (const FRoverPlacedActor& Landmark : Config.Landmarks)
	{
		SpawnShapeActor(Landmark);
	}

	SpawnRover();
}

void ARoverLevelBuilder::SpawnGround(const FRoverGround& Ground)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FRoverPlacedActor Spec;
	Spec.Name = TEXT("Ground");
	Spec.Shape = TEXT("Plane");
	Spec.PositionCm = FVector(0.0, 0.0, Ground.ZCm);
	// Engine Plane is 100 cm; scale so the half-extent is SizeM meters.
	const double ScaleXY = (Ground.SizeM * 2.0 * 100.0) / 100.0; // = SizeM*2
	Spec.Scale = FVector(ScaleXY, ScaleXY, 1.0);
	Spec.Color = FLinearColor(0.18f, 0.20f, 0.18f, 1.0f);
	SpawnShapeActor(Spec);
}

void ARoverLevelBuilder::SpawnLighting(const FRoverLighting& Lighting)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Directional light (the "sun").
	const FRotator SunRot(Lighting.DirectionalPitchDeg, Lighting.DirectionalYawDeg, 0.0);
	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(
		ADirectionalLight::StaticClass(), FVector(0, 0, 1000), SunRot, Params);
	if (Sun)
	{
		if (UDirectionalLightComponent* Comp = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
		{
			Comp->SetIntensity(static_cast<float>(Lighting.DirectionalIntensityLux));
			Comp->SetMobility(EComponentMobility::Movable);
		}
	}

	// Sky light so shadowed areas and camera frames are not black.
	ASkyLight* Sky = World->SpawnActor<ASkyLight>(
		ASkyLight::StaticClass(), FVector(0, 0, 1000), FRotator::ZeroRotator, Params);
	if (Sky)
	{
		if (USkyLightComponent* Comp = Sky->GetLightComponent())
		{
			Comp->SetMobility(EComponentMobility::Movable);
			Comp->SetIntensity(static_cast<float>(Lighting.SkyIntensity));
			Comp->SetLowerHemisphereColor(FLinearColor(0.1f, 0.1f, 0.12f, 1.0f));
			Comp->RecaptureSky();
		}
	}

	UE_LOG(LogRoverLevel, Log, TEXT("Spawned directional + sky lights."));
}

AStaticMeshActor* ARoverLevelBuilder::SpawnShapeActor(const FRoverPlacedActor& Spec)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	UStaticMesh* Mesh = LoadShapeMesh(Spec.Shape);
	if (!Mesh)
	{
		UE_LOG(LogRoverLevel, Warning, TEXT("Could not load engine shape '%s' for %s"), *Spec.Shape, *Spec.Name);
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), Spec.PositionCm, FRotator::ZeroRotator, Params);
	if (!Actor)
	{
		return nullptr;
	}
#if WITH_EDITOR
	Actor->SetActorLabel(Spec.Name);
#endif

	UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent();
	if (Comp)
	{
		Comp->SetMobility(EComponentMobility::Movable);
		Comp->SetStaticMesh(Mesh);
		Comp->SetWorldScale3D(Spec.Scale);

		// Tint via a dynamic material instance over the engine's basic material.
		UMaterialInterface* Base = Comp->GetMaterial(0);
		if (Base)
		{
			UMaterialInstanceDynamic* MID = Comp->CreateDynamicMaterialInstance(0, Base);
			if (MID)
			{
				// BasicShapeMaterial exposes a "Color" vector parameter.
				MID->SetVectorParameterValue(TEXT("Color"), Spec.Color);
			}
		}
	}

	return Actor;
}

ARover* ARoverLevelBuilder::SpawnRover()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Start the rover slightly above the ground at the origin.
	const FVector Start(0.0f, 0.0f, 60.0f);
	ARover* Rover = World->SpawnActor<ARover>(ARover::StaticClass(), Start, FRotator::ZeroRotator, Params);
	if (!Rover)
	{
		UE_LOG(LogRoverLevel, Error, TEXT("Failed to spawn ARover."));
		return nullptr;
	}

	// Hand the rover the parsed waypoint route and whichever mode the command
	// line selected (ARover::BeginPlay also reads the command line, but we set
	// the route from the config this builder already parsed to avoid a second
	// file read).
	Rover->ConfigureWaypoints(
		LoadedConfig.Waypoints.GetWaypointsXYMeters(),
		LoadedConfig.Waypoints.ArrivalToleranceM,
		LoadedConfig.Waypoints.WaypointTimeoutS);

	UE_LOG(LogRoverLevel, Log, TEXT("Spawned ARover at origin with %d waypoints."),
		LoadedConfig.Waypoints.RouteCm.Num());
	return Rover;
}
