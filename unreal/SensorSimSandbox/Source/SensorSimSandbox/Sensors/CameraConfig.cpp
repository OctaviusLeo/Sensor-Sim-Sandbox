// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Sensors/CameraConfig.h"

#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace
{
	/** Strip a trailing `# comment` and surrounding whitespace. */
	FString StripComment(const FString& InLine)
	{
		FString Line = InLine;
		int32 HashPos = INDEX_NONE;
		if (Line.FindChar(TEXT('#'), HashPos))
		{
			Line = Line.Left(HashPos);
		}
		return Line.TrimStartAndEnd();
	}

	/** Split `key: value` on the first colon. Returns false if no colon. */
	bool SplitKeyValue(const FString& Line, FString& OutKey, FString& OutValue)
	{
		int32 ColonPos = INDEX_NONE;
		if (!Line.FindChar(TEXT(':'), ColonPos))
		{
			return false;
		}
		OutKey = Line.Left(ColonPos).TrimStartAndEnd();
		OutValue = Line.Mid(ColonPos + 1).TrimStartAndEnd();
		return true;
	}

	/** Strip surrounding single/double quotes from a scalar value. */
	FString Unquote(const FString& In)
	{
		FString S = In;
		if (S.Len() >= 2)
		{
			const TCHAR First = S[0];
			const TCHAR Last = S[S.Len() - 1];
			if ((First == TEXT('"') && Last == TEXT('"')) ||
				(First == TEXT('\'') && Last == TEXT('\'')))
			{
				S = S.Mid(1, S.Len() - 2);
			}
		}
		return S;
	}
}

FSensorBoundSet FCameraConfig::MakeBoundSet()
{
	// Declared in the design's parameter order so the first out-of-bound
	// parameter is reported deterministically. These bounds match EXACTLY the
	// camera bounds the Property 2 config-bounds test enumerates (task 5.3,
	// ConfigBoundsEnforcedTest.cpp): width/height (1-7680, int), rate_hz
	// (0.1-100.0), pixel_noise_sigma (0.0-255.0), frame_dropout_prob (0.0-1.0),
	// latency_ms (0-1000, int). The sampling interval is NOT a bounded sensor
	// parameter and is not added here.
	FSensorBoundSet Bounds;
	Bounds
		.Add(TEXT("width"),              1.0,   7680.0, /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ true)
		.Add(TEXT("height"),             1.0,   7680.0, /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ true)
		.Add(TEXT("rate_hz"),            0.1,   100.0,  /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ false)
		.Add(TEXT("pixel_noise_sigma"),  0.0,   255.0,  /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ false)
		.Add(TEXT("frame_dropout_prob"), 0.0,   1.0,    /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ false)
		.Add(TEXT("latency_ms"),         0.0,   1000.0, /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ true);
	return Bounds;
}

TMap<FString, double> FCameraConfig::ToCandidateMap() const
{
	TMap<FString, double> Candidate;
	Candidate.Add(TEXT("width"),              static_cast<double>(Width));
	Candidate.Add(TEXT("height"),             static_cast<double>(Height));
	Candidate.Add(TEXT("rate_hz"),            RateHz);
	Candidate.Add(TEXT("pixel_noise_sigma"),  PixelNoiseSigma);
	Candidate.Add(TEXT("frame_dropout_prob"), FrameDropoutProb);
	Candidate.Add(TEXT("latency_ms"),         static_cast<double>(LatencyMs));
	return Candidate;
}

FSensorConfigValidationResult FCameraConfig::Validate() const
{
	return MakeBoundSet().Validate(ToCandidateMap());
}

bool FCameraConfig::ParseSamplingInterval(const FString& Ratio, int32& OutN, int32& OutM)
{
	FString Left, Right;
	if (!Ratio.Split(TEXT("/"), &Left, &Right))
	{
		return false;
	}
	Left = Left.TrimStartAndEnd();
	Right = Right.TrimStartAndEnd();
	if (Left.IsEmpty() || Right.IsEmpty() || !Left.IsNumeric() || !Right.IsNumeric())
	{
		return false;
	}
	const int32 N = FCString::Atoi(*Left);
	const int32 M = FCString::Atoi(*Right);
	if (N <= 0 || M <= 0 || N > M)
	{
		return false;
	}
	OutN = N;
	OutM = M;
	return true;
}

bool FCameraConfig::IsFrameSampled(int64 Seq) const
{
	int32 N = 0, M = 0;
	if (!ParseSamplingInterval(SamplingInterval, N, M))
	{
		// Fail-open: a malformed interval samples every frame rather than
		// silently dropping evidence of what the camera produced.
		return true;
	}
	const int64 Pos = ((Seq % M) + M) % M; // non-negative modulo
	return Pos < static_cast<int64>(N);
}

bool FCameraConfig::ParseFromString(const FString& YamlText, FCameraConfig& OutConfig, FString& OutError)
{
	OutConfig = FCameraConfig();
	OutError.Reset();

	TArray<FString> Lines;
	YamlText.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

	for (const FString& Raw : Lines)
	{
		const FString Line = StripComment(Raw);
		if (Line.IsEmpty())
		{
			continue;
		}

		FString Key, Value;
		if (!SplitKeyValue(Line, Key, Value))
		{
			continue; // not a key: value line; ignore
		}
		if (Value.IsEmpty())
		{
			continue;
		}

		// The sampling interval is a string ratio ("N/M"), not a number.
		if (Key == TEXT("camera_sampling_interval") || Key == TEXT("sampling_interval"))
		{
			OutConfig.SamplingInterval = Unquote(Value);
			continue;
		}

		if (!Value.IsNumeric())
		{
			OutError = FString::Printf(
				TEXT("camera config key '%s' has non-numeric value '%s'"), *Key, *Value);
			return false;
		}

		const double Num = FCString::Atod(*Value);
		if (Key == TEXT("width"))                   { OutConfig.Width = FMath::RoundToInt(Num); }
		else if (Key == TEXT("height"))             { OutConfig.Height = FMath::RoundToInt(Num); }
		else if (Key == TEXT("rate_hz"))            { OutConfig.RateHz = Num; }
		else if (Key == TEXT("pixel_noise_sigma"))  { OutConfig.PixelNoiseSigma = Num; }
		else if (Key == TEXT("frame_dropout_prob")) { OutConfig.FrameDropoutProb = Num; }
		else if (Key == TEXT("latency_ms"))         { OutConfig.LatencyMs = FMath::RoundToInt(Num); }
		// Unknown keys are ignored so the config can grow without breaking.
	}

	return true;
}

bool FCameraConfig::LoadFromFile(const FString& FilePath, FCameraConfig& OutConfig, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		OutError = FString::Printf(TEXT("could not read camera config file: %s"), *FilePath);
		return false;
	}
	return ParseFromString(Text, OutConfig, OutError);
}

FString FCameraConfig::ResolveConfigPath()
{
	FString ConfigPath;
	if (FParse::Value(FCommandLine::Get(), TEXT("CameraConfig="), ConfigPath) && !ConfigPath.IsEmpty())
	{
		return ConfigPath;
	}

	// Default: repo-root config/camera.yaml. The uproject lives at
	// <repo>/unreal/SensorSimSandbox/, so the repo root is two levels up.
	const FString ProjectDir = FPaths::ProjectDir();
	const FString RepoRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(ProjectDir, TEXT("../../")));
	return FPaths::Combine(RepoRoot, TEXT("config"), TEXT("camera.yaml"));
}
