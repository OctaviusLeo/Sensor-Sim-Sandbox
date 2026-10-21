// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Sensors/LidarPointCloud.h"
#include "Sensors/LidarModel.h"
#include "Sensors/LidarPipelineRunner.h"
#include "Sensors/LidarConfig.h"
#include "Sensors/LidarScanGeometry.h"
#include "Bridge/RosbridgeClient.h"

#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "CoreGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogLidarPublish, Log, All);

namespace
{
	UStaticMesh* LoadCube()
	{
		return LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	}

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

	// Same enclosure LidarSelfCheck builds, so a lidar at the center reliably
	// gets many hits at known distances regardless of the loaded level.
	void BuildEnclosure(UWorld* World, const FVector& Center)
	{
		UStaticMesh* Cube = LoadCube();
		if (!Cube)
		{
			UE_LOG(LogLidarPublish, Warning, TEXT("could not load engine cube; enclosure not built"));
			return;
		}
		const double HalfCm = 1000.0;
		const double WallH = 6.0;
		const double WallT = 0.2;
		const double WallLen = 22.0;
		SpawnBox(World, Center + FVector(+HalfCm, 0, 0), FVector(WallT, WallLen, WallH), Cube);
		SpawnBox(World, Center + FVector(-HalfCm, 0, 0), FVector(WallT, WallLen, WallH), Cube);
		SpawnBox(World, Center + FVector(0, +HalfCm, 0), FVector(WallLen, WallT, WallH), Cube);
		SpawnBox(World, Center + FVector(0, -HalfCm, 0), FVector(WallLen, WallT, WallH), Cube);
		SpawnBox(World, Center + FVector(0, 0, -350), FVector(WallLen, WallLen, WallT), Cube);
		SpawnBox(World, Center + FVector(0, 0, +350), FVector(WallLen, WallLen, WallT), Cube);
		SpawnBox(World, Center + FVector(400, 200, 0), FVector(1.0, 1.0, WallH), Cube);
		SpawnBox(World, Center + FVector(-350, -450, 0), FVector(1.2, 1.2, WallH), Cube);
		SpawnBox(World, Center + FVector(250, -600, 0), FVector(0.8, 0.8, WallH), Cube);
	}

	// Kept alive across the publish grace period so the socket stays open.
	TSharedPtr<FRosbridgeClient> GLidarPublishClient;
}

