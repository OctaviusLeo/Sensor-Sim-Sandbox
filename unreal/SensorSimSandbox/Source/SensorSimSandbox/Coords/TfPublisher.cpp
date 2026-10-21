// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Coords/TfPublisher.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogTfPublisher, Log, All);

namespace
{
	/**
	 * Serialize a JSON object to a COMPACT (condensed, single-line) string.
	 * rosbridge accepts either form, but a condensed op is smaller over the
	 * WebSocket and gives deterministic, whitespace-free framing the verifier
	 * can match exactly.
	 */
	FString JsonToString(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		using FCondensedWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;
		using FCondensedWriterFactory = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;
		TSharedRef<FCondensedWriter> Writer = FCondensedWriterFactory::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	/** {x, y, z} JSON object from a REP 103 position. */
	TSharedRef<FJsonObject> MakeVector3(const FRep103Position& P)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), P.X);
		O->SetNumberField(TEXT("y"), P.Y);
		O->SetNumberField(TEXT("z"), P.Z);
		return O;
	}

	/**
	 * {x, y, z, w} JSON object from a REP 103 quaternion. ROS serializes
	 * geometry_msgs/Quaternion in x, y, z, w order (the FRep103Quat stores
	 * w, x, y, z), so reorder here.
	 */
	TSharedRef<FJsonObject> MakeQuaternion(const FRep103Quat& Q)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), Q.X);
		O->SetNumberField(TEXT("y"), Q.Y);
		O->SetNumberField(TEXT("z"), Q.Z);
		O->SetNumberField(TEXT("w"), Q.W);
		return O;
	}

	/** Split fractional seconds into a builtin_interfaces/Time {sec, nanosec}. */
	TSharedRef<FJsonObject> MakeStamp(double StampSeconds)
	{
		const double Clamped = FMath::Max(0.0, StampSeconds);
		const int64 Sec = static_cast<int64>(FMath::FloorToDouble(Clamped));
		int64 Nanosec = static_cast<int64>(FMath::RoundToDouble((Clamped - static_cast<double>(Sec)) * 1.0e9));
		// Rounding can carry nanosec to a full second; normalise.
		if (Nanosec >= 1000000000)
		{
			Nanosec -= 1000000000;
		}

		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("sec"), static_cast<double>(Sec));
		O->SetNumberField(TEXT("nanosec"), static_cast<double>(Nanosec));
		return O;
	}

	/** Build one geometry_msgs/TransformStamped for a single frame. */
	TSharedRef<FJsonObject> MakeTransformStamped(const FTfFrame& Frame, double StampSeconds)
	{
		// header: { stamp, frame_id }  -- frame_id is the PARENT frame.
		TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
		Header->SetObjectField(TEXT("stamp"), MakeStamp(StampSeconds));
		Header->SetStringField(TEXT("frame_id"), Frame.ParentFrameId);

		// transform: { translation (REP 103), rotation (REP 103) }
		const FRep103Position Pos = FRep103::ConvertPosition(Frame.UePositionCm);
		const FRep103Quat Rot = FRep103::ConvertRotation(Frame.UeRotation);

		TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
		Transform->SetObjectField(TEXT("translation"), MakeVector3(Pos));
		Transform->SetObjectField(TEXT("rotation"), MakeQuaternion(Rot));

		TSharedRef<FJsonObject> Stamped = MakeShared<FJsonObject>();
		Stamped->SetObjectField(TEXT("header"), Header);
		Stamped->SetStringField(TEXT("child_frame_id"), Frame.ChildFrameId);
		Stamped->SetObjectField(TEXT("transform"), Transform);
		return Stamped;
	}
}

// ---------------------------------------------------------------------------
// FCapturingTfTransport
// ---------------------------------------------------------------------------

void FCapturingTfTransport::Send(const FString& OpJson)
{
	SentOps.Add(OpJson);
	UE_LOG(LogTfPublisher, Verbose, TEXT("TF op: %s"), *OpJson);
}

