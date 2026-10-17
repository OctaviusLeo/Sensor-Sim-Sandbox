// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Rover/RoverConfig.h"
#include "RoverLevelBuilder.generated.h"

class ARover;
class AStaticMeshActor;

/**
 * Game mode that builds `/Game/Maps/RoverLevel` programmatically at BeginPlay
 * from `config/level.yaml`, WITHOUT the editor UI (design "Level content",
 * Requirement 2.2). This is the "spawn the actors from C++ at BeginPlay on a
 * minimal saved map" path the design permits: RoverLevel.umap can be an empty
 * map whose World Settings set this game mode, so launching
 * `UnrealEditor.exe <uproject> /Game/Maps/RoverLevel -game` yields the full
 * scene with no marketplace content and no editor interaction.
 *
 * Everything is built from engine basic shapes (`/Engine/BasicShapes/`) and
 * engine materials only:
 *   - a ground plane (scaled Plane),
 *   - a directional light and a sky light (so camera frames are not black),
 *   - >= 3 static obstacles and >= 3 distinguishable landmarks placed from the
 *     config, each tinted via a dynamic material instance,
 *   - the ARover pawn, which reads -RoverMode / -RoverConfig / -RunDir to run
 *     unattended.
 *
 * The config path defaults to `<project>/../../config/level.yaml` (repo
 * `config/level.yaml`) and can be overridden with `-RoverConfig=<path>`.
 */
UCLASS()
class SENSORSIMSANDBOX_API ARoverLevelBuilder : public AGameModeBase
{
	GENERATED_BODY()

public:
	ARoverLevelBuilder();

	virtual void BeginPlay() override;

	/** Resolve the level-config path: -RoverConfig=, else the repo default. */
	static FString ResolveConfigPath();

	/** The config this builder loaded (empty until BeginPlay). */
	const FRoverLevelConfig& GetLoadedConfig() const { return LoadedConfig; }

protected:
	/** Build the scene from the given parsed config. Safe to call once. */
	void BuildLevel(const FRoverLevelConfig& Config);

private:
	FRoverLevelConfig LoadedConfig;
	bool bBuilt = false;

	void SpawnGround(const FRoverGround& Ground);
	void SpawnLighting(const FRoverLighting& Lighting);
	AStaticMeshActor* SpawnShapeActor(const FRoverPlacedActor& Spec);
	ARover* SpawnRover();

	/** Load the engine basic-shape mesh for a shape name (Cube/Sphere/...). */
	static class UStaticMesh* LoadShapeMesh(const FString& ShapeName);
};
