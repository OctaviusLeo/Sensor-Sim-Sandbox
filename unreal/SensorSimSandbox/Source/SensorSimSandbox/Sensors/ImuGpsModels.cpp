// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Sensors/ImuGpsModels.h"

#include "Coords/TfPublisher.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogImuGps, Log, All);

namespace
{
	/** Parse flat `key: number` YAML into a map (comments and blank lines skipped). */
	bool ParseFlatYaml(const FString& Text, const TCHAR* What, TMap<FString, double>& Out, FString& OutError)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
		for (const FString& Raw : Lines)
		{
			FString Line = Raw;
			int32 Hash = INDEX_NONE;
			if (Line.FindChar(TEXT('#'), Hash)) { Line = Line.Left(Hash); }
			Line.TrimStartAndEndInline();
			int32 Colon = INDEX_NONE;
			if (Line.IsEmpty() || !Line.FindChar(TEXT(':'), Colon)) { continue; }
			const FString Key = Line.Left(Colon).TrimStartAndEnd();
			const FString Value = Line.Mid(Colon + 1).TrimStartAndEnd();
			if (Value.IsEmpty()) { continue; }
			if (!Value.IsNumeric())
			{
				OutError = FString::Printf(TEXT("%s config key '%s' has non-numeric value '%s'"), What, *Key, *Value);
				return false;
			}
			Out.Add(Key, FCString::Atod(*Value));
		}
		return true;
	}

	FString RepoConfigPath(const TCHAR* Switch, const TCHAR* FileName)
	{
		FString Path;
		if (FParse::Value(FCommandLine::Get(), Switch, Path) && !Path.IsEmpty())
		{
			return Path;
		}
		const FString RepoRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../../")));
		return FPaths::Combine(RepoRoot, TEXT("config"), FileName);
	}

	void Get(const TMap<FString, double>& M, const TCHAR* Key, double& Out) { if (const double* V = M.Find(Key)) { Out = *V; } }
	void Get(const TMap<FString, double>& M, const TCHAR* Key, int32& Out) { if (const double* V = M.Find(Key)) { Out = FMath::RoundToInt(*V); } }

	TSharedRef<FJsonObject> MakeStamp(double StampSeconds)
	{
		const double Clamped = FMath::Max(0.0, StampSeconds);
		const int64 Sec = static_cast<int64>(FMath::FloorToDouble(Clamped));
		int64 Nanosec = static_cast<int64>(FMath::RoundToDouble((Clamped - static_cast<double>(Sec)) * 1.0e9));
		if (Nanosec >= 1000000000) { Nanosec -= 1000000000; }
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("sec"), static_cast<double>(Sec));
		O->SetNumberField(TEXT("nanosec"), static_cast<double>(Nanosec));
		return O;
	}

	TSharedRef<FJsonObject> MakeHeader(double StampSeconds, const FString& FrameId)
	{
		TSharedRef<FJsonObject> H = MakeShared<FJsonObject>();
		H->SetObjectField(TEXT("stamp"), MakeStamp(StampSeconds));
		H->SetStringField(TEXT("frame_id"), FrameId);
		return H;
	}

	TSharedRef<FJsonObject> MakeVector3(double X, double Y, double Z)
	{
		TSharedRef<FJsonObject> V = MakeShared<FJsonObject>();
		V->SetNumberField(TEXT("x"), X);
		V->SetNumberField(TEXT("y"), Y);
		V->SetNumberField(TEXT("z"), Z);
		return V;
	}

	TArray<TSharedPtr<FJsonValue>> MakeDiagCovariance(double Variance)
	{
		TArray<TSharedPtr<FJsonValue>> C;
		for (int32 i = 0; i < 9; ++i)
		{
			C.Add(MakeShared<FJsonValueNumber>((i % 4 == 0) ? Variance : 0.0));
		}
		return C;
	}

	FString Num(double V) { return FString::Printf(TEXT("%.17g"), V); }

	constexpr double EarthRadiusM = 6378137.0;
}

// ---------------------------------------------------------------------------
// Configs
// ---------------------------------------------------------------------------

