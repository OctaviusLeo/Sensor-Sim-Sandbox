// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Rover/WaypointStateMachine.h"
#include "Rover.generated.h"

class UStaticMeshComponent;
class UFloatingPawnMovement;
class UInputComponent;

/** Rover control mode (design "Rover + Waypoint Controller"). */
UENUM(BlueprintType)
enum class ERoverMode : uint8
{
	/** Keyboard throttle/brake/steering drive the rover. */
	Manual,
	/** The scripted waypoint controller drives the rover; keyboard ignored (2.3). */
	Waypoint
};

/**
 * The drivable rover pawn (Requirement 2.1, 2.3-2.6).
 *
 * Manual mode: keyboard throttle/brake/steering (bound through the legacy input
 * axes so no Enhanced Input assets — which cannot be authored without the
 * editor UI — are required). Keyboard motion begins the same tick the input is
 * read, well within the 100 ms budget of 2.1.
 *
 * Waypoint mode: the pawn delegates ALL decisions to FWaypointStateMachine (the
 * pure, engine-independent controller) and ignores keyboard input entirely
 * (2.3). It advances within the arrival tolerance (2.4), holds with zero
 * throttle/steer at the final waypoint (2.5), and on timeout stops and reports
 * the unreached waypoint (2.6).
 *
 * The run selects waypoint mode, the run config, and the run folder from
 * command-line arguments so an unattended run needs no keyboard:
 *   -RoverMode=Waypoint  -RoverConfig=<path/to/level.yaml>  -RunDir=<folder>
 */
UCLASS()
class SENSORSIMSANDBOX_API ARover : public APawn
{
	GENERATED_BODY()

public:
	ARover();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	/** Switch control mode. In Waypoint mode keyboard input is ignored (2.3). */
	UFUNCTION(BlueprintCallable, Category = "Rover")
	void SetMode(ERoverMode NewMode);

	UFUNCTION(BlueprintCallable, Category = "Rover")
	ERoverMode GetMode() const { return Mode; }

	/**
	 * Configure the scripted route the waypoint controller follows. Waypoints
	 * are (x, y) pairs in METERS; tolerance in meters, timeout in seconds.
	 */
	void ConfigureWaypoints(
		const TArray<TPair<double, double>>& WaypointsXYMeters,
		double ArrivalToleranceM,
		double WaypointTimeoutS);

	/** Max forward speed (cm/s) at full throttle. */
	UPROPERTY(EditAnywhere, Category = "Rover")
	float MaxSpeedCmS = 600.0f;

	/** Yaw rate (deg/s) at full steering. */
	UPROPERTY(EditAnywhere, Category = "Rover")
	float MaxYawRateDegS = 90.0f;

	// --- Current commanded values (also used by tests / logging) -----------

	/** Commanded throttle this tick, [-1, 1] (1 = full forward). */
	float GetCommandedThrottle() const { return CommandedThrottle; }
	/** Commanded steering this tick, [-1, 1] (1 = full right). */
	float GetCommandedSteer() const { return CommandedSteer; }

	/** Direct access to the pure controller (for tests and the self-check). */
	FWaypointStateMachine& GetWaypointMachine() { return WaypointMachine; }

	/** True after a waypoint timeout stopped the rover (2.6). */
	bool HasReportedTimeout() const { return bTimeoutReported; }
	/** The unreached waypoint index reported on timeout, or INDEX_NONE. */
	int32 GetUnreachedWaypointIndex() const { return WaypointMachine.GetUnreachedWaypointIndex(); }

	/**
	 * Test-only seam: inject the keyboard throttle/steer axis values that the
	 * input handlers would otherwise set from real key presses. This exists so
	 * the headless automation test for "keyboard ignored in waypoint mode"
	 * (2.3) can drive the exact same code path a key press would, without a
	 * player controller or input device, and then assert that in Waypoint mode
	 * the injected axis has NO effect on the commanded motion. It writes the
	 * same private fields OnThrottleInput/OnSteerInput write (clamped to
	 * [-1, 1]); it does not change any runtime behavior or decision logic.
	 */
	void Test_InjectKeyboardAxis(float InThrottleAxis, float InSteerAxis)
	{
		ThrottleAxis = FMath::Clamp(InThrottleAxis, -1.0f, 1.0f);
		SteerAxis = FMath::Clamp(InSteerAxis, -1.0f, 1.0f);
	}

protected:
	/** Visible body built from an engine basic shape (no marketplace content). */
	UPROPERTY(VisibleAnywhere, Category = "Rover")
	TObjectPtr<UStaticMeshComponent> BodyMesh;

	/** Simple kinematic movement so the pawn drives without a full vehicle sim. */
	UPROPERTY(VisibleAnywhere, Category = "Rover")
	TObjectPtr<UFloatingPawnMovement> Movement;

private:
	ERoverMode Mode = ERoverMode::Manual;

	FWaypointStateMachine WaypointMachine;

	// Keyboard axis state (Manual mode only).
	float ThrottleAxis = 0.0f; // forward/back from keyboard
	float SteerAxis = 0.0f;    // left/right from keyboard

	// The resolved command applied to movement this tick.
	float CommandedThrottle = 0.0f;
	float CommandedSteer = 0.0f;

	bool bTimeoutReported = false;

	// Legacy input axis handlers.
	void OnThrottleInput(float Value);
	void OnSteerInput(float Value);

	// Apply a (throttle, steer) command to the kinematic movement for dt.
	void ApplyDrive(float Throttle, float Steer, float DeltaSeconds);

	// Read -RoverMode / -RoverConfig / -RunDir from the command line and apply.
	void ApplyCommandLineConfig();

	// Steer/throttle toward a world-space (x, y) target in meters.
	void ComputeDriveToTarget(double TargetXMeters, double TargetYMeters, float& OutThrottle, float& OutSteer) const;
};
