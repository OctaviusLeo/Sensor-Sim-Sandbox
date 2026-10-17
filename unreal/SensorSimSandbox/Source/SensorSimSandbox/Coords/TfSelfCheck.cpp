// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Coords/TfSelfCheck.h"

#include "Coords/Rep103.h"
#include "Coords/TfPublisher.h"
#include "HAL/IConsoleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogTfSelfCheck, Log, All);

namespace
{
	constexpr double Tol = 1.0e-6;

	bool NearlyEqual(double A, double B, double Eps = Tol)
	{
		return FMath::Abs(A - B) <= Eps;
	}

	// Build the rover body + sensor frames (fixed Unreal-cm poses) for the TF
	// message build and the scheduler check. Non-identity rotations/positions so
	// the REP 103 conversion is actually exercised in the serialized message.
	TArray<FTfFrame> MakeFrames()
	{
		TArray<FTfFrame> Frames;

		// Rover body under odom: 2.5 m forward, 1 m left (REP 103) => UE (250, -100, 0) cm.
		Frames.Add(FTfFrame(
			SensorFrames::Odom, SensorFrames::BaseLink,
			FVector(250.0, -100.0, 0.0),
			FQuat(FVector::UpVector, FMath::DegreesToRadians(30.0))));

		// Lidar on the rover body, 50 cm up.
		Frames.Add(FTfFrame(
			SensorFrames::BaseLink, SensorFrames::LidarLink,
			FVector(0.0, 0.0, 50.0),
			FQuat::Identity));

		// Camera on the rover body, forward + up (M3 frame, present in TF now).
		Frames.Add(FTfFrame(
			SensorFrames::BaseLink, SensorFrames::CameraLink,
			FVector(30.0, 0.0, 40.0),
			FQuat(FVector::RightVector, FMath::DegreesToRadians(-10.0))));

		return Frames;
	}

	// Check 1: position conversion against hand-worked expected values.
	bool CheckPosition(FString& OutDetail)
	{
		struct FCase { FVector Ue; FRep103Position Expected; };
		const FCase Cases[] =
		{
			{ FVector(100.0, 200.0, 300.0),  FRep103Position( 1.0, -2.0,  3.0) },
			{ FVector(0.0, 0.0, 0.0),        FRep103Position( 0.0,  0.0,  0.0) },
			{ FVector(-250.0, 50.0, -75.0),  FRep103Position(-2.5, -0.5, -0.75) },
		};

		for (const FCase& C : Cases)
		{
			const FRep103Position Got = FRep103::ConvertPosition(C.Ue);
			if (!NearlyEqual(Got.X, C.Expected.X) || !NearlyEqual(Got.Y, C.Expected.Y) || !NearlyEqual(Got.Z, C.Expected.Z))
			{
				OutDetail = FString::Printf(
					TEXT("position (%g,%g,%g) cm -> got (%.9g,%.9g,%.9g), expected (%.9g,%.9g,%.9g)"),
					C.Ue.X, C.Ue.Y, C.Ue.Z, Got.X, Got.Y, Got.Z, C.Expected.X, C.Expected.Y, C.Expected.Z);
				return false;
			}
		}
		OutDetail = TEXT("position mapping (x/100,-y/100,z/100) matches hand-worked values within 1e-6 m");
		return true;
	}

