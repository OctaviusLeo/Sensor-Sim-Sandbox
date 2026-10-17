// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Sensors/CameraMessage.h"
#include "Sensors/CameraModel.h"
#include "Sensors/CameraPipelineRunner.h"
#include "Sensors/CameraConfig.h"
#include "Bridge/RosbridgeClient.h"

#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "CoreGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogCameraPublish, Log, All);

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
		}
		return Actor;
	}

	// A lit box of geometry in front of the camera so the rendered frame is not
	// black (the level may be empty when this runs in isolation). A directional
	// light plus a few boxes gives the camera something to see.
	void BuildScene(UWorld* World, const FVector& CameraPos)
	{
		UStaticMesh* Cube = LoadCube();
		if (!Cube)
		{
			UE_LOG(LogCameraPublish, Warning, TEXT("could not load engine cube; scene not built"));
		}
		else
		{
			// Boxes ahead of the camera (camera looks down +X by default).
			SpawnBox(World, CameraPos + FVector(600, 0, 0), FVector(3, 6, 4), Cube);
			SpawnBox(World, CameraPos + FVector(500, 250, -50), FVector(2, 2, 2), Cube);
			SpawnBox(World, CameraPos + FVector(500, -250, -50), FVector(2, 2, 2), Cube);
			SpawnBox(World, CameraPos + FVector(450, 0, 150), FVector(1, 4, 1), Cube);
		}

		// A directional light so the boxes are lit (frames not black).
		FActorSpawnParameters LightParams;
		LightParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ADirectionalLight* Light = World->SpawnActor<ADirectionalLight>(
			ADirectionalLight::StaticClass(), CameraPos + FVector(0, 0, 500),
			FRotator(-45.0f, 0.0f, 0.0f), LightParams);
		(void)Light;
	}

	// Kept alive across the publish grace period so the socket stays open.
	TSharedPtr<FRosbridgeClient> GCameraPublishClient;
}

