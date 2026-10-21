// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "Sensors/LidarPointCloud.h"
#include "Sensors/LidarPipelineRunner.h"   // FLidarScan, FLidarRayDir, FSensorValue
#include "Bridge/RosbridgeClient.h"         // FRosbridgeFraming::DecodeBase64 / ExtractBinaryField
#include "Coords/Rep103.h"                  // FRep103::ConvertPosition (expected points)
#include "Pipeline/SensorRng.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 12.2 (PROPERTY test).
//
// Property 6: Lidar message is well-formed
//   *For any* generated scan, the built message is a sensor_msgs/PointCloud2
//   whose point count matches the scan, whose frame_id equals the lidar sensor
//   frame, and whose timestamp equals the scan acquisition time.
//   Validates: Requirements 3.3, 3.4
//
// The design pins the exact PointCloud2 contract (design.md "ROS message
// contracts" -> sensor_msgs/PointCloud2 (lidar), and the FLidarPointCloud
// header "Message layout"):
//
//     header.frame_id = "lidar_link" (the lidar sensor frame)          (3.4)
//     header.stamp    = scan acquisition (capture) time                (3.4)
//     fields          = x, y, z, each FLOAT32 (datatype 7), count 1,
//                       offsets 0, 4, 8
//     point_step      = 12 (three float32)
//     height          = 1   (unordered cloud)
//     width           = number of returns in the scan                  (3.3)
//     row_step        = point_step * width
//     is_bigendian    = false   (bytes packed little-endian)
//     is_dense        = true
//     data            = width points, each [x,y,z] little-endian float32,
//                       base64-encoded on the rosbridge wire            (3.3)
//
// This drives the PURE builder FLidarPointCloud directly over seeded, generated
// scans (no world, no socket, no engine state), exactly as the other property
// tests isolate pure logic from the engine. Unreal's Automation framework has
// no native property generator, so a fixed-seed FSensorRng loop drives >= 100
// iterations (design "Testing Strategy" / "Property-based testing"). Each
// iteration varies the ray count, the mix of hits / no-hits / dropped rays (so
// the RETURN count varies independently of the ray count), and the capture
// time (including the whole-second and fractional-second boundaries the stamp
// split must handle).
//
// A failing iteration is a real message-construction bug in FLidarPointCloud at
// a known, reproducible seed; it is NEVER a reason to lower the iteration count
// or weaken an assertion.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxLidarProperty6Test,
	"SensorSimSandbox.Lidar.Property6_PointCloud2WellFormed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
{
	// Property-based tests in this project run >= 100 iterations (design
	// "Testing Strategy"). Do not lower this; a failing iteration is a real
	// message-construction bug.
	constexpr int32 NumIterations = 128;

	// Fixed base seed so the generated scans are reproducible across runs and
	// machines; distinct from the other property tests' seeds.
	constexpr uint64 ScanGeneratorBaseSeed = 0x6D1F8B3A5C27E904ull;

	// The design's fixed PointCloud2 constants, restated here so the test fails
	// if a builder change silently drifts from the contract.
	constexpr int32 ExpectedPointStep = 12;
	constexpr int32 ExpectedFloat32 = 7;              // sensor_msgs/PointField::FLOAT32
	constexpr float FloatMatchTolerance = 0.0f;        // exact: float packed, float compared

	FSensorRng MakeGenerator(int32 Index)
	{
		return FSensorRng(ScanGeneratorBaseSeed ^ (static_cast<uint64>(Index) * 0x9E3779B97F4A7C15ull));
	}

	// A ray's role in a generated scan, so RETURN count varies independently of
	// ray count: a HIT ray has a published range (a return); a DROPPED ray hit
	// but was dropped (truth set, published empty); a MISS ray never hit (both
	// empty). Only HIT rays become points, so width == number of HIT rays.
	enum class ERayRole : uint8 { Hit, Dropped, Miss };

	// A unit direction on the sphere from two uniforms, so points land all
	// around the sensor (not just the forward hemisphere) and the REP 103
	// Y-flip is exercised with both signs.
	FVector RandomUnitDirection(FSensorRng& Gen)
	{
		const double Z = 2.0 * Gen.NextUniform() - 1.0;          // cos(polar) in [-1, 1]
		const double Azimuth = 2.0 * PI * Gen.NextUniform();     // [0, 2pi)
		const double R = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
		FVector Dir(R * FMath::Cos(Azimuth), R * FMath::Sin(Azimuth), Z);
		if (!Dir.Normalize())
		{
			Dir = FVector::ForwardVector;
		}
		return Dir;
	}

	// Build a generated scan for iteration `Iteration`. The scan has
	// Rays/TruthRangeM/PublishedRangeM/bDropped all sized to the same ray count
	// (as the live pipeline produces), with a varied mix of hit / dropped / miss
	// rays and a varied capture time. Iteration 0 pins the empty-cloud edge
	// case (zero rays -> zero returns); iteration 1 pins an all-miss scan
	// (rays present, zero returns) so width == 0 is covered with rays present.
	FLidarScan GenerateScan(FSensorRng& Gen, int32 Iteration)
	{
		FLidarScan Scan;
		Scan.Seq = Iteration;

		// Capture time spans whole seconds, just-below and just-above a second
		// boundary, and large values, so the {sec, nanosec} split is exercised.
		switch (Iteration % 4)
		{
			case 0:  Scan.CaptureTime = static_cast<double>(Iteration); break;               // whole second
			case 1:  Scan.CaptureTime = static_cast<double>(Iteration) + 0.999999999; break; // near-boundary
			case 2:  Scan.CaptureTime = static_cast<double>(Iteration) * 10.0 + Gen.NextUniform(); break;
			default: Scan.CaptureTime = Gen.NextUniform() * 1000.0; break;
		}

		int32 NumRays = 0;
		if (Iteration == 0)
		{
			NumRays = 0; // empty cloud: no rays at all
		}
		else if (Iteration == 1)
		{
			NumRays = 1 + static_cast<int32>(Gen.NextUInt64() % 32); // all-miss below
		}
		else
		{
			// 1 .. 256 rays so single-ray, small and larger clouds are covered.
			NumRays = 1 + static_cast<int32>(Gen.NextUInt64() % 256);
		}

		Scan.Rays.Reserve(NumRays);
		Scan.TruthRangeM.Reserve(NumRays);
		Scan.PublishedRangeM.Reserve(NumRays);
		Scan.bDropped.Reserve(NumRays);

		for (int32 i = 0; i < NumRays; ++i)
		{
			FLidarRayDir Ray;
			Ray.Direction = RandomUnitDirection(Gen);
			Ray.Channel = i;
			Ray.AzimuthIndex = i;
			Scan.Rays.Add(Ray);

			// Choose this ray's role. Iteration 1 forces every ray to miss so a
			// rays-present / zero-return scan is covered.
			ERayRole Role;
			if (Iteration == 1)
			{
				Role = ERayRole::Miss;
			}
			else
			{
				const double U = Gen.NextUniform();
				Role = (U < 0.70) ? ERayRole::Hit : ((U < 0.85) ? ERayRole::Dropped : ERayRole::Miss);
			}

			// Range in [0.1, 1000] m, the lidar max_range bound span.
			const double RangeM = 0.1 + Gen.NextUniform() * 999.9;

			switch (Role)
			{
				case ERayRole::Hit:
					Scan.TruthRangeM.Add(FSensorValue(RangeM));
					Scan.PublishedRangeM.Add(FSensorValue(RangeM));
					Scan.bDropped.Add(false);
					break;
				case ERayRole::Dropped:
					Scan.TruthRangeM.Add(FSensorValue(RangeM)); // hit, but dropped
					Scan.PublishedRangeM.Add(FSensorValue());   // excluded from cloud
					Scan.bDropped.Add(true);
					break;
				case ERayRole::Miss:
				default:
					Scan.TruthRangeM.Add(FSensorValue()); // no hit
					Scan.PublishedRangeM.Add(FSensorValue());
					Scan.bDropped.Add(false);
					break;
			}
		}

		return Scan;
	}
}