	// Check 2 + 3: rotation conversion mapping and forward-then-back identity.
	bool CheckRotation(FString& OutDetail, bool& bOutRoundTrip)
	{
		bOutRoundTrip = true;

		// Mapping on an explicit quaternion: q_ros = (w, -x, y, -z).
		const FQuat Ue(0.2, 0.3, 0.4, 0.8); // FQuat(X, Y, Z, W)
		const FRep103Quat Ros = FRep103::ConvertRotation(Ue);
		if (!NearlyEqual(Ros.W, Ue.W) || !NearlyEqual(Ros.X, -Ue.X) || !NearlyEqual(Ros.Y, Ue.Y) || !NearlyEqual(Ros.Z, -Ue.Z))
		{
			OutDetail = FString::Printf(
				TEXT("rotation map got (w%.6g,x%.6g,y%.6g,z%.6g), expected (w%.6g,x%.6g,y%.6g,z%.6g)"),
				Ros.W, Ros.X, Ros.Y, Ros.Z, Ue.W, -Ue.X, Ue.Y, -Ue.Z);
			return false;
		}

		// Forward-then-back identity over several normalized rotations.
		const FQuat Samples[] =
		{
			FQuat::Identity,
			FQuat(FVector::UpVector, FMath::DegreesToRadians(90.0)),
			FQuat(FVector(1, 1, 1).GetSafeNormal(), FMath::DegreesToRadians(123.0)),
			FQuat(0.2, 0.3, 0.4, 0.8).GetNormalized(),
		};

		for (const FQuat& Q : Samples)
		{
			const FRep103Quat R = FRep103::ConvertRotation(Q);
			const FQuat Back = FRep103::InvertRotation(R);
			// Compare component-wise within 1e-6; a quaternion and its negation
			// represent the same rotation, so accept either sign.
			const bool Same =
				NearlyEqual(Back.X, Q.X) && NearlyEqual(Back.Y, Q.Y) && NearlyEqual(Back.Z, Q.Z) && NearlyEqual(Back.W, Q.W);
			const bool Negated =
				NearlyEqual(Back.X, -Q.X) && NearlyEqual(Back.Y, -Q.Y) && NearlyEqual(Back.Z, -Q.Z) && NearlyEqual(Back.W, -Q.W);
			if (!Same && !Negated)
			{
				OutDetail = FString::Printf(
					TEXT("round trip failed: q(%.6g,%.6g,%.6g,%.6g) -> back(%.6g,%.6g,%.6g,%.6g)"),
					Q.X, Q.Y, Q.Z, Q.W, Back.X, Back.Y, Back.Z, Back.W);
				bOutRoundTrip = false;
				return false;
			}
		}

		OutDetail = TEXT("rotation map q_ros=(w,-x,y,-z) correct; forward-then-back identity within 1e-6");
		return true;
	}

	// Check 4: TF message build is well-formed (advertise + a publish carrying
	// every frame, with the REP 103-converted rover body translation present).
	bool CheckTfMessage(FString& OutDetail)
	{
		const TArray<FTfFrame> Frames = MakeFrames();

		const FString Advertise = FTfPublisher::BuildAdvertiseJson();
		const FString Publish = FTfPublisher::BuildPublishJson(Frames, /*StampSeconds*/ 12.345);

		if (!Advertise.Contains(TEXT("\"op\":\"advertise\"")) ||
			!Advertise.Contains(TEXT("\"topic\":\"/tf\"")) ||
			!Advertise.Contains(TEXT("tf2_msgs/msg/TFMessage")))
		{
			OutDetail = FString::Printf(TEXT("advertise malformed: %s"), *Advertise);
			return false;
		}

		// Parse the publish op back and inspect it numerically so the check does
		// not depend on how the JSON writer formats integral-valued doubles.
		TSharedPtr<FJsonObject> Op;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Publish);
		if (!FJsonSerializer::Deserialize(Reader, Op) || !Op.IsValid())
		{
			OutDetail = FString::Printf(TEXT("publish op is not valid JSON: %s"), *Publish);
			return false;
		}

		if (Op->GetStringField(TEXT("op")) != TEXT("publish") || Op->GetStringField(TEXT("topic")) != TEXT("/tf"))
		{
			OutDetail = FString::Printf(TEXT("publish op/topic malformed: %s"), *Publish);
			return false;
		}