// ---------------------------------------------------------------------------
// Console command: `CameraPublish [configPath]`
//
// One-command [HOST] demo/verify for task 14.1. Spawns a lit scene + a camera,
// captures one real frame, encodes the published RGBA to JPEG, builds the
// sensor_msgs/CompressedImage and the accompanying sensor_msgs/CameraInfo with
// the SAME stamp, and publishes both through the live Rosbridge_Client to
// /camera/image/compressed and /camera/camera_info. Logs the built format /
// jpeg byte count / CameraInfo width,height,fx,fy,cx,cy so the exact shapes are
// visible in the editor log, then (after a short grace so rosbridge can deliver
// them to `ros2 topic echo`) disconnects and requests a clean engine exit,
// mirroring LidarPublish. Needs a world + GPU RHI for the render, so it takes
// the world-and-args delegate; a `-game` run supplies the live world.
// ---------------------------------------------------------------------------
static void ExecuteCameraPublish(const TArray<FString>& Args, UWorld* World)
{
	FString ConfigPath;
	if (Args.Num() >= 1 && !Args[0].IsEmpty())
	{
		ConfigPath = Args[0];
	}

	// --- Load + validate the Gate A config through the shared contract ------
	const FString ResolvedConfig = ConfigPath.IsEmpty() ? FCameraConfig::ResolveConfigPath() : ConfigPath;
	FCameraConfig Config;
	FString LoadError;
	if (!FCameraConfig::LoadFromFile(ResolvedConfig, Config, LoadError))
	{
		UE_LOG(LogCameraPublish, Error, TEXT("CameraPublish FAIL: could not load %s: %s"), *ResolvedConfig, *LoadError);
		return;
	}
	if (!Config.Validate().bValid)
	{
		UE_LOG(LogCameraPublish, Error, TEXT("CameraPublish FAIL: config %s out of bounds"), *ResolvedConfig);
		return;
	}

	if (World == nullptr)
	{
		UE_LOG(LogCameraPublish, Error, TEXT("CameraPublish FAIL: no World for render"));
		return;
	}

	// --- Spawn a lit scene + the camera, far from the level geometry --------
	const FVector CameraPos(0.0, 0.0, 50000.0);
	BuildScene(World, CameraPos);

	FActorSpawnParameters CamParams;
	CamParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ACameraModel* Camera = World->SpawnActor<ACameraModel>(ACameraModel::StaticClass(),
		CameraPos, FRotator::ZeroRotator, CamParams);
	if (!Camera)
	{
		UE_LOG(LogCameraPublish, Error, TEXT("CameraPublish FAIL: could not spawn ACameraModel"));
		return;
	}
	Camera->Initialise(/*RunSeed*/ 1234, Config, TEXT("camera"));

	// --- One real capture, then JPEG-encode the published frame -------------
	// Retry a few captures: the first frame can be dropped by the pipeline, and
	// a dropped frame has no published pixels to encode.
	FCameraFrame Frame;
	bool bHavePublished = false;
	for (int32 Attempt = 0; Attempt < 8 && !bHavePublished; ++Attempt)
	{
		Frame = Camera->CaptureFrame(/*CaptureTime*/ static_cast<double>(Attempt) * 0.1);
		bHavePublished = Frame.HasPublished();
	}
	if (!bHavePublished)
	{
		UE_LOG(LogCameraPublish, Error,
			TEXT("CameraPublish FAIL: no published frame after retries (no RHI / all dropped)."));
		return;
	}

	const FCameraFrameBuffer& Pub = Frame.Published;
	TArray<uint8> Jpeg;
	if (!FCameraMessage::EncodeRgbaToJpeg(Pub.Width, Pub.Height, Pub.Pixels, Jpeg))
	{
		UE_LOG(LogCameraPublish, Error,
			TEXT("CameraPublish FAIL: JPEG encode failed for %dx%d (%d px)."),
			Pub.Width, Pub.Height, Pub.Pixels.Num());
		return;
	}

	// Same stamp for the image and the camera_info (published together).
	const double Stamp = Frame.CaptureTime;
	const int32 CfgW = Config.Width;
	const int32 CfgH = Config.Height;

	const FCameraMessage::FIntrinsics Intr = FCameraMessage::ComputeIntrinsics(CfgW, CfgH);
	TSharedRef<FJsonObject> ImageMsg = FCameraMessage::BuildCompressedImageMessage(Jpeg, Stamp);
	TSharedRef<FJsonObject> InfoMsg = FCameraMessage::BuildCameraInfoMessage(CfgW, CfgH, Stamp);

	UE_LOG(LogCameraPublish, Log,
		TEXT("CameraPublish built CompressedImage: format='jpeg' jpeg_bytes=%d frame=%dx%d; CameraInfo: w=%d h=%d fx=%.3f fy=%.3f cx=%.3f cy=%.3f (hfov=%.1f deg)"),
		Jpeg.Num(), Pub.Width, Pub.Height, CfgW, CfgH, Intr.Fx, Intr.Fy, Intr.Cx, Intr.Cy, FCameraMessage::HorizontalFovDeg);

	// --- Publish both through the live Rosbridge_Client ---------------------
	const FRosbridgeConfig BridgeConfig = FRosbridgeConfig::Resolve();
	GCameraPublishClient = MakeShared<FRosbridgeClient>(BridgeConfig);
	GCameraPublishClient->Connect();
	GCameraPublishClient->Advertise(FCameraMessage::GetImageTopic(), FCameraMessage::GetImageMessageType());
	GCameraPublishClient->Advertise(FCameraMessage::GetCameraInfoTopic(), FCameraMessage::GetCameraInfoMessageType());

	TSharedPtr<FRosbridgeClient> ClientRef = GCameraPublishClient;
	// Publish after a short grace so the socket has time to open; the client
	// re-advertises on connect so this is safe on the first open.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[ClientRef, ImageMsg, InfoMsg](float /*Dt*/) -> bool
		{
			const FString InfoOp = ClientRef->Publish(FCameraMessage::GetCameraInfoTopic(), InfoMsg);
			const FString ImgOp = ClientRef->Publish(FCameraMessage::GetImageTopic(), ImageMsg);
			UE_LOG(LogCameraPublish, Log,
				TEXT("CameraPublish sent CameraInfo (%d chars) and CompressedImage (%d chars, connected=%d) to %s + %s"),
				InfoOp.Len(), ImgOp.Len(), ClientRef->IsConnected() ? 1 : 0,
				*FCameraMessage::GetCameraInfoTopic(), *FCameraMessage::GetImageTopic());

			// Self-terminate like LidarPublish: after a grace to let rosbridge
			// deliver the messages to `ros2 topic echo`, disconnect and request
			// a clean exit.
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[ClientRef](float /*Dt2*/) -> bool
				{
					ClientRef->Disconnect();
					GCameraPublishClient.Reset();
					UE_LOG(LogCameraPublish, Log, TEXT("CameraPublish requesting engine exit."));
					RequestEngineExit(TEXT("CameraPublish complete"));
					return false; // one-shot
				}), 3.0f);
			return false; // one-shot
		}), 1.5f);
}

static FAutoConsoleCommandWithWorldAndArgs GCameraPublishCommand(
	TEXT("CameraPublish"),
	TEXT("Build a sensor_msgs/CompressedImage (JPEG) + sensor_msgs/CameraInfo from one real camera frame and publish both through the Rosbridge_Client to /camera/image/compressed and /camera/camera_info. Usage: CameraPublish [configPath]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ExecuteCameraPublish));