FSensorBoundSet FImuConfig::MakeBoundSet()
{
	FSensorBoundSet B;
	B.Add(TEXT("rate_hz"),           0.1,    1000.0)
	 .Add(TEXT("accel_noise_sigma"), 0.0,    100.0)
	 .Add(TEXT("accel_bias"),        -100.0, 100.0)
	 .Add(TEXT("gyro_noise_sigma"),  0.0,    10.0)
	 .Add(TEXT("gyro_bias"),         -10.0,  10.0)
	 .Add(TEXT("dropout_prob"),      0.0,    1.0)
	 .Add(TEXT("latency_ms"),        0.0,    1000.0, true, true, /*intOnly*/ true);
	return B;
}

TMap<FString, double> FImuConfig::ToCandidateMap() const
{
	TMap<FString, double> M;
	M.Add(TEXT("rate_hz"), RateHz);
	M.Add(TEXT("accel_noise_sigma"), AccelNoiseSigma);
	M.Add(TEXT("accel_bias"), AccelBias);
	M.Add(TEXT("gyro_noise_sigma"), GyroNoiseSigma);
	M.Add(TEXT("gyro_bias"), GyroBias);
	M.Add(TEXT("dropout_prob"), DropoutProb);
	M.Add(TEXT("latency_ms"), static_cast<double>(LatencyMs));
	return M;
}

FSensorConfigValidationResult FImuConfig::Validate() const { return MakeBoundSet().Validate(ToCandidateMap()); }

bool FImuConfig::ParseFromString(const FString& YamlText, FImuConfig& Out, FString& OutError)
{
	Out = FImuConfig();
	TMap<FString, double> M;
	if (!ParseFlatYaml(YamlText, TEXT("imu"), M, OutError)) { return false; }
	Get(M, TEXT("rate_hz"), Out.RateHz);
	Get(M, TEXT("accel_noise_sigma"), Out.AccelNoiseSigma);
	Get(M, TEXT("accel_bias"), Out.AccelBias);
	Get(M, TEXT("gyro_noise_sigma"), Out.GyroNoiseSigma);
	Get(M, TEXT("gyro_bias"), Out.GyroBias);
	Get(M, TEXT("dropout_prob"), Out.DropoutProb);
	Get(M, TEXT("latency_ms"), Out.LatencyMs);
	return true;
}

bool FImuConfig::LoadFromFile(const FString& FilePath, FImuConfig& Out, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		OutError = FString::Printf(TEXT("could not read imu config file: %s"), *FilePath);
		return false;
	}
	return ParseFromString(Text, Out, OutError);
}

FString FImuConfig::ResolveConfigPath() { return RepoConfigPath(TEXT("ImuConfig="), TEXT("imu.yaml")); }

FSensorBoundSet FGpsConfig::MakeBoundSet()
{
	FSensorBoundSet B;
	B.Add(TEXT("rate_hz"),                0.1,    100.0)
	 .Add(TEXT("position_noise_sigma_m"), 0.0,    100.0)
	 .Add(TEXT("position_bias_m"),        -100.0, 100.0)
	 .Add(TEXT("dropout_prob"),           0.0,    1.0)
	 .Add(TEXT("latency_ms"),             0.0,    1000.0, true, true, /*intOnly*/ true)
	 .Add(TEXT("origin_lat_deg"),         -89.0,  89.0)
	 .Add(TEXT("origin_lon_deg"),         -180.0, 180.0)
	 .Add(TEXT("origin_alt_m"),           -500.0, 10000.0);
	return B;
}

TMap<FString, double> FGpsConfig::ToCandidateMap() const
{
	TMap<FString, double> M;
	M.Add(TEXT("rate_hz"), RateHz);
	M.Add(TEXT("position_noise_sigma_m"), PositionNoiseSigmaM);
	M.Add(TEXT("position_bias_m"), PositionBiasM);
	M.Add(TEXT("dropout_prob"), DropoutProb);
	M.Add(TEXT("latency_ms"), static_cast<double>(LatencyMs));
	M.Add(TEXT("origin_lat_deg"), OriginLatDeg);
	M.Add(TEXT("origin_lon_deg"), OriginLonDeg);
	M.Add(TEXT("origin_alt_m"), OriginAltM);
	return M;
}

FSensorConfigValidationResult FGpsConfig::Validate() const { return MakeBoundSet().Validate(ToCandidateMap()); }