		const TSharedPtr<FJsonObject>* Msg = nullptr;
		if (!Op->TryGetObjectField(TEXT("msg"), Msg) || !Msg)
		{
			OutDetail = TEXT("publish op has no msg object");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Transforms = nullptr;
		if (!(*Msg)->TryGetArrayField(TEXT("transforms"), Transforms) || !Transforms)
		{
			OutDetail = TEXT("TFMessage has no transforms array");
			return false;
		}
		if (Transforms->Num() != Frames.Num())
		{
			OutDetail = FString::Printf(TEXT("expected %d transforms, got %d"), Frames.Num(), Transforms->Num());
			return false;
		}

		// First transform is the rover body: verify its parent/child frame ids
		// and the REP 103-converted translation (UE (250,-100,0) cm -> (2.5,1,0) m).
		const TSharedPtr<FJsonObject> Body = (*Transforms)[0]->AsObject();
		if (!Body.IsValid())
		{
			OutDetail = TEXT("rover body transform missing");
			return false;
		}
		if (Body->GetStringField(TEXT("child_frame_id")) != FString(SensorFrames::BaseLink))
		{
			OutDetail = FString::Printf(TEXT("first transform child_frame_id is '%s', expected base_link"),
				*Body->GetStringField(TEXT("child_frame_id")));
			return false;
		}
		const TSharedPtr<FJsonObject> BodyHeader = Body->GetObjectField(TEXT("header"));
		const TSharedPtr<FJsonObject> BodyTransform = Body->GetObjectField(TEXT("transform"));
		const TSharedPtr<FJsonObject> Translation = BodyTransform->GetObjectField(TEXT("translation"));
		if (BodyHeader->GetStringField(TEXT("frame_id")) != FString(SensorFrames::Odom))
		{
			OutDetail = TEXT("rover body header.frame_id is not odom");
			return false;
		}
		if (!NearlyEqual(Translation->GetNumberField(TEXT("x")), 2.5) ||
			!NearlyEqual(Translation->GetNumberField(TEXT("y")), 1.0) ||
			!NearlyEqual(Translation->GetNumberField(TEXT("z")), 0.0))
		{
			OutDetail = FString::Printf(TEXT("rover body translation (%.6g,%.6g,%.6g), expected (2.5,1,0)"),
				Translation->GetNumberField(TEXT("x")), Translation->GetNumberField(TEXT("y")),
				Translation->GetNumberField(TEXT("z")));
			return false;
		}

		// Every frame id present as a child_frame_id somewhere in the array.
		for (const FTfFrame& F : Frames)
		{
			bool bFound = false;
			for (const TSharedPtr<FJsonValue>& V : *Transforms)
			{
				const TSharedPtr<FJsonObject> T = V->AsObject();
				if (T.IsValid() && T->GetStringField(TEXT("child_frame_id")) == F.ChildFrameId)
				{
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				OutDetail = FString::Printf(TEXT("publish missing child_frame_id %s"), *F.ChildFrameId);
				return false;
			}
		}

		OutDetail = FString::Printf(
			TEXT("TFMessage well-formed: %d transforms on /tf, rover body REP 103 translation (2.5,1,0) m correct"),
			Transforms->Num());
		return true;
	}

	// Check 5: the scheduler publishes at >= 10 Hz over a synthetic clock.
	bool CheckRate(FString& OutDetail, double& OutRateHz)
	{
		FCapturingTfTransport Transport;
		FTfPublisher Publisher(&Transport); // default rate = MinRateHz (10)
		Publisher.Advertise();

		const TArray<FTfFrame> Frames = MakeFrames();

		// Simulate game time at a fine step (200 Hz tick), letting the scheduler
		// decide when to publish. Record the time of each publish so the rate is
		// measured from the ACTUAL publish timeline (first..last), and the
		// largest gap between consecutive publishes is checked against the period
		// — a gap <= period is the real >= 10 Hz guarantee.
		const double Duration = 3.0;
		const double TickStep = 1.0 / 200.0;
		TArray<double> PublishTimes;
		int32 PrevCount = 0;
		for (double Now = 0.0; Now <= Duration + 1e-9; Now += TickStep)
		{
			Publisher.Tick(Now, Frames);
			if (Publisher.GetPublishCount() > PrevCount)
			{
				PublishTimes.Add(Now);
				PrevCount = Publisher.GetPublishCount();
			}
		}

		const int32 Publishes = Publisher.GetPublishCount();
		const int32 TransportPublishes = Transport.NumPublishes();

		if (Publishes < 2 || PublishTimes.Num() < 2)
		{
			OutDetail = FString::Printf(TEXT("too few publishes (%d) to measure a rate"), Publishes);
			return false;
		}

		// Achieved rate over the actual publish span (design's (n-1)/T form).
		const double Span = PublishTimes.Last() - PublishTimes[0];
		OutRateHz = (Span > 0.0) ? (static_cast<double>(PublishTimes.Num() - 1) / Span) : 0.0;

		// Largest inter-publish gap: must not exceed one period (+ a float
		// epsilon). This is the stricter, timeline-local >= 10 Hz check.
		double MaxGap = 0.0;
		for (int32 i = 1; i < PublishTimes.Num(); ++i)
		{
			MaxGap = FMath::Max(MaxGap, PublishTimes[i] - PublishTimes[i - 1]);
		}
		const double Period = Publisher.GetPeriodSeconds();

		if (TransportPublishes != Publishes)
		{
			OutDetail = FString::Printf(
				TEXT("transport received %d publishes but publisher counted %d"),
				TransportPublishes, Publishes);
			return false;
		}
		if (OutRateHz + 1e-6 < FTfPublisher::MinRateHz)
		{
			OutDetail = FString::Printf(
				TEXT("scheduler rate %.4f Hz < 10 Hz floor (%d publishes over %.3f s)"),
				OutRateHz, Publishes, Span);
			return false;
		}
		if (MaxGap > Period + (TickStep + 1e-9))
		{
			OutDetail = FString::Printf(
				TEXT("max inter-publish gap %.4f s exceeds period %.4f s (would dip below 10 Hz)"),
				MaxGap, Period);
			return false;
		}

		OutDetail = FString::Printf(
			TEXT("scheduler published %d TFMessages over %.3f s => %.4f Hz (>= 10 Hz), max gap %.4f s <= period %.4f s"),
			Publishes, Span, OutRateHz, MaxGap, Period);
		return true;
	}
}

FTfSelfCheck::FResult FTfSelfCheck::Run()
{
	FResult Result;

	FString PosDetail, RotDetail, MsgDetail, RateDetail;

	Result.bPositionConversionCorrect = CheckPosition(PosDetail);
	Result.bRotationConversionCorrect = CheckRotation(RotDetail, Result.bRoundTripIdentity);
	Result.bTfMessageWellFormed = CheckTfMessage(MsgDetail);
	Result.bRateAtLeast10Hz = CheckRate(RateDetail, Result.MeasuredRateHz);

	Result.Detail = FString::Printf(
		TEXT("position=%s [%s]; rotation=%s [%s]; tf_message=%s [%s]; rate=%s [%s]"),
		Result.bPositionConversionCorrect ? TEXT("PASS") : TEXT("FAIL"), *PosDetail,
		(Result.bRotationConversionCorrect && Result.bRoundTripIdentity) ? TEXT("PASS") : TEXT("FAIL"), *RotDetail,
		Result.bTfMessageWellFormed ? TEXT("PASS") : TEXT("FAIL"), *MsgDetail,
		Result.bRateAtLeast10Hz ? TEXT("PASS") : TEXT("FAIL"), *RateDetail);

	return Result;
}

// ---------------------------------------------------------------------------
// Console command: `TfSelfCheck`
//
// One-command headless verification for task 9.1: REP 103 conversion
// correctness + round-trip identity, TFMessage build, and the >= 10 Hz
// publish scheduler. Logs a single PASS/FAIL line the verify script can grep.
// The live `ros2 topic hz /tf` [HOST] measurement needs the Rosbridge_Client
// (task 11) and is recorded as deferred until then.
// ---------------------------------------------------------------------------
static void ExecuteTfSelfCheck(const TArray<FString>& /*Args*/)
{
	const FTfSelfCheck::FResult Result = FTfSelfCheck::Run();

	UE_LOG(LogTfSelfCheck, Log, TEXT("TfSelfCheck %s: %s"),
		Result.Passed() ? TEXT("PASS") : TEXT("FAIL"), *Result.Detail);
}

static FAutoConsoleCommand GTfSelfCheckCommand(
	TEXT("TfSelfCheck"),
	TEXT("Verify the REP 103 converter (position + rotation + round trip) and the TfPublisher (TFMessage build + >= 10 Hz scheduling) headlessly. Usage: TfSelfCheck"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&ExecuteTfSelfCheck));
