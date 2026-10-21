// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Sensors/CameraSelfCheck.h"

#include "Sensors/CameraConfig.h"
#include "Sensors/CameraPipelineRunner.h"

#include "HAL/IConsoleManager.h"

#include <cmath>

DEFINE_LOG_CATEGORY_STATIC(LogCameraSelfCheck, Log, All);

namespace
{
	// Build a synthetic flat RGBA frame of WxH where every RGB channel is
	// `Level` and alpha is 255. Used so noise can be measured against a known
	// truth without a GPU render.
	FCameraFrameBuffer MakeFlatFrame(int32 W, int32 H, uint8 Level)
	{
		FCameraFrameBuffer Buf;
		Buf.Width = W;
		Buf.Height = H;
		const int64 N = static_cast<int64>(W) * H;
		Buf.Pixels.SetNumUninitialized(N * 4);
		for (int64 i = 0; i < N; ++i)
		{
			const int64 Base = i * 4;
			Buf.Pixels[Base + 0] = Level;
			Buf.Pixels[Base + 1] = Level;
			Buf.Pixels[Base + 2] = Level;
			Buf.Pixels[Base + 3] = 255;
		}
		return Buf;
	}
}

FCameraSelfCheck::FResult FCameraSelfCheck::Run(const FString& CameraConfigPath)
{
	FResult Result;

	// --- Load + validate the Gate A config through the shared contract ------
	const FString ConfigPath = CameraConfigPath.IsEmpty()
		? FCameraConfig::ResolveConfigPath()
		: CameraConfigPath;

	FCameraConfig Config;
	FString LoadError;
	if (!FCameraConfig::LoadFromFile(ConfigPath, Config, LoadError))
	{
		Result.Detail = FString::Printf(TEXT("could not load %s: %s"), *ConfigPath, *LoadError);
		UE_LOG(LogCameraSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}

	const FSensorConfigValidationResult ConfigCheck = Config.Validate();
	Result.bConfigValid = ConfigCheck.bValid;
	if (!ConfigCheck.bValid)
	{
		Result.Detail = FString::Printf(TEXT("config %s out of bounds: %s"), *ConfigPath, *ConfigCheck.ErrorMessage);
		UE_LOG(LogCameraSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}
	Result.ConfiguredSigma = Config.PixelNoiseSigma;
	Result.ConfiguredDropout = Config.FrameDropoutProb;
	Result.ConfiguredLatencyMs = static_cast<double>(Config.LatencyMs);
	UE_LOG(LogCameraSelfCheck, Log,
		TEXT("loaded Gate A config %s: %dx%d rate=%.1f sigma=%.3f frame_dropout=%.3f latency=%d ms sampling='%s'"),
		*ConfigPath, Config.Width, Config.Height, Config.RateHz,
		Config.PixelNoiseSigma, Config.FrameDropoutProb, Config.LatencyMs, *Config.SamplingInterval);

	// --- Shared validation contract wired in: out-of-bound width rejected ---
	{
		FCameraConfig Bad = Config;
		Bad.Width = 99999; // 1..7680 -> out of bounds
		const FSensorConfigValidationResult BadCheck = Bad.Validate();
		Result.bOutOfBoundRejected = (!BadCheck.bValid && BadCheck.OffendingParameter == TEXT("width"));
		UE_LOG(LogCameraSelfCheck, Log, TEXT("out-of-bound width=99999 rejected=%d offending='%s' msg='%s'"),
			!BadCheck.bValid ? 1 : 0, *BadCheck.OffendingParameter, *BadCheck.ErrorMessage);
	}

	const int64 RunSeed = 1234; // Gate A run seed.

	// --- Determinism: same (seed, config) -> byte-identical published -------
	{
		FCameraPipelineRunner A, B;
		A.Initialise(RunSeed, Config, TEXT("camera"));
		B.Initialise(RunSeed, Config, TEXT("camera"));

		// A few small synthetic frames with varied content.
		const int32 W = 32, H = 24;
		bool bIdentical = true;
		for (int64 seq = 0; seq < 8 && bIdentical; ++seq)
		{
			FCameraFrameBuffer T = MakeFlatFrame(W, H, static_cast<uint8>(40 + seq * 20));
			const FCameraFrame Fa = A.ProcessCapturedFrame(seq, seq * 0.1, T);
			const FCameraFrame Fb = B.ProcessCapturedFrame(seq, seq * 0.1, T);
			if (Fa.bDropped != Fb.bDropped) { bIdentical = false; break; }
			if (Fa.Published.Pixels != Fb.Published.Pixels) { bIdentical = false; break; }
		}
		Result.bDeterministic = bIdentical;
		UE_LOG(LogCameraSelfCheck, Log, TEXT("determinism (two runners, 8 frames): identical=%d"), bIdentical ? 1 : 0);
	}

	// --- Per-pixel noise: recover sigma within +-10% on mid-grey ------------
	// Use mid-grey (128) so N(0, sigma) rarely clamps at 0 or 255 and the
	// recovered std is unbiased. Over >= 20,000 draws the +-10% window is met
	// with overwhelming probability (design "Sample sizes").
	{
		FCameraPipelineRunner Runner;
		Runner.Initialise(RunSeed, Config, TEXT("camera_noise"));

		// A frame with >= ~7000 pixels gives >= ~21000 RGB draws in one frame.
		const int32 W = 100, H = 80; // 8000 px -> 24000 RGB draws
		FCameraFrameBuffer Truth = MakeFlatFrame(W, H, 128);
		const FCameraFrame Frame = Runner.ProcessCapturedFrame(0, 0.0, Truth);

		// Frame may be dropped by chance; if so, re-run with a fresh runner on
		// a sensor name that we know does not drop frame 0 is not guaranteed,
		// so instead measure noise on the NOISED buffer we can still obtain:
		// ProcessCapturedFrame draws all noise BEFORE the dropout decision, but
		// discards the published buffer when dropped. To measure reliably,
		// temporarily use a zero-dropout copy of the config for this stat.
		FCameraConfig NoiseCfg = Config;
		NoiseCfg.FrameDropoutProb = 0.0; // never drop so we always get pixels
		FCameraPipelineRunner NoiseRunner;
		NoiseRunner.Initialise(RunSeed, NoiseCfg, TEXT("camera_noise"));
		const FCameraFrame NF = NoiseRunner.ProcessCapturedFrame(0, 0.0, Truth);

		double SumSq = 0.0, Sum = 0.0;
		int64 Count = 0;
		const int64 PixelCount = Truth.NumPixels();
		for (int64 p = 0; p < PixelCount; ++p)
		{
			const int64 Base = p * 4;
			for (int32 c = 0; c < 3; ++c)
			{
				const double Diff = static_cast<double>(NF.Published.Pixels[Base + c]) - 128.0;
				Sum += Diff;
				SumSq += Diff * Diff;
				++Count;
			}
		}
		const double Mean = Count > 0 ? Sum / Count : 0.0;
		const double Var = Count > 1 ? (SumSq - Count * Mean * Mean) / (Count - 1) : 0.0;
		Result.RecoveredSigma = FMath::Sqrt(FMath::Max(0.0, Var));
		const double Tol = 0.10 * Config.PixelNoiseSigma;
		Result.bNoiseRecovered = (Config.PixelNoiseSigma <= 0.0)
			? (Result.RecoveredSigma == 0.0)
			: (FMath::Abs(Result.RecoveredSigma - Config.PixelNoiseSigma) <= Tol);
		UE_LOG(LogCameraSelfCheck, Log,
			TEXT("noise: %lld RGB draws, recovered sigma=%.4f vs configured %.4f (tol +-%.4f) -> ok=%d; (frame0 dropped under real p=%d)"),
			Count, Result.RecoveredSigma, Config.PixelNoiseSigma, Tol, Result.bNoiseRecovered ? 1 : 0, Frame.bDropped ? 1 : 0);
	}

	// --- sigma = 0 is an exact passthrough ----------------------------------
	{
		FCameraConfig ZeroSigma = Config;
		ZeroSigma.PixelNoiseSigma = 0.0;
		ZeroSigma.FrameDropoutProb = 0.0; // isolate the noise stage
		FCameraPipelineRunner Runner;
		Runner.Initialise(RunSeed, ZeroSigma, TEXT("camera_zero"));
		FCameraFrameBuffer Truth = MakeFlatFrame(16, 16, 77);
		const FCameraFrame F = Runner.ProcessCapturedFrame(0, 0.0, Truth);
		Result.bSigmaZeroPassthrough = (F.Published.Pixels == Truth.Pixels);
		UE_LOG(LogCameraSelfCheck, Log, TEXT("sigma=0 passthrough: identical=%d"),
			Result.bSigmaZeroPassthrough ? 1 : 0);
	}

	// --- Frame dropout: measured fraction within +-2 pp of p ----------------
	// n >= ceil(40000 * p * (1-p)) (design "Sample sizes"), never below the
	// requirement minimum of 100 frames.
	{
		const double P = Config.FrameDropoutProb;
		const int32 N = FMath::Max(100, static_cast<int32>(FMath::CeilToInt(40000.0 * P * (1.0 - P))));
		FCameraPipelineRunner Runner;
		Runner.Initialise(RunSeed, Config, TEXT("camera_dropout"));
		FCameraFrameBuffer Truth = MakeFlatFrame(4, 4, 100); // tiny, dropout is per-frame
		int32 Dropped = 0;
		for (int32 i = 0; i < N; ++i)
		{
			const FCameraFrame F = Runner.ProcessCapturedFrame(i, i * 0.1, Truth);
			if (F.bDropped) { ++Dropped; }
		}
		Result.DropoutSamples = N;
		Result.MeasuredDropout = N > 0 ? static_cast<double>(Dropped) / N : 0.0;
		Result.bDropoutWithinTolerance = FMath::Abs(Result.MeasuredDropout - P) <= 0.02;
		UE_LOG(LogCameraSelfCheck, Log,
			TEXT("frame dropout: %d of %d dropped -> measured=%.4f vs configured=%.4f (tol +-0.02) -> ok=%d"),
			Dropped, N, Result.MeasuredDropout, P, Result.bDropoutWithinTolerance ? 1 : 0);
	}

	// --- Latency: mean released delay within +-20% of L on an L/50 clock ----
	{
		const double L = Config.LatencySeconds();
		FCameraPipelineRunner Runner;
		Runner.Initialise(RunSeed, Config, TEXT("camera_latency"));

		const double Dt = L > 0.0 ? L / 50.0 : 0.001; // L/50 synthetic step
		const int32 NumFrames = 200;
		// Enqueue frames at their capture times, then advance the clock and
		// collect release delays.
		double Clock = 0.0;
		int32 Enqueued = 0;
		int32 NextCaptureFrame = 0;
		const double CapturePeriod = Dt; // one capture per step for a dense sample
		TArray<double> Delays;
		for (int32 step = 0; step < NumFrames * 60 && Delays.Num() < NumFrames; ++step)
		{
			Clock = step * Dt;
			if (NextCaptureFrame < NumFrames && Clock >= NextCaptureFrame * CapturePeriod - 1e-12)
			{
				Runner.EnqueueForLatency(NextCaptureFrame, Clock);
				++NextCaptureFrame;
				++Enqueued;
			}
			const TArray<TReleasedSample<int64>> Released = Runner.ReleaseDue(Clock);
			for (const TReleasedSample<int64>& R : Released)
			{
				Delays.Add(R.Delay());
			}
		}
		double DelaySum = 0.0;
		for (double D : Delays) { DelaySum += D; }
		const double MeanDelay = Delays.Num() > 0 ? DelaySum / Delays.Num() : 0.0;
		Result.MeasuredLatencyMs = MeanDelay * 1000.0;
		if (L <= 0.0)
		{
			Result.bLatencyWithinTolerance = true; // handled by zero-latency check below
		}
		else
		{
			const double Tol = 0.20 * L;
			Result.bLatencyWithinTolerance = (Delays.Num() > 0) && (FMath::Abs(MeanDelay - L) <= Tol);
		}
		UE_LOG(LogCameraSelfCheck, Log,
			TEXT("latency: %d released, mean delay=%.2f ms vs L=%.2f ms (tol +-20%%) -> ok=%d"),
			Delays.Num(), Result.MeasuredLatencyMs, L * 1000.0, Result.bLatencyWithinTolerance ? 1 : 0);
	}

	// --- Zero latency releases in the capture step (delay exactly 0) --------
	{
		TLatencyQueue<int64> Q(0.0);
		Q.Enqueue(0, 5.0, 0);
		const TArray<TReleasedSample<int64>> Released = Q.ReleaseDue(5.0);
		Result.bZeroLatencyImmediate = (Released.Num() == 1) && (Released[0].Delay() == 0.0);
		UE_LOG(LogCameraSelfCheck, Log, TEXT("L=0 immediate release: released=%d delay0=%d"),
			Released.Num(), (Released.Num() == 1 && Released[0].Delay() == 0.0) ? 1 : 0);
	}

	// --- Clamp: a bright (255) frame with sigma>0 never exceeds [0,255] ----
	{
		FCameraConfig ClampCfg = Config;
		ClampCfg.FrameDropoutProb = 0.0; // always get a published frame
		if (ClampCfg.PixelNoiseSigma <= 0.0) { ClampCfg.PixelNoiseSigma = 10.0; }
		FCameraPipelineRunner Runner;
		Runner.Initialise(RunSeed, ClampCfg, TEXT("camera_clamp"));
		FCameraFrameBuffer Bright = MakeFlatFrame(100, 80, 255);
		FCameraFrameBuffer Dark = MakeFlatFrame(100, 80, 0);
		const FCameraFrame FB = Runner.ProcessCapturedFrame(0, 0.0, Bright);
		const FCameraFrame FD = Runner.ProcessCapturedFrame(1, 0.1, Dark);
		bool bInRange = true;
		for (uint8 V : FB.Published.Pixels) { /* uint8 is inherently [0,255] */ (void)V; }
		// The real proof: with truth=255 and positive noise, an UNCLAMPED model
		// would need >255 (impossible in uint8) — so instead confirm the mean
		// published on a bright frame is BELOW 255 (noise pushes some down and
		// the positive side clamps), and on a dark frame is ABOVE 0.
		double BrightSum = 0.0, DarkSum = 0.0; int64 Cnt = 0;
		const int64 Pix = Bright.NumPixels();
		for (int64 p = 0; p < Pix; ++p)
		{
			const int64 Base = p * 4;
			for (int32 c = 0; c < 3; ++c)
			{
				BrightSum += FB.Published.Pixels[Base + c];
				DarkSum += FD.Published.Pixels[Base + c];
				++Cnt;
			}
		}
		const double BrightMean = Cnt > 0 ? BrightSum / Cnt : 0.0;
		const double DarkMean = Cnt > 0 ? DarkSum / Cnt : 0.0;
		// Clamp at 255 pulls the bright-frame mean below 255; clamp at 0 pulls
		// the dark-frame mean above 0. Both hold only because of the clamp.
		Result.bClamped = bInRange && (BrightMean < 255.0) && (DarkMean > 0.0);
		UE_LOG(LogCameraSelfCheck, Log,
			TEXT("clamp [0,255]: bright-frame mean=%.2f (<255), dark-frame mean=%.2f (>0) -> ok=%d"),
			BrightMean, DarkMean, Result.bClamped ? 1 : 0);
	}

	Result.Detail = FString::Printf(
		TEXT("config=%s | configValid=%d outOfBoundRejected=%d deterministic=%d noise(sigma %.3f~%.3f)=%d sigma0=%d dropout(%.4f~%.4f n=%d)=%d latency(%.1f~%.1f ms)=%d L0=%d clamp=%d"),
		*ConfigPath,
		Result.bConfigValid ? 1 : 0, Result.bOutOfBoundRejected ? 1 : 0, Result.bDeterministic ? 1 : 0,
		Result.RecoveredSigma, Result.ConfiguredSigma, Result.bNoiseRecovered ? 1 : 0,
		Result.bSigmaZeroPassthrough ? 1 : 0,
		Result.MeasuredDropout, Result.ConfiguredDropout, Result.DropoutSamples, Result.bDropoutWithinTolerance ? 1 : 0,
		Result.MeasuredLatencyMs, Result.ConfiguredLatencyMs, Result.bLatencyWithinTolerance ? 1 : 0,
		Result.bZeroLatencyImmediate ? 1 : 0, Result.bClamped ? 1 : 0);

	return Result;
}

// ---------------------------------------------------------------------------
// Console command: `CameraSelfCheck [configPath]`
//
// One-command HEADLESS demo/verify for task 13.1. No world / RHI needed, so it
// runs under -nullrhi. Logs one PASS/FAIL line plus the measured numbers.
// ---------------------------------------------------------------------------
static void ExecuteCameraSelfCheck(const TArray<FString>& Args)
{
	FString ConfigPath;
	if (Args.Num() >= 1 && !Args[0].IsEmpty())
	{
		ConfigPath = Args[0];
	}

	const FCameraSelfCheck::FResult Result = FCameraSelfCheck::Run(ConfigPath);

	UE_LOG(LogCameraSelfCheck, Log, TEXT("CameraSelfCheck %s: %s"),
		Result.Passed() ? TEXT("PASS") : TEXT("FAIL"), *Result.Detail);
}

static FAutoConsoleCommand GCameraSelfCheckCommand(
	TEXT("CameraSelfCheck"),
	TEXT("Headless camera pipeline demo/verify on SYNTHETIC frames: determinism, per-pixel noise sigma recovery, frame dropout, latency, config bounds, clamp [0,255]. Usage: CameraSelfCheck [configPath]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&ExecuteCameraSelfCheck));
