// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Misc/AutomationTest.h"

#include "Sensors/ImuGpsModels.h"
#include "Pipeline/SensorRng.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include <cmath>

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, tasks 20.2 and 21.2 (M5, Requirements 11.1, 11.2).
//
// 20.2 applies the shared pipeline properties to the IMU channel, each over a
// seeded generator loop of >= 100 iterations:
//   Property 1 (determinism): same seed + config + truth => identical output.
//   Property 4 (dropout): 20,000 draws per iteration, dropped fraction within
//     +-2 pp of p, p = 0 drops nothing and p = 1 drops everything.
//   Property 5 (latency): synthetic clock stepping L/50; every release delay
//     within +-20% of L, and L = 0 releases with delay exactly 0.
// 21.2 example tests: in-bound GPS config accepted; NavSatFix fields populated
// from the pipeline output.
// ---------------------------------------------------------------------------

namespace
{
	FImuConfig RandomImuConfig(FSensorRng& G)
	{
		FImuConfig C;
		C.RateHz = 1.0 + 199.0 * G.NextUniform();
		C.AccelNoiseSigma = 0.5 * G.NextUniform();
		C.AccelBias = -0.5 + G.NextUniform();
		C.GyroNoiseSigma = 0.05 * G.NextUniform();
		C.GyroBias = -0.05 + 0.1 * G.NextUniform();
		C.DropoutProb = G.NextUniform();
		C.LatencyMs = static_cast<int32>(1000.0 * G.NextUniform());
		return C;
	}
}

// Feature: sensor-sim-sandbox, Property 1: Pipeline determinism under fixed seed (IMU channel)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImuDeterminismTest, "SensorSimSandbox.Imu.Property1_Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FImuDeterminismTest::RunTest(const FString& Parameters)
{
	FSensorRng Gen(int64(20201), TEXT("imu_property1"));
	for (int32 Iter = 0; Iter < 128; ++Iter)
	{
		const FImuConfig Config = RandomImuConfig(Gen);
		const int64 Seed = static_cast<int64>(Gen.NextUniform() * 1.0e9);
		FImuPipelineRunner A, B;
		if (!TestTrue(TEXT("config accepted"), A.Initialise(Seed, Config).bValid && B.Initialise(Seed, Config).bValid))
		{
			return false;
		}
		for (int32 k = 0; k < 50; ++k)
		{
			const FVector Acc(Gen.NextGaussian(0.0, 2.0), Gen.NextGaussian(0.0, 2.0), 9.8 + Gen.NextGaussian(0.0, 1.0));
			const FVector Gyr(Gen.NextGaussian(0.0, 0.5), Gen.NextGaussian(0.0, 0.5), Gen.NextGaussian(0.0, 0.5));
			const FVectorSample SA = A.Process(k, k * 0.02, Acc, Gyr);
			const FVectorSample SB = B.Process(k, k * 0.02, Acc, Gyr);
			bool bSame = SA.bDropped == SB.bDropped && SA.Published.Num() == 6 && SB.Published.Num() == 6;
			for (int32 i = 0; bSame && i < 6; ++i)
			{
				bSame = SA.Published[i] == SB.Published[i];
			}
			if (!bSame)
			{
				AddError(FString::Printf(TEXT("iter %d sample %d: outputs differ for identical seed/config/truth"), Iter, k));
				return false;
			}
		}
	}
	return true;
}