bool FGpsConfig::ParseFromString(const FString& YamlText, FGpsConfig& Out, FString& OutError)
{
	Out = FGpsConfig();
	TMap<FString, double> M;
	if (!ParseFlatYaml(YamlText, TEXT("gps"), M, OutError)) { return false; }
	Get(M, TEXT("rate_hz"), Out.RateHz);
	Get(M, TEXT("position_noise_sigma_m"), Out.PositionNoiseSigmaM);
	Get(M, TEXT("position_bias_m"), Out.PositionBiasM);
	Get(M, TEXT("dropout_prob"), Out.DropoutProb);
	Get(M, TEXT("latency_ms"), Out.LatencyMs);
	Get(M, TEXT("origin_lat_deg"), Out.OriginLatDeg);
	Get(M, TEXT("origin_lon_deg"), Out.OriginLonDeg);
	Get(M, TEXT("origin_alt_m"), Out.OriginAltM);
	return true;
}

bool FGpsConfig::LoadFromFile(const FString& FilePath, FGpsConfig& Out, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		OutError = FString::Printf(TEXT("could not read gps config file: %s"), *FilePath);
		return false;
	}
	return ParseFromString(Text, Out, OutError);
}

FString FGpsConfig::ResolveConfigPath() { return RepoConfigPath(TEXT("GpsConfig="), TEXT("gps.yaml")); }

// ---------------------------------------------------------------------------
// Runners
// ---------------------------------------------------------------------------

FSensorConfigValidationResult FImuPipelineRunner::Initialise(int64 RunSeed, const FImuConfig& InConfig, const FString& SensorName)
{
	const FSensorConfigValidationResult R = InConfig.Validate();
	if (!R.bValid)
	{
		UE_LOG(LogImuGps, Error, TEXT("IMU Initialise rejected config: %s"), *R.ErrorMessage);
		return R;
	}
	Config = InConfig;
	Pipeline = MakeUnique<FSensorPipeline>(RunSeed, SensorName);
	Queue.SetLatencySeconds(Config.LatencySeconds());
	return R;
}

FVectorSample FImuPipelineRunner::Process(int64 Seq, double CaptureTime, const FVector& Accel, const FVector& Gyro)
{
	FVectorSample S;
	S.Seq = Seq;
	S.CaptureTime = CaptureTime;
	S.Truth = { Accel.X, Accel.Y, Accel.Z, Gyro.X, Gyro.Y, Gyro.Z };
	S.Published.SetNum(6);
	FSensorRng& Rng = Pipeline->GetRng();
	for (int32 i = 0; i < 6; ++i)
	{
		const bool bAccel = i < 3;
		const double Sigma = bAccel ? Config.AccelNoiseSigma : Config.GyroNoiseSigma;
		const double Bias = bAccel ? Config.AccelBias : Config.GyroBias;
		const double Noise = Sigma > 0.0 ? Rng.NextGaussian(0.0, Sigma) : 0.0;
		S.Published[i] = S.Truth[i] + Noise + Bias;
	}
	S.bDropped = Pipeline->ApplyFrameDropout(Config.DropoutProb);
	return S;
}

FSensorConfigValidationResult FGpsPipelineRunner::Initialise(int64 RunSeed, const FGpsConfig& InConfig, const FString& SensorName)
{
	const FSensorConfigValidationResult R = InConfig.Validate();
	if (!R.bValid)
	{
		UE_LOG(LogImuGps, Error, TEXT("GPS Initialise rejected config: %s"), *R.ErrorMessage);
		return R;
	}
	Config = InConfig;
	Pipeline = MakeUnique<FSensorPipeline>(RunSeed, SensorName);
	Queue.SetLatencySeconds(Config.LatencySeconds());
	return R;
}

