// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Sensors/CameraPipelineRunner.h"

DEFINE_LOG_CATEGORY_STATIC(LogCameraRunner, Log, All);

FSensorConfigValidationResult FCameraPipelineRunner::Initialise(int64 InRunSeed, const FCameraConfig& Config, const FString& InSensorName)
{
	const FSensorConfigValidationResult Result = Config.Validate();
	if (!Result.bValid)
	{
		UE_LOG(LogCameraRunner, Error,
			TEXT("Camera Initialise rejected config: %s (offending '%s'). Retaining last valid config."),
			*Result.ErrorMessage, *Result.OffendingParameter);
		return Result;
	}

	RunSeed = InRunSeed;
	SensorName = InSensorName.IsEmpty() ? TEXT("camera") : InSensorName;
	CurrentConfig = Config;
	Pipeline = MakeUnique<FSensorPipeline>(RunSeed, SensorName);
	LatencyQueue.SetLatencySeconds(CurrentConfig.LatencySeconds());

	UE_LOG(LogCameraRunner, Log,
		TEXT("Camera runner initialised: seed=%lld sensor='%s' sensor_seed=0x%016llX -> %dx%d px, sigma=%.4f p=%.4f latency=%d ms sampling='%s'"),
		RunSeed, *SensorName, Pipeline->GetSensorSeed(),
		CurrentConfig.Width, CurrentConfig.Height,
		CurrentConfig.PixelNoiseSigma, CurrentConfig.FrameDropoutProb, CurrentConfig.LatencyMs,
		*CurrentConfig.SamplingInterval);

	return Result;
}

FSensorConfigValidationResult FCameraPipelineRunner::ApplyConfig(const FCameraConfig& Candidate)
{
	const FSensorConfigValidationResult Result = Candidate.Validate();
	if (!Result.bValid)
	{
		UE_LOG(LogCameraRunner, Error,
			TEXT("Camera ApplyConfig rejected: %s (offending '%s'). Last valid config retained."),
			*Result.ErrorMessage, *Result.OffendingParameter);
		return Result;
	}
	CurrentConfig = Candidate;
	LatencyQueue.SetLatencySeconds(CurrentConfig.LatencySeconds());
	return Result;
}

FCameraFrame FCameraPipelineRunner::ProcessCapturedFrame(int64 Seq, double CaptureTime, const FCameraFrameBuffer& Truth)
{
	FCameraFrame Frame;
	Frame.Seq = Seq;
	Frame.CaptureTime = CaptureTime;
	Frame.Truth = Truth; // captured truth, verbatim (captured first)
	Frame.bSampled = CurrentConfig.IsFrameSampled(Seq);

	if (!Pipeline.IsValid())
	{
		// No pipeline: cannot publish. Report as dropped so a caller never
		// publishes an unprocessed frame.
		Frame.bDropped = true;
		return Frame;
	}

	// --- Stage 2: per-pixel Gaussian noise, clamped to [0,255] -------------
	// Build an FSensorValue array over the RGB channels only (alpha carried
	// verbatim), run the SHARED noise stage with clamp enabled, then fold the
	// noised channels back into an RGBA buffer. The draw order is R,G,B per
	// pixel in row-major order (the order the values are laid out here), which
	// is fixed and reproducible for the seed. Noise draws happen whether or not
	// the frame is later dropped, so the stream advances deterministically.
	const int64 PixelCount = Truth.NumPixels();
	TArray<FSensorValue> RgbTruth;
	RgbTruth.Reserve(PixelCount * 3);
	for (int64 p = 0; p < PixelCount; ++p)
	{
		const int64 Base = p * 4;
		RgbTruth.Add(FSensorValue(static_cast<double>(Truth.Pixels[Base + 0]))); // R
		RgbTruth.Add(FSensorValue(static_cast<double>(Truth.Pixels[Base + 1]))); // G
		RgbTruth.Add(FSensorValue(static_cast<double>(Truth.Pixels[Base + 2]))); // B
	}

	const TArray<FSensorValue> RgbNoisy = Pipeline->AddGaussianNoise(
		RgbTruth,
		CurrentConfig.PixelNoiseSigma,
		/*bClamp*/ true,
		/*ClampMin*/ 0.0,
		/*ClampMax*/ 255.0);

	FCameraFrameBuffer PublishedBuf;
	PublishedBuf.Width = Truth.Width;
	PublishedBuf.Height = Truth.Height;
	PublishedBuf.Pixels.SetNumUninitialized(Truth.Pixels.Num());
	for (int64 p = 0; p < PixelCount; ++p)
	{
		const int64 Base = p * 4;
		const int64 Rgb = p * 3;
		// Clamp already applied in [0,255]; round to the nearest 8-bit level.
		PublishedBuf.Pixels[Base + 0] = static_cast<uint8>(FMath::RoundToInt(RgbNoisy[Rgb + 0].GetValue()));
		PublishedBuf.Pixels[Base + 1] = static_cast<uint8>(FMath::RoundToInt(RgbNoisy[Rgb + 1].GetValue()));
		PublishedBuf.Pixels[Base + 2] = static_cast<uint8>(FMath::RoundToInt(RgbNoisy[Rgb + 2].GetValue()));
		PublishedBuf.Pixels[Base + 3] = Truth.Pixels[Base + 3]; // alpha verbatim
	}

	// --- Stage 3: whole-frame dropout (one Bernoulli draw) -----------------
	// The draw is made AFTER all noise draws (fixed order), and is made even
	// though the noised buffer may be discarded, so the RNG stream is identical
	// for a given seed regardless of the dropout outcome.
	Frame.bDropped = Pipeline->ApplyFrameDropout(CurrentConfig.FrameDropoutProb);

	if (!Frame.bDropped)
	{
		Frame.Published = MoveTemp(PublishedBuf);
	}
	// A dropped frame is NOT published and carries no image refs; bSampled is
	// still reported for logging but the logger writes no refs for a dropped
	// frame (design "Ground-truth logging format": camera rows).

	return Frame;
}

void FCameraPipelineRunner::EnqueueForLatency(int64 Seq, double CaptureTime)
{
	LatencyQueue.Enqueue(Seq, CaptureTime, Seq);
}

TArray<TReleasedSample<int64>> FCameraPipelineRunner::ReleaseDue(double Now)
{
	return LatencyQueue.ReleaseDue(Now);
}