int32 FCapturingTfTransport::NumPublishes() const
{
	int32 Count = 0;
	for (const FString& Op : SentOps)
	{
		if (Op.Contains(TEXT("\"op\":\"publish\"")))
		{
			++Count;
		}
	}
	return Count;
}

// ---------------------------------------------------------------------------
// Static builders
// ---------------------------------------------------------------------------

FString FTfPublisher::BuildAdvertiseJson()
{
	TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("op"), TEXT("advertise"));
	Op->SetStringField(TEXT("topic"), GetTopic());
	Op->SetStringField(TEXT("type"), GetMessageType());
	return JsonToString(Op);
}

FString FTfPublisher::BuildPublishJson(const TArray<FTfFrame>& Frames, double StampSeconds)
{
	// msg: tf2_msgs/msg/TFMessage { transforms: TransformStamped[] }
	TArray<TSharedPtr<FJsonValue>> Transforms;
	Transforms.Reserve(Frames.Num());
	for (const FTfFrame& Frame : Frames)
	{
		Transforms.Add(MakeShared<FJsonValueObject>(MakeTransformStamped(Frame, StampSeconds)));
	}

	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetArrayField(TEXT("transforms"), Transforms);

	TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("op"), TEXT("publish"));
	Op->SetStringField(TEXT("topic"), GetTopic());
	Op->SetObjectField(TEXT("msg"), Msg);
	return JsonToString(Op);
}

// ---------------------------------------------------------------------------
// FTfPublisher
// ---------------------------------------------------------------------------

FTfPublisher::FTfPublisher(ITfTransport* InTransport, double InRateHz)
	: Transport(InTransport)
	// Clamp UP to the >= 10 Hz contract: a smaller requested rate never lowers
	// the published rate below the Requirement 7.4 floor.
	, RateHz(FMath::Max(InRateHz, MinRateHz))
{
}

void FTfPublisher::Advertise()
{
	if (bAdvertised)
	{
		return;
	}
	bAdvertised = true;
	if (Transport)
	{
		Transport->Send(BuildAdvertiseJson());
	}
}

FString FTfPublisher::Publish(const TArray<FTfFrame>& Frames, double StampSeconds)
{
	const FString Op = BuildPublishJson(Frames, StampSeconds);
	if (Transport)
	{
		Transport->Send(Op);
	}
	++PublishCount;
	return Op;
}

bool FTfPublisher::Tick(double NowSeconds, const TArray<FTfFrame>& Frames)
{
	const double Period = GetPeriodSeconds();

	// First tick always publishes so TF is available immediately, and the next
	// deadline is anchored to this first publish time.
	if (!bHasPublished)
	{
		Publish(Frames, NowSeconds);
		LastPublishTime = NowSeconds;
		NextDeadline = NowSeconds + Period;
		bHasPublished = true;
		return true;
	}

	// Not yet due. A small epsilon absorbs floating-point accumulation in the
	// caller's clock so a tick landing a hair before an exact period boundary
	// still fires, keeping the achieved rate from drifting BELOW the target.
	const double Epsilon = Period * 1.0e-6;
	if (NowSeconds + Epsilon < NextDeadline)
	{
		return false;
	}

	// Due: publish once and advance the deadline by exactly one period from the
	// previous DEADLINE (not from NowSeconds). Anchoring to the deadline means
	// per-tick rounding never accumulates, so the long-run rate stays locked to
	// the target >= 10 Hz. If the clock jumped forward past several periods,
	// advance to just beyond NowSeconds (publish once, do not spam catch-ups).
	Publish(Frames, NowSeconds);
	LastPublishTime = NowSeconds;
	NextDeadline += Period;
	if (NextDeadline <= NowSeconds)
	{
		NextDeadline = NowSeconds + Period;
	}
	return true;
}