// ---------------------------------------------------------------------------
// Console command: `LidarPublish [configPath]`
//
// One-command [HOST] demo/verify for task 12.1. Spawns a lidar in an enclosure,
// performs one real raycast scan, builds the sensor_msgs/PointCloud2 from the
// returns (FLidarPointCloud), and publishes it through the live Rosbridge_Client
// to /lidar/points. Logs the built width/height/frame_id so the exact message
// shape is visible in the editor log, then (after a short grace so rosbridge can
// deliver it to `ros2 topic echo`) disconnects and requests a clean engine exit,
// mirroring RosbridgePublish. Needs a world for raycasting, so it takes the
// world-and-args delegate; a `-game` run supplies the live world.
// ---------------------------------------------------------------------------
static void ExecuteLidarPublish(const TArray<FString>& Args, UWorld* World)
{
	FString ConfigPath;
	if (Args.Num() >= 1 && !Args[0].IsEmpty())
	{
		ConfigPath = Args[0];
	}

	// --- Load + validate the Gate A config through the shared contract ------
	const FString ResolvedConfig = ConfigPath.IsEmpty() ? FLidarConfig::ResolveConfigPath() : ConfigPath;
	FLidarConfig Config;
	FString LoadError;
	if (!FLidarConfig::LoadFromFile(ResolvedConfig, Config, LoadError))
	{
		UE_LOG(LogLidarPublish, Error, TEXT("LidarPublish FAIL: could not load %s: %s"), *ResolvedConfig, *LoadError);
		return;
	}
	if (!Config.Validate().bValid)
	{
		UE_LOG(LogLidarPublish, Error, TEXT("LidarPublish FAIL: config %s out of bounds"), *ResolvedConfig);
		return;
	}

	if (World == nullptr)
	{
		UE_LOG(LogLidarPublish, Error, TEXT("LidarPublish FAIL: no World for raycast"));
		return;
	}

	// --- Spawn the lidar in an enclosure far from the level geometry --------
	const FVector SensorCenter(0.0, 0.0, 50000.0);
	BuildEnclosure(World, SensorCenter);

	FActorSpawnParameters LidarParams;
	LidarParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ALidarModel* Lidar = World->SpawnActor<ALidarModel>(ALidarModel::StaticClass(),
		SensorCenter, FRotator::ZeroRotator, LidarParams);
	if (!Lidar)
	{
		UE_LOG(LogLidarPublish, Error, TEXT("LidarPublish FAIL: could not spawn ALidarModel"));
		return;
	}
	Lidar->Initialise(/*RunSeed*/ 1234, Config, TEXT("lidar"));

	// --- One real scan, then build the PointCloud2 from the returns ---------
	const FLidarScan Scan = Lidar->PerformScan(/*CaptureTime*/ 0.0);
	const int32 Returns = Scan.NumReturns();
	const TSharedRef<FJsonObject> Msg = FLidarPointCloud::BuildMessage(Scan);

	int32 Width = 0, Height = 0;
	FString FrameId;
	{
		double W = 0.0, H = 0.0;
		Msg->TryGetNumberField(TEXT("width"), W);
		Msg->TryGetNumberField(TEXT("height"), H);
		Width = static_cast<int32>(W);
		Height = static_cast<int32>(H);
		const TSharedPtr<FJsonObject>* Header = nullptr;
		if (Msg->TryGetObjectField(TEXT("header"), Header) && Header && Header->IsValid())
		{
			(*Header)->TryGetStringField(TEXT("frame_id"), FrameId);
		}
	}
	UE_LOG(LogLidarPublish, Log,
		TEXT("LidarPublish built PointCloud2: frame_id='%s' width=%d height=%d (returns=%d, rays=%d)"),
		*FrameId, Width, Height, Returns, Scan.PublishedRangeM.Num());

	// --- Publish through the live Rosbridge_Client to /lidar/points ---------
	const FRosbridgeConfig BridgeConfig = FRosbridgeConfig::Resolve();
	GLidarPublishClient = MakeShared<FRosbridgeClient>(BridgeConfig);
	GLidarPublishClient->Connect();
	GLidarPublishClient->Advertise(FLidarPointCloud::GetTopic(), FLidarPointCloud::GetMessageType());

	TSharedPtr<FRosbridgeClient> ClientRef = GLidarPublishClient;
	// Publish after a short grace so the socket has time to open; the client
	// re-advertises on connect so this is safe on the first open.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[ClientRef, Msg](float /*Dt*/) -> bool
		{
			const FString Op = ClientRef->Publish(FLidarPointCloud::GetTopic(), Msg);
			UE_LOG(LogLidarPublish, Log, TEXT("LidarPublish sent PointCloud2 (%d chars, connected=%d) to %s"),
				Op.Len(), ClientRef->IsConnected() ? 1 : 0, *FLidarPointCloud::GetTopic());

			// Self-terminate like RosbridgePublish: after a grace to let
			// rosbridge deliver the message to `ros2 topic echo`, disconnect
			// and request a clean exit.
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[ClientRef](float /*Dt2*/) -> bool
				{
					ClientRef->Disconnect();
					GLidarPublishClient.Reset();
					UE_LOG(LogLidarPublish, Log, TEXT("LidarPublish requesting engine exit."));
					RequestEngineExit(TEXT("LidarPublish complete"));
					return false; // one-shot
				}), 3.0f);
			return false; // one-shot
		}), 1.5f);
}

static FAutoConsoleCommandWithWorldAndArgs GLidarPublishCommand(
	TEXT("LidarPublish"),
	TEXT("Build a sensor_msgs/PointCloud2 from one real lidar scan and publish it through the Rosbridge_Client to /lidar/points. Usage: LidarPublish [configPath]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ExecuteLidarPublish));