// Feature: sensor-sim-sandbox, Property 4: Dropout fraction matches configured probability (IMU channel)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImuDropoutTest, "SensorSimSandbox.Imu.Property4_Dropout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FImuDropoutTest::RunTest(const FString& Parameters)
{
	FSensorRng Gen(int64(20204), TEXT("imu_property4"));
	constexpr int32 Draws = 20000;
	for (int32 Iter = 0; Iter < 100; ++Iter)
	{
		FImuConfig Config;
		Config.DropoutProb = (Iter == 0) ? 0.0 : (Iter == 1) ? 1.0 : Gen.NextUniform();
		FImuPipelineRunner R;
		R.Initialise(1000 + Iter, Config);
		int32 Dropped = 0;
		for (int32 k = 0; k < Draws; ++k)
		{
			Dropped += R.Process(k, 0.0, FVector(0, 0, 9.8), FVector::ZeroVector).bDropped ? 1 : 0;
		}
		const double Frac = static_cast<double>(Dropped) / Draws;
		if (FMath::Abs(Frac - Config.DropoutProb) > 0.02
			|| (Config.DropoutProb == 0.0 && Dropped != 0)
			|| (Config.DropoutProb == 1.0 && Dropped != Draws))
		{
			AddError(FString::Printf(TEXT("iter %d: p=%.4f measured %.4f"), Iter, Config.DropoutProb, Frac));
			return false;
		}
	}
	return true;
}

