// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Sensors/LidarConfig.h"

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
}

FSensorBoundSet FLidarConfig::MakeBoundSet()
{
	// Declared in the design's parameter order so the first out-of-bound
	// parameter is reported deterministically. These bounds match exactly the
	// lidar bounds the Property 2 config-bounds test enumerates (task 5.3).
	FSensorBoundSet Bounds;
	Bounds
		.Add(TEXT("channels"),               1.0,   128.0,  /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ true)
		.Add(TEXT("h_fov_deg"),              0.0,   360.0,  /*minInc*/ false, /*maxInc*/ true,  /*intOnly*/ false)
		.Add(TEXT("v_fov_deg"),              0.0,   180.0,  /*minInc*/ false, /*maxInc*/ true,  /*intOnly*/ false)
		.Add(TEXT("angular_resolution_deg"), 0.01,  10.0,   /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ false)
		.Add(TEXT("max_range_m"),            0.1,   1000.0, /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ false)
		.Add(TEXT("rate_hz"),                0.1,   100.0,  /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ false)
		.Add(TEXT("range_noise_sigma_m"),    0.0,   10.0,   /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ false)
		.Add(TEXT("dropout_prob"),           0.0,   1.0,    /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ false)
		.Add(TEXT("latency_ms"),             0.0,   1000.0, /*minInc*/ true,  /*maxInc*/ true,  /*intOnly*/ true);
	return Bounds;
}

TMap<FString, double> FLidarConfig::ToCandidateMap() const
{
	TMap<FString, double> Candidate;
	Candidate.Add(TEXT("channels"),               static_cast<double>(Channels));
	Candidate.Add(TEXT("h_fov_deg"),              HFovDeg);
	Candidate.Add(TEXT("v_fov_deg"),              VFovDeg);
	Candidate.Add(TEXT("angular_resolution_deg"), AngularResolutionDeg);
	Candidate.Add(TEXT("max_range_m"),            MaxRangeM);
	Candidate.Add(TEXT("rate_hz"),                RateHz);
	Candidate.Add(TEXT("range_noise_sigma_m"),    RangeNoiseSigmaM);
	Candidate.Add(TEXT("dropout_prob"),           DropoutProb);
	Candidate.Add(TEXT("latency_ms"),             static_cast<double>(LatencyMs));
	return Candidate;
}

FSensorConfigValidationResult FLidarConfig::Validate() const
{
	return MakeBoundSet().Validate(ToCandidateMap());
}

bool FLidarConfig::ParseFromString(const FString& YamlText, FLidarConfig& OutConfig, FString& OutError)
{
	OutConfig = FLidarConfig();
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
		if (!Value.IsNumeric())
		{
			OutError = FString::Printf(
				TEXT("lidar config key '%s' has non-numeric value '%s'"), *Key, *Value);
			return false;
		}

		const double Num = FCString::Atod(*Value);
		if (Key == TEXT("channels"))                     { OutConfig.Channels = FMath::RoundToInt(Num); }
		else if (Key == TEXT("h_fov_deg"))               { OutConfig.HFovDeg = Num; }
		else if (Key == TEXT("v_fov_deg"))               { OutConfig.VFovDeg = Num; }
		else if (Key == TEXT("angular_resolution_deg"))  { OutConfig.AngularResolutionDeg = Num; }
		else if (Key == TEXT("max_range_m"))             { OutConfig.MaxRangeM = Num; }
		else if (Key == TEXT("rate_hz"))                 { OutConfig.RateHz = Num; }
		else if (Key == TEXT("range_noise_sigma_m"))     { OutConfig.RangeNoiseSigmaM = Num; }
		else if (Key == TEXT("dropout_prob"))            { OutConfig.DropoutProb = Num; }
		else if (Key == TEXT("latency_ms"))              { OutConfig.LatencyMs = FMath::RoundToInt(Num); }
		// Unknown keys are ignored so the config can grow without breaking.
	}

	return true;
}

bool FLidarConfig::LoadFromFile(const FString& FilePath, FLidarConfig& OutConfig, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		OutError = FString::Printf(TEXT("could not read lidar config file: %s"), *FilePath);
		return false;
	}
	return ParseFromString(Text, OutConfig, OutError);
}

FString FLidarConfig::ResolveConfigPath()
{
	FString ConfigPath;
	if (FParse::Value(FCommandLine::Get(), TEXT("LidarConfig="), ConfigPath) && !ConfigPath.IsEmpty())
	{
		return ConfigPath;
	}

	// Default: repo-root config/lidar.yaml. The uproject lives at
	// <repo>/unreal/SensorSimSandbox/, so the repo root is two levels up.
	const FString ProjectDir = FPaths::ProjectDir();
	const FString RepoRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(ProjectDir, TEXT("../../")));
	return FPaths::Combine(RepoRoot, TEXT("config"), TEXT("lidar.yaml"));
}