bool FSensorSimSandboxLidarProperty6Test::RunTest(const FString& Parameters)
{
	int32 CasesChecked = 0;
	int32 CasesWithReturns = 0;
	int32 CasesWithZeroReturns = 0;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FSensorRng Gen = MakeGenerator(Iteration);
		const FLidarScan Scan = GenerateScan(Gen, Iteration);

		// The number of RETURNS the message must represent: hit rays whose
		// published range is set, which is exactly FLidarScan::NumReturns().
		const int32 ExpectedWidth = Scan.NumReturns();
		if (ExpectedWidth > 0) { ++CasesWithReturns; } else { ++CasesWithZeroReturns; }

		// Independently recompute the expected REP 103 points from the scan, in
		// return order, so the test does not just echo the builder: a point is
		// the ray direction scaled by the published range (converted to cm) and
		// run through the FIXED REP 103 converter. These must match the bytes
		// the message carries.
		TArray<FLidarPointCloud::FPackedPoint> Expected;
		Expected.Reserve(ExpectedWidth);
		for (int32 i = 0; i < Scan.PublishedRangeM.Num(); ++i)
		{
			if (!Scan.PublishedRangeM[i].IsSet() || !Scan.Rays.IsValidIndex(i))
			{
				continue;
			}
			const double RangeM = Scan.PublishedRangeM[i].GetValue();
			const FVector PointUeCm = Scan.Rays[i].Direction * (RangeM * 100.0);
			const FRep103Position Ros = FRep103::ConvertPosition(PointUeCm);
			FLidarPointCloud::FPackedPoint P;
			P.X = static_cast<float>(Ros.X);
			P.Y = static_cast<float>(Ros.Y);
			P.Z = static_cast<float>(Ros.Z);
			Expected.Add(P);
		}

		// Build the full rosbridge publish op the live client would send.
		const FString PublishOp = FLidarPointCloud::BuildPublishJson(Scan);

		// ---- parse the op as JSON and verify op/topic (3.3) ----------------
		TSharedPtr<FJsonObject> Op;
		{
			TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(PublishOp);
			if (!FJsonSerializer::Deserialize(Reader, Op) || !Op.IsValid())
			{
				AddError(FString::Printf(
					TEXT("Property 6 FAILED at iteration %d: publish op is not valid JSON: %s"),
					Iteration, *PublishOp));
				return false;
			}
		}

		FString OpName;
		if (!Op->TryGetStringField(TEXT("op"), OpName) || OpName != TEXT("publish"))
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: expected op==\"publish\", got \"%s\"."),
				Iteration, *OpName));
			return false;
		}

		FString TopicName;
		if (!Op->TryGetStringField(TEXT("topic"), TopicName) || TopicName != TEXT("/lidar/points"))
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: expected topic==\"/lidar/points\", got \"%s\"."),
				Iteration, *TopicName));
			return false;
		}

		const TSharedPtr<FJsonObject>* MsgPtr = nullptr;
		if (!Op->TryGetObjectField(TEXT("msg"), MsgPtr) || MsgPtr == nullptr || !(*MsgPtr).IsValid())
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: publish op has no msg object."), Iteration));
			return false;
		}
		const TSharedPtr<FJsonObject>& Msg = *MsgPtr;

		// ---- header.frame_id == lidar sensor frame (3.4) -------------------
		const TSharedPtr<FJsonObject>* HeaderPtr = nullptr;
		if (!Msg->TryGetObjectField(TEXT("header"), HeaderPtr) || HeaderPtr == nullptr || !(*HeaderPtr).IsValid())
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: msg has no header object."), Iteration));
			return false;
		}
		const TSharedPtr<FJsonObject>& Header = *HeaderPtr;

		FString FrameId;
		if (!Header->TryGetStringField(TEXT("frame_id"), FrameId) || FrameId != FLidarPointCloud::LidarFrameId())
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: frame_id==\"%s\" != lidar frame \"%s\"."),
				Iteration, *FrameId, *FLidarPointCloud::LidarFrameId()));
			return false;
		}
		if (FLidarPointCloud::LidarFrameId() != TEXT("lidar_link"))
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: lidar frame id is \"%s\", expected \"lidar_link\"."),
				Iteration, *FLidarPointCloud::LidarFrameId()));
			return false;
		}

		// ---- header.stamp == scan acquisition time (3.4) ------------------
		// The capture time split into {sec, nanosec}; reconstruct and compare
		// to the scan's CaptureTime within 1 ns (the split's own rounding).
		const TSharedPtr<FJsonObject>* StampPtr = nullptr;
		if (!Header->TryGetObjectField(TEXT("stamp"), StampPtr) || StampPtr == nullptr || !(*StampPtr).IsValid())
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: header has no stamp object."), Iteration));
			return false;
		}
		const TSharedPtr<FJsonObject>& Stamp = *StampPtr;

		double StampSec = -1.0;
		double StampNanosec = -1.0;
		if (!Stamp->TryGetNumberField(TEXT("sec"), StampSec) ||
			!Stamp->TryGetNumberField(TEXT("nanosec"), StampNanosec))
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: stamp missing sec/nanosec."), Iteration));
			return false;
		}
		if (StampNanosec < 0.0 || StampNanosec >= 1.0e9)
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: nanosec %.1f out of [0, 1e9)."),
				Iteration, StampNanosec));
			return false;
		}
		const double ReconstructedTime = StampSec + StampNanosec * 1.0e-9;
		const double ExpectedTime = FMath::Max(0.0, Scan.CaptureTime);
		if (FMath::Abs(ReconstructedTime - ExpectedTime) > 1.0e-6)
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: stamp %.9f != capture time %.9f."),
				Iteration, ReconstructedTime, ExpectedTime));
			return false;
		}

		// ---- height / width / steps / flags (3.3) -------------------------
		double Height = -1.0, Width = -1.0, PointStep = -1.0, RowStep = -1.0;
		Msg->TryGetNumberField(TEXT("height"), Height);
		Msg->TryGetNumberField(TEXT("width"), Width);
		Msg->TryGetNumberField(TEXT("point_step"), PointStep);
		Msg->TryGetNumberField(TEXT("row_step"), RowStep);

		if (static_cast<int32>(Height) != 1)
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: height==%.0f, expected 1."), Iteration, Height));
			return false;
		}
		if (static_cast<int32>(Width) != ExpectedWidth)
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: width==%.0f != return count %d."),
				Iteration, Width, ExpectedWidth));
			return false;
		}
		if (static_cast<int32>(PointStep) != ExpectedPointStep)
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: point_step==%.0f, expected %d."),
				Iteration, PointStep, ExpectedPointStep));
			return false;
		}
		if (static_cast<int32>(RowStep) != ExpectedPointStep * ExpectedWidth)
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: row_step==%.0f, expected point_step*width=%d."),
				Iteration, RowStep, ExpectedPointStep * ExpectedWidth));
			return false;
		}

		bool bBigEndian = true, bDense = false;
		if (!Msg->TryGetBoolField(TEXT("is_bigendian"), bBigEndian) || bBigEndian)
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: is_bigendian must be false."), Iteration));
			return false;
		}
		if (!Msg->TryGetBoolField(TEXT("is_dense"), bDense) || !bDense)
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: is_dense must be true."), Iteration));
			return false;
		}

		// ---- fields are exactly x, y, z FLOAT32 at offsets 0, 4, 8 --------
		const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
		if (!Msg->TryGetArrayField(TEXT("fields"), Fields) || Fields == nullptr || Fields->Num() != 3)
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: fields must have exactly 3 entries."), Iteration));
			return false;
		}
		const TCHAR* ExpectedNames[3] = { TEXT("x"), TEXT("y"), TEXT("z") };
		const int32 ExpectedOffsets[3] = { 0, 4, 8 };
		for (int32 f = 0; f < 3; ++f)
		{
			const TSharedPtr<FJsonObject>* FieldObj = nullptr;
			if (!(*Fields)[f].IsValid() || !(*Fields)[f]->TryGetObject(FieldObj) || FieldObj == nullptr)
			{
				AddError(FString::Printf(
					TEXT("Property 6 FAILED at iteration %d: field %d is not an object."), Iteration, f));
				return false;
			}
			FString FName;
			double FOffset = -1.0, FDatatype = -1.0, FCount = -1.0;
			(*FieldObj)->TryGetStringField(TEXT("name"), FName);
			(*FieldObj)->TryGetNumberField(TEXT("offset"), FOffset);
			(*FieldObj)->TryGetNumberField(TEXT("datatype"), FDatatype);
			(*FieldObj)->TryGetNumberField(TEXT("count"), FCount);
			if (FName != ExpectedNames[f] ||
				static_cast<int32>(FOffset) != ExpectedOffsets[f] ||
				static_cast<int32>(FDatatype) != ExpectedFloat32 ||
				static_cast<int32>(FCount) != 1)
			{
				AddError(FString::Printf(
					TEXT("Property 6 FAILED at iteration %d: field %d is {name=%s, offset=%.0f, datatype=%.0f, count=%.0f}, ")
					TEXT("expected {name=%s, offset=%d, datatype=%d, count=1}."),
					Iteration, f, *FName, FOffset, FDatatype, FCount,
					ExpectedNames[f], ExpectedOffsets[f], ExpectedFloat32));
				return false;
			}
		}

		// ---- data: base64 decodes to width*12 bytes and round-trips (3.3) --
		TArray<uint8> DecodedData;
		if (!FRosbridgeFraming::ExtractBinaryField(PublishOp, TEXT("data"), DecodedData))
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: could not extract/base64-decode msg.data."), Iteration));
			return false;
		}
		if (DecodedData.Num() != ExpectedWidth * ExpectedPointStep)
		{
			AddError(FString::Printf(
				TEXT("Property 6 FAILED at iteration %d: data is %d bytes, expected width*point_step=%d."),
				Iteration, DecodedData.Num(), ExpectedWidth * ExpectedPointStep));
			return false;
		}

		// Decode each point's three little-endian float32 and require them to
		// reproduce the independently computed REP 103 points exactly (same
		// float packing on both sides).
		for (int32 p = 0; p < ExpectedWidth; ++p)
		{
			float Xyz[3] = { 0.0f, 0.0f, 0.0f };
			for (int32 c = 0; c < 3; ++c)
			{
				const int32 Base = p * ExpectedPointStep + c * 4;
				const uint32 Bits =
					static_cast<uint32>(DecodedData[Base + 0]) |
					(static_cast<uint32>(DecodedData[Base + 1]) << 8) |
					(static_cast<uint32>(DecodedData[Base + 2]) << 16) |
					(static_cast<uint32>(DecodedData[Base + 3]) << 24);
				FMemory::Memcpy(&Xyz[c], &Bits, sizeof(float));
			}

			const FLidarPointCloud::FPackedPoint& E = Expected[p];
			if (FMath::Abs(Xyz[0] - E.X) > FloatMatchTolerance ||
				FMath::Abs(Xyz[1] - E.Y) > FloatMatchTolerance ||
				FMath::Abs(Xyz[2] - E.Z) > FloatMatchTolerance)
			{
				AddError(FString::Printf(
					TEXT("Property 6 FAILED at iteration %d: point %d decoded (%g, %g, %g) != expected REP 103 (%g, %g, %g)."),
					Iteration, p, Xyz[0], Xyz[1], Xyz[2], E.X, E.Y, E.Z));
				return false;
			}
		}

		++CasesChecked;
	}

	// Guard the generator itself: it must have produced >= 100 checked cases,
	// at least one scan with returns, and at least one zero-return scan, so the
	// property is exercised across the full range the design calls out.
	TestTrue(TEXT("well-formedness checked at least 100 generated scans"), CasesChecked >= 100);
	TestTrue(TEXT("at least one generated scan had returns"), CasesWithReturns >= 1);
	TestTrue(TEXT("at least one generated scan had zero returns"), CasesWithZeroReturns >= 1);
	return true;
}