// Feature: sensor-sim-sandbox, Property 5: Latency queue releases within tolerance (IMU channel)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImuLatencyTest, "SensorSimSandbox.Imu.Property5_Latency",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FImuLatencyTest::RunTest(const FString& Parameters)
{
	FSensorRng Gen(int64(20205), TEXT("imu_property5"));
	for (int32 Iter = 0; Iter < 100; ++Iter)
	{
		FImuConfig Config;
		Config.DropoutProb = 0.0;
		Config.LatencyMs = (Iter == 0) ? 0 : 1 + static_cast<int32>(999.0 * Gen.NextUniform());
		FImuPipelineRunner R;
		R.Initialise(2000 + Iter, Config);
		const double L = Config.LatencySeconds();
		const double Step = (L > 0.0) ? L / 50.0 : 0.001;
		int32 Released = 0;
		for (int32 Tick = 0; Tick < 400; ++Tick)
		{
			const double Now = Tick * Step;
			if (Tick < 100)
			{
				R.EnqueueForLatency(R.Process(Tick, Now, FVector(0, 0, 9.8), FVector::ZeroVector));
			}
			for (const TReleasedSample<FVectorSample>& Out : R.ReleaseDue(Now))
			{
				++Released;
				const double Delay = Out.PublishTime - Out.CaptureTime;
				const bool bOk = (L == 0.0) ? (Delay == 0.0) : (FMath::Abs(Delay - L) <= 0.2 * L);
				if (!bOk)
				{
					AddError(FString::Printf(TEXT("iter %d: L=%.4f s, delay %.6f s"), Iter, L, Delay));
					return false;
				}
			}
		}
		if (Released != 100)
		{
			AddError(FString::Printf(TEXT("iter %d: released %d of 100"), Iter, Released));
			return false;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGpsInBoundConfigTest, "SensorSimSandbox.Gps.InBoundConfigAccepted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FGpsInBoundConfigTest::RunTest(const FString& Parameters)
{
	FGpsConfig Defaults;
	FString Error;
	TestTrue(TEXT("config/gps.yaml parses"), FGpsConfig::LoadFromFile(FGpsConfig::ResolveConfigPath(), Defaults, Error));
	TestTrue(TEXT("config/gps.yaml in bounds"), Defaults.Validate().bValid);

	FGpsConfig Lo;
	Lo.RateHz = 0.1; Lo.PositionNoiseSigmaM = 0.0; Lo.PositionBiasM = -100.0; Lo.DropoutProb = 0.0; Lo.LatencyMs = 0;
	FGpsConfig Hi;
	Hi.RateHz = 100.0; Hi.PositionNoiseSigmaM = 100.0; Hi.PositionBiasM = 100.0; Hi.DropoutProb = 1.0; Hi.LatencyMs = 1000;
	TestTrue(TEXT("all lower edges accepted"), Lo.Validate().bValid);
	TestTrue(TEXT("all upper edges accepted"), Hi.Validate().bValid);

	FGpsPipelineRunner Runner;
	TestTrue(TEXT("runner adopts in-bound config"), Runner.Initialise(1234, Hi).bValid && Runner.IsReady());

	FGpsConfig Bad = Defaults;
	Bad.PositionNoiseSigmaM = 101.0;
	const FSensorConfigValidationResult R = Bad.Validate();
	TestFalse(TEXT("out-of-bound sigma rejected"), R.bValid);
	TestEqual(TEXT("rejection names the parameter"), R.OffendingParameter, FString(TEXT("position_noise_sigma_m")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGpsNavSatFixTest, "SensorSimSandbox.Gps.NavSatFixFromPipeline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FGpsNavSatFixTest::RunTest(const FString& Parameters)
{
	FGpsConfig Config;
	Config.DropoutProb = 0.0;
	FGpsPipelineRunner Runner;
	Runner.Initialise(1234, Config);
	const FVectorSample S = Runner.Process(7, 1.5, FVector(100.0, -50.0, 2.0));
	TestFalse(TEXT("p = 0 never drops"), S.bDropped);
	TestTrue(TEXT("published differs from truth (noise + bias)"), S.Published[0] != S.Truth[0]);

	const TSharedRef<FJsonObject> Msg = FImuGpsMessages::BuildNavSatFix(S, Config, 1000.25);
	double Lat = 0, Lon = 0, Alt = 0;
	FImuGpsMessages::EnuToGeodetic(Config, S.Published[0], S.Published[1], S.Published[2], Lat, Lon, Alt);
	TestEqual(TEXT("latitude from published north"), Msg->GetNumberField(TEXT("latitude")), Lat);
	TestEqual(TEXT("longitude from published east"), Msg->GetNumberField(TEXT("longitude")), Lon);
	TestEqual(TEXT("altitude from published up"), Msg->GetNumberField(TEXT("altitude")), Alt);
	TestTrue(TEXT("south of origin lowers latitude"), Lat < Config.OriginLatDeg); // y = -50 m
	TestTrue(TEXT("east of origin raises longitude"), Lon > Config.OriginLonDeg); // x = +100 m
	TestEqual(TEXT("header.frame_id"), Msg->GetObjectField(TEXT("header"))->GetStringField(TEXT("frame_id")), FString(TEXT("gps_link")));
	TestEqual(TEXT("stamp sec"), Msg->GetObjectField(TEXT("header"))->GetObjectField(TEXT("stamp"))->GetNumberField(TEXT("sec")), 1000.0);
	const TArray<TSharedPtr<FJsonValue>>& Cov = Msg->GetArrayField(TEXT("position_covariance"));
	TestEqual(TEXT("covariance has 9 entries"), Cov.Num(), 9);
	TestEqual(TEXT("covariance diagonal = sigma^2"), Cov[0]->AsNumber(), Config.PositionNoiseSigmaM * Config.PositionNoiseSigmaM);
	TestEqual(TEXT("covariance type diagonal known"), Msg->GetNumberField(TEXT("position_covariance_type")), 2.0);
	TestEqual(TEXT("status fix"), Msg->GetObjectField(TEXT("status"))->GetNumberField(TEXT("status")), 0.0);

	FImuConfig ImuCfg;
	FImuPipelineRunner Imu;
	Imu.Initialise(1234, ImuCfg);
	const FVectorSample I = Imu.Process(0, 0.0, FVector(0.1, 0.2, 9.8), FVector(0.0, 0.0, 0.3));
	const TSharedRef<FJsonObject> ImuMsg = FImuGpsMessages::BuildImu(I, ImuCfg, 5.0);
	TestEqual(TEXT("imu accel z from pipeline"), ImuMsg->GetObjectField(TEXT("linear_acceleration"))->GetNumberField(TEXT("z")), I.Published[2]);
	TestEqual(TEXT("imu gyro z from pipeline"), ImuMsg->GetObjectField(TEXT("angular_velocity"))->GetNumberField(TEXT("z")), I.Published[5]);
	TestEqual(TEXT("imu orientation not provided"), ImuMsg->GetArrayField(TEXT("orientation_covariance"))[0]->AsNumber(), -1.0);
	return true;
}
