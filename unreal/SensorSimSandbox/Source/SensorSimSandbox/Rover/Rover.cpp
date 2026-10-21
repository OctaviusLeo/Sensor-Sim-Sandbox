// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Rover/Rover.h"

#include "Rover/RoverConfig.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/StaticMesh.h"
#include "Components/InputComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogRover, Log, All);

ARover::ARover()
{
	PrimaryActorTick.bCanEverTick = true;

	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BodyMesh"));
	SetRootComponent(BodyMesh);

	// Engine basic shape only (no marketplace content). Resolve the cube mesh.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		BodyMesh->SetStaticMesh(CubeMesh.Object);
	}
	BodyMesh->SetRelativeScale3D(FVector(1.6f, 1.0f, 0.5f));
	BodyMesh->SetSimulatePhysics(false);
	BodyMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

	Movement = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("Movement"));
	Movement->UpdatedComponent = BodyMesh;
	Movement->MaxSpeed = MaxSpeedCmS;
	Movement->Acceleration = 2000.0f;
	Movement->Deceleration = 4000.0f;

	AutoPossessPlayer = EAutoReceiveInput::Player0;
}

void ARover::BeginPlay()
{
	Super::BeginPlay();
	ApplyCommandLineConfig();
}

void ARover::ApplyCommandLineConfig()
{
	const TCHAR* CmdLine = FCommandLine::Get();

	// -RoverMode=Manual|Waypoint
	FString ModeStr;
	if (FParse::Value(CmdLine, TEXT("RoverMode="), ModeStr))
	{
		if (ModeStr.Equals(TEXT("Waypoint"), ESearchCase::IgnoreCase))
		{
			SetMode(ERoverMode::Waypoint);
		}
		else
		{
			SetMode(ERoverMode::Manual);
		}
	}

	// -RoverConfig=<path to level.yaml> selects the route for waypoint mode.
	FString ConfigPath;
	if (FParse::Value(CmdLine, TEXT("RoverConfig="), ConfigPath))
	{
		FRoverLevelConfig LevelConfig;
		FString Error;
		if (FRoverLevelConfig::LoadFromFile(ConfigPath, LevelConfig, Error))
		{
			ConfigureWaypoints(
				LevelConfig.Waypoints.GetWaypointsXYMeters(),
				LevelConfig.Waypoints.ArrivalToleranceM,
				LevelConfig.Waypoints.WaypointTimeoutS);
			UE_LOG(LogRover, Log, TEXT("Rover loaded %d waypoints from %s (tol=%.2f m, timeout=%.1f s)"),
				LevelConfig.Waypoints.RouteCm.Num(), *ConfigPath,
				LevelConfig.Waypoints.ArrivalToleranceM, LevelConfig.Waypoints.WaypointTimeoutS);
		}
		else
		{
			UE_LOG(LogRover, Error, TEXT("Rover failed to load config %s: %s"), *ConfigPath, *Error);
		}
	}

	// -RunDir=<folder> is where this run's artifacts are written; stored for
	// downstream components (logger in task 7). Logged here so an unattended
	// run records the folder it is writing into.
	FString RunDir;
	if (FParse::Value(CmdLine, TEXT("RunDir="), RunDir))
	{
		UE_LOG(LogRover, Log, TEXT("Rover run folder: %s"), *RunDir);
	}
}

void ARover::SetMode(ERoverMode NewMode)
{
	Mode = NewMode;
	// Clear any pending keyboard axis state so a mode switch cannot leak a
	// stale command into waypoint driving.
	ThrottleAxis = 0.0f;
	SteerAxis = 0.0f;
	UE_LOG(LogRover, Log, TEXT("Rover mode -> %s"),
		NewMode == ERoverMode::Waypoint ? TEXT("Waypoint") : TEXT("Manual"));
}

void ARover::ConfigureWaypoints(
	const TArray<TPair<double, double>>& WaypointsXYMeters,
	double ArrivalToleranceM,
	double WaypointTimeoutS)
{
	WaypointMachine.Configure(WaypointsXYMeters, ArrivalToleranceM, WaypointTimeoutS);
	bTimeoutReported = false;
}

void ARover::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	if (PlayerInputComponent == nullptr)
	{
		return;
	}

	// Legacy axis bindings. The matching axis mappings are declared in
	// Config/DefaultInput.ini (W/S -> RoverThrottle, A/D -> RoverSteer) so no
	// Enhanced Input assets (which need the editor UI) are required.
	PlayerInputComponent->BindAxis(TEXT("RoverThrottle"), this, &ARover::OnThrottleInput);
	PlayerInputComponent->BindAxis(TEXT("RoverSteer"), this, &ARover::OnSteerInput);
}

void ARover::OnThrottleInput(float Value)
{
	// Store raw axis; it is only consumed in Manual mode (2.3 ignores keyboard
	// in Waypoint mode).
	ThrottleAxis = FMath::Clamp(Value, -1.0f, 1.0f);
}