FVectorSample FGpsPipelineRunner::Process(int64 Seq, double CaptureTime, const FVector& PositionM)
{
	FVectorSample S;
	S.Seq = Seq;
	S.CaptureTime = CaptureTime;
	S.Truth = { PositionM.X, PositionM.Y, PositionM.Z };
	S.Published.SetNum(3);
	FSensorRng& Rng = Pipeline->GetRng();
	for (int32 i = 0; i < 3; ++i)
	{
		const double Noise = Config.PositionNoiseSigmaM > 0.0 ? Rng.NextGaussian(0.0, Config.PositionNoiseSigmaM) : 0.0;
		S.Published[i] = S.Truth[i] + Noise + Config.PositionBiasM;
	}
	S.bDropped = Pipeline->ApplyFrameDropout(Config.DropoutProb);
	return S;
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

TSharedRef<FJsonObject> FImuGpsMessages::BuildImu(const FVectorSample& Sample, const FImuConfig& Config, double StampSeconds)
{
	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetObjectField(TEXT("header"), MakeHeader(StampSeconds, SensorFrames::ImuLink));

	TSharedRef<FJsonObject> Orientation = MakeShared<FJsonObject>();
	Orientation->SetNumberField(TEXT("x"), 0.0);
	Orientation->SetNumberField(TEXT("y"), 0.0);
	Orientation->SetNumberField(TEXT("z"), 0.0);
	Orientation->SetNumberField(TEXT("w"), 1.0);
	Msg->SetObjectField(TEXT("orientation"), Orientation);
	TArray<TSharedPtr<FJsonValue>> NoOrientation = MakeDiagCovariance(0.0);
	NoOrientation[0] = MakeShared<FJsonValueNumber>(-1.0); // REP 145: orientation not provided
	Msg->SetArrayField(TEXT("orientation_covariance"), NoOrientation);

	const TArray<double>& P = Sample.Published;
	Msg->SetObjectField(TEXT("angular_velocity"), MakeVector3(P[3], P[4], P[5]));
	Msg->SetArrayField(TEXT("angular_velocity_covariance"), MakeDiagCovariance(Config.GyroNoiseSigma * Config.GyroNoiseSigma));
	Msg->SetObjectField(TEXT("linear_acceleration"), MakeVector3(P[0], P[1], P[2]));
	Msg->SetArrayField(TEXT("linear_acceleration_covariance"), MakeDiagCovariance(Config.AccelNoiseSigma * Config.AccelNoiseSigma));
	return Msg;
}

void FImuGpsMessages::EnuToGeodetic(const FGpsConfig& Config, double East, double North, double Up,
	double& OutLatDeg, double& OutLonDeg, double& OutAltM)
{
	const double Lat0 = FMath::DegreesToRadians(Config.OriginLatDeg);
	OutLatDeg = Config.OriginLatDeg + FMath::RadiansToDegrees(North / EarthRadiusM);
	OutLonDeg = Config.OriginLonDeg + FMath::RadiansToDegrees(East / (EarthRadiusM * FMath::Cos(Lat0)));
	OutAltM = Config.OriginAltM + Up;
}

TSharedRef<FJsonObject> FImuGpsMessages::BuildNavSatFix(const FVectorSample& Sample, const FGpsConfig& Config, double StampSeconds)
{
	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetObjectField(TEXT("header"), MakeHeader(StampSeconds, SensorFrames::GpsLink));

	TSharedRef<FJsonObject> Status = MakeShared<FJsonObject>();
	Status->SetNumberField(TEXT("status"), 0);  // STATUS_FIX
	Status->SetNumberField(TEXT("service"), 1); // SERVICE_GPS
	Msg->SetObjectField(TEXT("status"), Status);

	double Lat = 0.0, Lon = 0.0, Alt = 0.0;
	EnuToGeodetic(Config, Sample.Published[0], Sample.Published[1], Sample.Published[2], Lat, Lon, Alt);
	Msg->SetNumberField(TEXT("latitude"), Lat);
	Msg->SetNumberField(TEXT("longitude"), Lon);
	Msg->SetNumberField(TEXT("altitude"), Alt);
	Msg->SetArrayField(TEXT("position_covariance"),
		MakeDiagCovariance(Config.PositionNoiseSigmaM * Config.PositionNoiseSigmaM));
	Msg->SetNumberField(TEXT("position_covariance_type"), 2); // COVARIANCE_TYPE_DIAGONAL_KNOWN
	return Msg;
}

FString FImuGpsMessages::ChannelConfigJson(double RateHz, double NoiseSigma, double Bias, double DropoutProb, int32 LatencyMs)
{
	return FString::Printf(
		TEXT("{\"rate_hz\":%s,\"noise_sigma\":%s,\"bias\":%s,\"dropout_prob\":%s,\"latency_ms\":%d}"),
		*Num(RateHz), *Num(NoiseSigma), *Num(Bias), *Num(DropoutProb), LatencyMs);
}
