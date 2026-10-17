// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Sensors/LidarPointCloud.h"

#include "Sensors/LidarPipelineRunner.h"   // FLidarScan, FLidarRayDir
#include "Sensors/LidarScanGeometry.h"
#include "Coords/Rep103.h"
#include "Coords/TfPublisher.h"            // SensorFrames::LidarLink
#include "Bridge/RosbridgeClient.h"        // FRosbridgeFraming::EncodeBase64

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	/**
	 * Serialize a JSON object to a COMPACT (condensed, single-line) string.
	 * Mirrors FTfPublisher's framing so every rosbridge op in the project has
	 * the same deterministic, whitespace-free shape.
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

	/**
	 * Split fractional seconds into a builtin_interfaces/Time {sec, nanosec}.
	 * Same construction FTfPublisher uses for header stamps, so the lidar and
	 * TF stamps are built identically.
	 */
	TSharedRef<FJsonObject> MakeStamp(double StampSeconds)
	{
		const double Clamped = FMath::Max(0.0, StampSeconds);
		const int64 Sec = static_cast<int64>(FMath::FloorToDouble(Clamped));
		int64 Nanosec = static_cast<int64>(FMath::RoundToDouble((Clamped - static_cast<double>(Sec)) * 1.0e9));
		if (Nanosec >= 1000000000)
		{
			Nanosec -= 1000000000;
		}

		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("sec"), static_cast<double>(Sec));
		O->SetNumberField(TEXT("nanosec"), static_cast<double>(Nanosec));
		return O;
	}

	/** One sensor_msgs/PointField {name, offset, datatype, count}. */
	TSharedRef<FJsonValue> MakePointField(const FString& Name, int32 Offset)
	{
		TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
		F->SetStringField(TEXT("name"), Name);
		F->SetNumberField(TEXT("offset"), static_cast<double>(Offset));
		F->SetNumberField(TEXT("datatype"), static_cast<double>(FLidarPointCloud::PointFieldFloat32));
		F->SetNumberField(TEXT("count"), 1.0);
		return MakeShared<FJsonValueObject>(F);
	}

	/** Append a float32 as 4 little-endian bytes to `OutBytes`. */
	void AppendFloatLE(TArray<uint8>& OutBytes, float Value)
	{
		uint32 Bits;
		FMemory::Memcpy(&Bits, &Value, sizeof(float)); // IEEE-754 bit pattern
		OutBytes.Add(static_cast<uint8>(Bits & 0xFF));
		OutBytes.Add(static_cast<uint8>((Bits >> 8) & 0xFF));
		OutBytes.Add(static_cast<uint8>((Bits >> 16) & 0xFF));
		OutBytes.Add(static_cast<uint8>((Bits >> 24) & 0xFF));
	}
}

FString FLidarPointCloud::LidarFrameId()
{
	return SensorFrames::LidarLink;
}

TArray<FLidarPointCloud::FPackedPoint> FLidarPointCloud::ComputeReturnPoints(const FLidarScan& Scan)
{
	TArray<FPackedPoint> Points;
	Points.Reserve(Scan.NumReturns());

	const int32 N = Scan.PublishedRangeM.Num();
	for (int32 i = 0; i < N; ++i)
	{
		const FSensorValue& Published = Scan.PublishedRangeM[i];
		if (!Published.IsSet())
		{
			// No-hit or dropped ray: excluded from the cloud entirely (the
			// point count equals the returns, is_dense stays true).
			continue;
		}

		// Rays may be absent (e.g. a pure test that passed {} for geometry);
		// without a direction we cannot place the point, so skip it. In the
		// live path PerformScan always supplies Rays, so this never trips.
		if (!Scan.Rays.IsValidIndex(i))
		{
			continue;
		}

		const double RangeM = Published.GetValue();
		const FVector Dir = Scan.Rays[i].Direction; // unit, UE sensor-local (cm frame)

		// Point in the UE sensor frame, in CENTIMETERS, so the fixed REP 103
		// converter (which divides by 100) yields meters. The *100 here and the
		// /100 inside ConvertPosition cancel, leaving range_m in meters with the
		// REP 103 Y-flip applied.
		const FVector PointUeCm = Dir * (RangeM * 100.0);
		const FRep103Position Ros = FRep103::ConvertPosition(PointUeCm);

		FPackedPoint P;
		P.X = static_cast<float>(Ros.X);
		P.Y = static_cast<float>(Ros.Y);
		P.Z = static_cast<float>(Ros.Z);
		Points.Add(P);
	}

	return Points;
}

TArray<uint8> FLidarPointCloud::PackPointData(const TArray<FPackedPoint>& Points)
{
	TArray<uint8> Bytes;
	Bytes.Reserve(Points.Num() * PointStepBytes);
	for (const FPackedPoint& P : Points)
	{
		AppendFloatLE(Bytes, P.X);
		AppendFloatLE(Bytes, P.Y);
		AppendFloatLE(Bytes, P.Z);
	}
	return Bytes;
}

TSharedRef<FJsonObject> FLidarPointCloud::BuildMessage(const FLidarScan& Scan, const FString& FrameId)
{
	const FString Frame = FrameId.IsEmpty() ? LidarFrameId() : FrameId;

	const TArray<FPackedPoint> Points = ComputeReturnPoints(Scan);
	const int32 Width = Points.Num();          // one point per return
	const TArray<uint8> Data = PackPointData(Points);

	// header: { stamp = scan acquisition time, frame_id = lidar sensor frame }
	TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
	Header->SetObjectField(TEXT("stamp"), MakeStamp(Scan.CaptureTime));
	Header->SetStringField(TEXT("frame_id"), Frame);

	// fields: x, y, z (float32), offsets 0, 4, 8.
	TArray<TSharedPtr<FJsonValue>> Fields;
	Fields.Add(MakePointField(TEXT("x"), 0));
	Fields.Add(MakePointField(TEXT("y"), 4));
	Fields.Add(MakePointField(TEXT("z"), 8));

	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetObjectField(TEXT("header"), Header);
	// Unordered cloud: height 1, width = return count.
	Msg->SetNumberField(TEXT("height"), 1.0);
	Msg->SetNumberField(TEXT("width"), static_cast<double>(Width));
	Msg->SetArrayField(TEXT("fields"), Fields);
	Msg->SetBoolField(TEXT("is_bigendian"), false);
	Msg->SetNumberField(TEXT("point_step"), static_cast<double>(PointStepBytes));
	Msg->SetNumberField(TEXT("row_step"), static_cast<double>(PointStepBytes * Width));
	// Binary field base64-encoded inside msg (design "Rosbridge op payloads").
	Msg->SetStringField(TEXT("data"), FRosbridgeFraming::EncodeBase64(Data));
	Msg->SetBoolField(TEXT("is_dense"), true);

	return Msg;
}

FString FLidarPointCloud::BuildPublishJson(const FLidarScan& Scan, const FString& FrameId)
{
	TSharedRef<FJsonObject> Msg = BuildMessage(Scan, FrameId);

	TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("op"), TEXT("publish"));
	Op->SetStringField(TEXT("topic"), GetTopic());
	Op->SetObjectField(TEXT("msg"), Msg);
	return JsonToString(Op);
}

FString FLidarPointCloud::BuildAdvertiseJson()
{
	return FRosbridgeFraming::BuildAdvertiseJson(GetTopic(), GetMessageType());
}