void ARover::OnSteerInput(float Value)
{
	SteerAxis = FMath::Clamp(Value, -1.0f, 1.0f);
}

void ARover::ComputeDriveToTarget(double TargetXMeters, double TargetYMeters, float& OutThrottle, float& OutSteer) const
{
	// Rover position in meters (Unreal cm / 100).
	const FVector Loc = GetActorLocation();
	const double RoverX = Loc.X / 100.0;
	const double RoverY = Loc.Y / 100.0;

	const double Dx = TargetXMeters - RoverX;
	const double Dy = TargetYMeters - RoverY;

	// Desired yaw toward the target (Unreal yaw: 0 = +X, increasing toward +Y).
	const double DesiredYawDeg = FMath::RadiansToDegrees(FMath::Atan2(Dy, Dx));
	const double CurrentYawDeg = GetActorRotation().Yaw;
	double YawErr = FMath::FindDeltaAngleDegrees(CurrentYawDeg, DesiredYawDeg);

	// Steer proportionally to the heading error (full steer by +-45 deg off).
	OutSteer = static_cast<float>(FMath::Clamp(YawErr / 45.0, -1.0, 1.0));

	// Throttle: full ahead when roughly facing the target, backing off when the
	// heading error is large so the rover turns before charging off-course.
	const double HeadingFactor = FMath::Clamp(1.0 - FMath::Abs(YawErr) / 90.0, 0.0, 1.0);
	OutThrottle = static_cast<float>(HeadingFactor);
}

void ARover::ApplyDrive(float Throttle, float Steer, float DeltaSeconds)
{
	CommandedThrottle = FMath::Clamp(Throttle, -1.0f, 1.0f);
	CommandedSteer = FMath::Clamp(Steer, -1.0f, 1.0f);

	// Kinematic steering: rotate by yaw rate * steer, then move forward along
	// the facing direction by speed * throttle.
	if (!FMath::IsNearlyZero(CommandedSteer))
	{
		const float YawDelta = CommandedSteer * MaxYawRateDegS * DeltaSeconds;
		AddActorWorldRotation(FRotator(0.0f, YawDelta, 0.0f));
	}

	if (!FMath::IsNearlyZero(CommandedThrottle))
	{
		const FVector Forward = GetActorForwardVector();
		const float SpeedCmS = CommandedThrottle * MaxSpeedCmS;
		if (Movement)
		{
			Movement->AddInputVector(Forward * (SpeedCmS / FMath::Max(1.0f, MaxSpeedCmS)));
		}
		else
		{
			AddActorWorldOffset(Forward * SpeedCmS * DeltaSeconds, /*bSweep*/ true);
		}
	}
}

void ARover::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (Mode == ERoverMode::Waypoint)
	{
		// Waypoint mode: keyboard input is ignored (2.3). Decisions come
		// entirely from the pure state machine.
		const FVector Loc = GetActorLocation();
		const FWaypointStep StepResult = WaypointMachine.Update(
			Loc.X / 100.0, Loc.Y / 100.0, DeltaSeconds);

		switch (StepResult.Action)
		{
		case EWaypointAction::DriveToTarget:
		{
			float Throttle = 0.0f, Steer = 0.0f;
			ComputeDriveToTarget(StepResult.TargetX, StepResult.TargetY, Throttle, Steer);
			ApplyDrive(Throttle, Steer, DeltaSeconds);
			if (StepResult.bAdvancedThisStep)
			{
				UE_LOG(LogRover, Log, TEXT("Rover advanced to waypoint %d"), StepResult.CurrentIndex);
			}
			break;
		}
		case EWaypointAction::HoldAtFinal:
			// Final waypoint reached: hold with zero commanded throttle/steer (2.5).
			ApplyDrive(0.0f, 0.0f, DeltaSeconds);
			if (StepResult.bReachedFinalThisStep)
			{
				UE_LOG(LogRover, Log, TEXT("Rover reached final waypoint %d; holding."), StepResult.CurrentIndex);
			}
			break;
		case EWaypointAction::StoppedTimedOut:
			// Timeout: stop and report the unreached waypoint (2.6).
			ApplyDrive(0.0f, 0.0f, DeltaSeconds);
			if (StepResult.bTimedOutThisStep && !bTimeoutReported)
			{
				bTimeoutReported = true;
				UE_LOG(LogRover, Warning,
					TEXT("Rover TIMED OUT before reaching waypoint %d; stopping and reporting unreached waypoint."),
					StepResult.CurrentIndex);
			}
			break;
		case EWaypointAction::Idle:
		default:
			ApplyDrive(0.0f, 0.0f, DeltaSeconds);
			break;
		}
		return;
	}

	// Manual mode: apply keyboard throttle/steer. The input was captured this
	// frame by the axis handlers, so motion begins the same tick (well within
	// the 100 ms budget of 2.1).
	ApplyDrive(ThrottleAxis, SteerAxis, DeltaSeconds);
}
