// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Sensors/CameraModel.h"

#include "Engine/World.h"
#include "Components/SceneComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Scene.h"
#include "TextureResource.h"
#include "RHI.h"
#include "Sensors/CameraMessage.h"   // FCameraMessage::HorizontalFovDeg (calibration source of truth)

DEFINE_LOG_CATEGORY_STATIC(LogCameraModel, Log, All);

ACameraModel::ACameraModel()
{
	PrimaryActorTick.bCanEverTick = false;

	// A root scene component so the actor's transform (and the capture
	// component attached to it) is honored at the spawn location.
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("CameraRoot"));

	CaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
	CaptureComponent->SetupAttachment(RootComponent);
	// Capture the final tonemapped LDR scene colour so the readback pixels are
	// the visible RGB a viewer would see (not an HDR/scene-depth buffer).
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	// Capture on demand (we call CaptureScene explicitly before each readback),
	// not every frame, so the Ground_Truth is tied to the capture time.
	CaptureComponent->bCaptureEveryFrame = false;
	CaptureComponent->bCaptureOnMovement = false;
	// Pin the horizontal FOV to the SAME constant the CameraInfo intrinsics are
	// computed from (FCameraMessage::HorizontalFovDeg), so the published
	// calibration (K/P) matches the actual projection used to render the frame
	// (Requirement 4.4). FOVAngle is Unreal's HORIZONTAL field of view.
	CaptureComponent->FOVAngle = static_cast<float>(FCameraMessage::HorizontalFovDeg);
}

FSensorConfigValidationResult ACameraModel::Initialise(int64 InRunSeed, const FCameraConfig& Config, const FString& InSensorName)
{
	NextSeq = 0;
	const FSensorConfigValidationResult Result = Runner.Initialise(InRunSeed, Config, InSensorName);
	if (Result.bValid)
	{
		EnsureRenderTarget();
		UE_LOG(LogCameraModel, Log,
			TEXT("Camera actor ready: %dx%d px, rate=%.2f Hz, sigma=%.2f, frame_dropout=%.3f, latency=%d ms."),
			Runner.GetConfig().Width, Runner.GetConfig().Height, Runner.GetConfig().RateHz,
			Runner.GetConfig().PixelNoiseSigma, Runner.GetConfig().FrameDropoutProb, Runner.GetConfig().LatencyMs);
	}
	return Result;
}

FSensorConfigValidationResult ACameraModel::ApplyConfig(const FCameraConfig& Candidate)
{
	const FSensorConfigValidationResult Result = Runner.ApplyConfig(Candidate);
	if (Result.bValid)
	{
		EnsureRenderTarget();
	}
	return Result;
}

void ACameraModel::EnsureRenderTarget()
{
	const FCameraConfig& Config = Runner.GetConfig();
	const int32 W = FMath::Max(1, Config.Width);
	const int32 H = FMath::Max(1, Config.Height);

	if (RenderTarget == nullptr)
	{
		RenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("CameraRenderTarget"));
		RenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
		RenderTarget->ClearColor = FLinearColor::Black;
		RenderTarget->bAutoGenerateMips = false;
	}

	if (RenderTarget->SizeX != W || RenderTarget->SizeY != H)
	{
		RenderTarget->InitAutoFormat(W, H);
		RenderTarget->UpdateResourceImmediate(true);
	}

	if (CaptureComponent)
	{
		CaptureComponent->TextureTarget = RenderTarget;
	}
}

bool ACameraModel::CaptureTruthPixels(FCameraFrameBuffer& OutTruth) const
{
	OutTruth.Pixels.Reset();
	OutTruth.Width = 0;
	OutTruth.Height = 0;

	if (CaptureComponent == nullptr || RenderTarget == nullptr)
	{
		UE_LOG(LogCameraModel, Warning, TEXT("Camera CaptureTruthPixels: no capture component / render target."));
		return false;
	}

	FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
	if (Resource == nullptr)
	{
		// No RHI resource (e.g. a -nullrhi headless run): cannot read back.
		UE_LOG(LogCameraModel, Warning, TEXT("Camera CaptureTruthPixels: render target has no RHI resource (nullrhi?)."));
		return false;
	}

	// Render the scene into the render target NOW, so the readback is this
	// capture's pristine Ground_Truth.
	CaptureComponent->CaptureScene();

	TArray<FColor> Colors;
	FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
	ReadFlags.SetLinearToGamma(false);
	if (!Resource->ReadPixels(Colors, ReadFlags))
	{
		UE_LOG(LogCameraModel, Warning, TEXT("Camera CaptureTruthPixels: ReadPixels failed."));
		return false;
	}

	const int32 W = RenderTarget->SizeX;
	const int32 H = RenderTarget->SizeY;
	if (Colors.Num() != W * H)
	{
		UE_LOG(LogCameraModel, Warning,
			TEXT("Camera CaptureTruthPixels: readback size %d != %dx%d."), Colors.Num(), W, H);
		return false;
	}

	OutTruth.Width = W;
	OutTruth.Height = H;
	OutTruth.Pixels.SetNumUninitialized(static_cast<int64>(W) * H * 4);
	for (int64 i = 0; i < Colors.Num(); ++i)
	{
		const FColor& C = Colors[i];
		const int64 Base = i * 4;
		OutTruth.Pixels[Base + 0] = C.R;
		OutTruth.Pixels[Base + 1] = C.G;
		OutTruth.Pixels[Base + 2] = C.B;
		OutTruth.Pixels[Base + 3] = C.A;
	}
	return true;
}

FCameraFrame ACameraModel::CaptureFrame(double CaptureTime)
{
	// --- 1. Capture Ground_Truth BEFORE the pipeline runs ------------------
	FCameraFrameBuffer Truth;
	const bool bCaptured = CaptureTruthPixels(Truth);
	if (!bCaptured)
	{
		// Degrade gracefully: no truth frame means nothing to publish. Return a
		// dropped frame with an empty truth buffer so a headless caller does
		// not publish an unprocessed frame.
		FCameraFrame Frame;
		Frame.Seq = NextSeq++;
		Frame.CaptureTime = CaptureTime;
		Frame.bDropped = true;
		Frame.bSampled = Runner.GetConfig().IsFrameSampled(Frame.Seq);
		return Frame;
	}

	// --- 2+3. Per-pixel noise (clamp [0,255]) then whole-frame dropout -----
	FCameraFrame Frame = Runner.ProcessCapturedFrame(NextSeq++, CaptureTime, Truth);

	// --- 4. Latency: a non-dropped frame is enqueued for release later -----
	if (!Frame.bDropped)
	{
		Runner.EnqueueForLatency(Frame.Seq, CaptureTime);
	}

	return Frame;
}
