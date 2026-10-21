// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "Sensors/CameraMessage.h"
#include "Coords/TfPublisher.h"            // SensorFrames::CameraLink
#include "Bridge/RosbridgeClient.h"        // FRosbridgeFraming::ExtractBinaryField
#include "Pipeline/SensorRng.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 14.2 (PROPERTY test).
//
// Property 7: Camera message is well-formed
//   *For any* generated frame, the built message is a JPEG
//   sensor_msgs/CompressedImage accompanied by a sensor_msgs/CameraInfo whose
//   width and height equal the configured resolution and whose intrinsics equal
//   the intrinsics used to render the frame.
//   Validates: Requirements 4.3, 4.4
//
// The design pins the exact camera message contract (design.md
// "sensor_msgs/CompressedImage + sensor_msgs/CameraInfo (camera)" message table
// and the FCameraMessage header "Intrinsics convention" / "Message layout"):
//
//     CompressedImage.header.frame_id = "camera_link"                   (4.3)
//     CompressedImage.header.stamp    = frame capture time              (4.3)
//     CompressedImage.format          = "jpeg"                          (4.3)
//     CompressedImage.data            = base64 of the JPEG bytes        (4.3)
//     CameraInfo.header.{stamp,frame_id} = same stamp + "camera_link"   (4.4)
//     CameraInfo.width / height          = configured resolution        (4.4)
//     CameraInfo.distortion_model        = "plumb_bob"                  (4.4)
//     CameraInfo.d                       = [0,0,0,0,0]                  (4.4)
//     CameraInfo.k (row-major 3x3)       = intrinsics used to render    (4.4)
//     CameraInfo.r (row-major 3x3)       = identity                     (4.4)
//     CameraInfo.p (row-major 3x4)       = intrinsics, Tx=Ty=0          (4.4)
//     fx = (W/2)/tan(hfov/2); fy = fx; cx = W/2; cy = H/2
//
// This drives the PURE builders FCameraMessage directly over seeded, generated
// frames (no world, no socket, no engine state), exactly as the other property
// tests isolate pure logic from the engine. The ONE engine-dependent step,
// EncodeRgbaToJpeg, is CPU-side libjpeg via the ImageWrapper module and runs
// under -nullrhi (the task 14.1 example test SensorSimSandbox.Camera.
// MessageBuildDecode already encodes+decodes JPEG headlessly in the same green
// suite), so each iteration encodes its own generated RGBA buffer to real JPEG
// bytes rather than feeding synthetic ones.
//
// Unreal's Automation framework has no native property generator, so a
// fixed-seed FSensorRng loop drives >= 100 iterations (design "Testing
// Strategy" / "Property-based testing"). Each iteration varies the width and
// height (within the camera resolution bound 1..7680), the RGBA pixel buffer,
// and the capture time (including the whole-second and fractional-second
// boundaries the stamp split must handle).
//
// A failing iteration is a real message-construction bug in FCameraMessage at a
// known, reproducible seed; it is NEVER a reason to lower the iteration count or
// weaken an assertion.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxCameraProperty7Test,
	"SensorSimSandbox.Camera.Property7_CameraMessageWellFormed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
{
	// Property-based tests in this project run >= 100 iterations (design
	// "Testing Strategy"). Do not lower this; a failing iteration is a real
	// message-construction bug.
	constexpr int32 NumIterations = 128;

	// Fixed base seed so the generated frames are reproducible across runs and
	// machines; distinct from the other property tests' seeds.
	constexpr uint64 FrameGeneratorBaseSeed = 0xC4A17E2F93D8560Bull;

	// Camera resolution bound (design "Camera_Model" configurable parameters:
	// width/height 1..7680). The generator stays well inside this so each
	// encode+decode is quick while still varying dimensions widely, with the
	// two edges (1 and 7680) pinned on the first iterations.
	constexpr int32 MinDim = 1;        // the documented lower bound (1..7680)
	constexpr int32 MaxGenDim = 256;   // keep per-iteration JPEG encode fast

	FSensorRng MakeGenerator(int32 Index)
	{
		return FSensorRng(FrameGeneratorBaseSeed ^ (static_cast<uint64>(Index) * 0x9E3779B97F4A7C15ull));
	}

	// A generated RGBA frame plus the metadata the message must reflect.
	struct FGeneratedFrame
	{
		int32 Width = 0;
		int32 Height = 0;
		double CaptureTime = 0.0;
		TArray<uint8> Rgba;   // row-major, 4 bytes/pixel, alpha 255
	};

	// Build a varied RGBA buffer (row-major, 4 bytes/pixel). The content is
	// seeded so the encode is reproducible; a per-pixel gradient mixed with a
	// seeded offset keeps the image non-trivially compressible (so the JPEG is
	// real content, not a flat block), while the DECODED dimensions depend only
	// on W/H, which is what the property asserts.
	void FillRgba(FSensorRng& Gen, int32 W, int32 H, TArray<uint8>& OutPx)
	{
		const uint8 R0 = static_cast<uint8>(Gen.NextUInt64() & 0xFF);
		const uint8 G0 = static_cast<uint8>(Gen.NextUInt64() & 0xFF);
		const uint8 B0 = static_cast<uint8>(Gen.NextUInt64() & 0xFF);

		OutPx.SetNumUninitialized(static_cast<int64>(W) * H * 4);
		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x = 0; x < W; ++x)
			{
				const int64 Base = (static_cast<int64>(y) * W + x) * 4;
				OutPx[Base + 0] = static_cast<uint8>((R0 + (x * 255) / FMath::Max(1, W - 1)) & 0xFF);
				OutPx[Base + 1] = static_cast<uint8>((G0 + (y * 255) / FMath::Max(1, H - 1)) & 0xFF);
				OutPx[Base + 2] = static_cast<uint8>((B0 + ((x + y) * 255) / FMath::Max(1, (W - 1) + (H - 1))) & 0xFF);
				OutPx[Base + 3] = 255;
			}
		}
	}

	FGeneratedFrame GenerateFrame(FSensorRng& Gen, int32 Iteration)
	{
		FGeneratedFrame Frame;

		// Dimensions: pin the two single-pixel / bound edges first, then vary
		// width and height independently so non-square frames (cx != cy base,
		// width != height) are exercised and the intrinsics scale per-axis.
		if (Iteration == 0)
		{
			Frame.Width = MinDim;   // 1x1 edge
			Frame.Height = MinDim;
		}
		else if (Iteration == 1)
		{
			// A 1-pixel-wide, tall strip: a highly non-square aspect that pins
			// the min-width edge. Height is capped at MaxGenDim (not the full
			// 7680 bound) so the per-iteration encode stays fast while still
			// exercising a very non-square frame.
			Frame.Width = 1;
			Frame.Height = MaxGenDim;
		}
		else if (Iteration == 2)
		{
			Frame.Width = MaxGenDim;  // wide strip, 1px tall
			Frame.Height = 1;
		}
		else
		{
			Frame.Width = MinDim + static_cast<int32>(Gen.NextUInt64() % (MaxGenDim - MinDim + 1));
			Frame.Height = MinDim + static_cast<int32>(Gen.NextUInt64() % (MaxGenDim - MinDim + 1));
		}

		// Capture time spans whole seconds, just below a second boundary, large
		// values, and small fractional values, so the {sec, nanosec} split is
		// exercised the same way the lidar/TF stamp split is.
		switch (Iteration % 4)
		{
			case 0:  Frame.CaptureTime = static_cast<double>(Iteration); break;               // whole second
			case 1:  Frame.CaptureTime = static_cast<double>(Iteration) + 0.999999999; break; // near-boundary
			case 2:  Frame.CaptureTime = static_cast<double>(Iteration) * 10.0 + Gen.NextUniform(); break;
			default: Frame.CaptureTime = Gen.NextUniform() * 1000.0; break;
		}

		FillRgba(Gen, Frame.Width, Frame.Height, Frame.Rgba);
		return Frame;
	}

	// Decode a JPEG's dimensions with the same ImageWrapper module the encoder
	// uses, so the test proves the built CompressedImage carries a decodable
	// JPEG of the generated resolution (not just any bytes).
	bool DecodeJpegDimensions(const TArray<uint8>& Jpeg, int32& OutW, int32& OutH)
	{
		OutW = 0; OutH = 0;
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::JPEG);
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Jpeg.GetData(), Jpeg.Num()))
		{
			return false;
		}
		OutW = static_cast<int32>(Wrapper->GetWidth());
		OutH = static_cast<int32>(Wrapper->GetHeight());
		return OutW > 0 && OutH > 0;
	}

	// Read a row-major number array (d/k/r/p) out of a msg object; returns false
	// if absent or any element is not a number.
	bool ReadNumberArray(const TSharedPtr<FJsonObject>& Msg, const FString& Field, TArray<double>& Out)
	{
		Out.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Msg->TryGetArrayField(Field, Arr) || Arr == nullptr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			double Num = 0.0;
			if (!V.IsValid() || !V->TryGetNumber(Num))
			{
				return false;
			}
			Out.Add(Num);
		}
		return true;
	}
}

bool FSensorSimSandboxCameraProperty7Test::RunTest(const FString& Parameters)
{
	int32 CasesChecked = 0;
	int32 CasesSquare = 0;
	int32 CasesNonSquare = 0;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FSensorRng Gen = MakeGenerator(Iteration);
		const FGeneratedFrame Frame = GenerateFrame(Gen, Iteration);
		const int32 W = Frame.Width;
		const int32 H = Frame.Height;
		if (W == H) { ++CasesSquare; } else { ++CasesNonSquare; }

		// ---- the ONE engine step: encode the generated frame to JPEG -------
		TArray<uint8> Jpeg;
		if (!FCameraMessage::EncodeRgbaToJpeg(W, H, Frame.Rgba, Jpeg) || Jpeg.Num() <= 0)
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: EncodeRgbaToJpeg failed for %dx%d."),
				Iteration, W, H));
			return false;
		}

		// Independently recompute the expected intrinsics from the generated
		// resolution and the fixed horizontal FOV, so the test does not just
		// echo the builder: fx = (W/2)/tan(hfov/2), fy = fx, cx = W/2, cy = H/2.
		const double HalfFovRad = FMath::DegreesToRadians(FCameraMessage::HorizontalFovDeg) * 0.5;
		const double ExpectFx = (static_cast<double>(W) * 0.5) / FMath::Tan(HalfFovRad);
		const double ExpectFy = ExpectFx;
		const double ExpectCx = static_cast<double>(W) * 0.5;
		const double ExpectCy = static_cast<double>(H) * 0.5;
		const double ExpectK[9] = { ExpectFx, 0.0, ExpectCx, 0.0, ExpectFy, ExpectCy, 0.0, 0.0, 1.0 };
		const double ExpectP[12] = { ExpectFx, 0.0, ExpectCx, 0.0, 0.0, ExpectFy, ExpectCy, 0.0, 0.0, 0.0, 1.0, 0.0 };
		const double ExpectR[9] = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };

		const double ExpectedStamp = FMath::Max(0.0, Frame.CaptureTime);
		constexpr double IntrinsicTol = 1.0e-6;

		// =====================================================================
		// CompressedImage op (Requirement 4.3)
		// =====================================================================
		const FString ImageOp = FCameraMessage::BuildImagePublishJson(Jpeg, Frame.CaptureTime);

		TSharedPtr<FJsonObject> ImgOp;
		{
			TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(ImageOp);
			if (!FJsonSerializer::Deserialize(Reader, ImgOp) || !ImgOp.IsValid())
			{
				AddError(FString::Printf(
					TEXT("Property 7 FAILED at iteration %d: image op is not valid JSON: %s"),
					Iteration, *ImageOp));
				return false;
			}
		}

		FString ImgOpName;
		if (!ImgOp->TryGetStringField(TEXT("op"), ImgOpName) || ImgOpName != TEXT("publish"))
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: image expected op==\"publish\", got \"%s\"."),
				Iteration, *ImgOpName));
			return false;
		}

		FString ImgTopic;
		if (!ImgOp->TryGetStringField(TEXT("topic"), ImgTopic) || ImgTopic != TEXT("/camera/image/compressed"))
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: image expected topic==\"/camera/image/compressed\", got \"%s\"."),
				Iteration, *ImgTopic));
			return false;
		}

		const TSharedPtr<FJsonObject>* ImgMsgPtr = nullptr;
		if (!ImgOp->TryGetObjectField(TEXT("msg"), ImgMsgPtr) || ImgMsgPtr == nullptr || !(*ImgMsgPtr).IsValid())
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: image op has no msg object."), Iteration));
			return false;
		}
		const TSharedPtr<FJsonObject>& ImgMsg = *ImgMsgPtr;

		// header.frame_id == camera_link
		const TSharedPtr<FJsonObject>* ImgHeaderPtr = nullptr;
		if (!ImgMsg->TryGetObjectField(TEXT("header"), ImgHeaderPtr) || ImgHeaderPtr == nullptr || !(*ImgHeaderPtr).IsValid())
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: image msg has no header object."), Iteration));
			return false;
		}
		FString ImgFrameId;
		if (!(*ImgHeaderPtr)->TryGetStringField(TEXT("frame_id"), ImgFrameId) || ImgFrameId != TEXT("camera_link"))
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: image frame_id==\"%s\", expected \"camera_link\"."),
				Iteration, *ImgFrameId));
			return false;
		}
		if (FCameraMessage::CameraFrameId() != TEXT("camera_link"))
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: camera frame id is \"%s\", expected \"camera_link\"."),
				Iteration, *FCameraMessage::CameraFrameId()));
			return false;
		}

		// header.stamp == capture time (reconstructed within 1e-6 s)
		double ImgStampSec = -1.0, ImgStampNanosec = -1.0;
		{
			const TSharedPtr<FJsonObject>* StampPtr = nullptr;
			if (!(*ImgHeaderPtr)->TryGetObjectField(TEXT("stamp"), StampPtr) || StampPtr == nullptr || !(*StampPtr).IsValid() ||
				!(*StampPtr)->TryGetNumberField(TEXT("sec"), ImgStampSec) ||
				!(*StampPtr)->TryGetNumberField(TEXT("nanosec"), ImgStampNanosec))
			{
				AddError(FString::Printf(
					TEXT("Property 7 FAILED at iteration %d: image stamp missing sec/nanosec."), Iteration));
				return false;
			}
		}
		if (ImgStampNanosec < 0.0 || ImgStampNanosec >= 1.0e9)
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: image nanosec %.1f out of [0, 1e9)."),
				Iteration, ImgStampNanosec));
			return false;
		}
		const double ImgReconstructed = ImgStampSec + ImgStampNanosec * 1.0e-9;
		if (FMath::Abs(ImgReconstructed - ExpectedStamp) > 1.0e-6)
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: image stamp %.9f != capture time %.9f."),
				Iteration, ImgReconstructed, ExpectedStamp));
			return false;
		}

		// format == "jpeg"
		FString Format;
		if (!ImgMsg->TryGetStringField(TEXT("format"), Format) || Format != TEXT("jpeg"))
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: format==\"%s\", expected \"jpeg\"."),
				Iteration, *Format));
			return false;
		}

		// data base64-decodes to EXACTLY the JPEG bytes that were encoded.
		TArray<uint8> DecodedData;
		if (!FRosbridgeFraming::ExtractBinaryField(ImageOp, TEXT("data"), DecodedData))
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: could not extract/base64-decode image msg.data."), Iteration));
			return false;
		}
		if (DecodedData != Jpeg)
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: base64-decoded data (%d bytes) != encoded JPEG (%d bytes)."),
				Iteration, DecodedData.Num(), Jpeg.Num()));
			return false;
		}

		// and the decoded JPEG's dimensions equal the frame's width/height.
		int32 DecW = 0, DecH = 0;
		if (!DecodeJpegDimensions(DecodedData, DecW, DecH))
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: round-tripped data is not a decodable JPEG."), Iteration));
			return false;
		}
		if (DecW != W || DecH != H)
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: decoded JPEG %dx%d != frame %dx%d."),
				Iteration, DecW, DecH, W, H));
			return false;
		}

		// =====================================================================
		// CameraInfo op (Requirement 4.4)
		// =====================================================================
		const FString InfoOp = FCameraMessage::BuildCameraInfoPublishJson(W, H, Frame.CaptureTime);

		TSharedPtr<FJsonObject> CiOp;
		{
			TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(InfoOp);
			if (!FJsonSerializer::Deserialize(Reader, CiOp) || !CiOp.IsValid())
			{
				AddError(FString::Printf(
					TEXT("Property 7 FAILED at iteration %d: info op is not valid JSON: %s"),
					Iteration, *InfoOp));
				return false;
			}
		}

		FString CiOpName;
		if (!CiOp->TryGetStringField(TEXT("op"), CiOpName) || CiOpName != TEXT("publish"))
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: info expected op==\"publish\", got \"%s\"."),
				Iteration, *CiOpName));
			return false;
		}

		FString CiTopic;
		if (!CiOp->TryGetStringField(TEXT("topic"), CiTopic) || CiTopic != TEXT("/camera/camera_info"))
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: info expected topic==\"/camera/camera_info\", got \"%s\"."),
				Iteration, *CiTopic));
			return false;
		}

		const TSharedPtr<FJsonObject>* CiMsgPtr = nullptr;
		if (!CiOp->TryGetObjectField(TEXT("msg"), CiMsgPtr) || CiMsgPtr == nullptr || !(*CiMsgPtr).IsValid())
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: info op has no msg object."), Iteration));
			return false;
		}
		const TSharedPtr<FJsonObject>& CiMsg = *CiMsgPtr;

		// header.frame_id == camera_link, stamp == same frame stamp
		const TSharedPtr<FJsonObject>* CiHeaderPtr = nullptr;
		if (!CiMsg->TryGetObjectField(TEXT("header"), CiHeaderPtr) || CiHeaderPtr == nullptr || !(*CiHeaderPtr).IsValid())
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: info msg has no header object."), Iteration));
			return false;
		}
		FString CiFrameId;
		if (!(*CiHeaderPtr)->TryGetStringField(TEXT("frame_id"), CiFrameId) || CiFrameId != TEXT("camera_link"))
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: info frame_id==\"%s\", expected \"camera_link\"."),
				Iteration, *CiFrameId));
			return false;
		}

		double CiStampSec = -1.0, CiStampNanosec = -1.0;
		{
			const TSharedPtr<FJsonObject>* StampPtr = nullptr;
			if (!(*CiHeaderPtr)->TryGetObjectField(TEXT("stamp"), StampPtr) || StampPtr == nullptr || !(*StampPtr).IsValid() ||
				!(*StampPtr)->TryGetNumberField(TEXT("sec"), CiStampSec) ||
				!(*StampPtr)->TryGetNumberField(TEXT("nanosec"), CiStampNanosec))
			{
				AddError(FString::Printf(
					TEXT("Property 7 FAILED at iteration %d: info stamp missing sec/nanosec."), Iteration));
				return false;
			}
		}
		const double CiReconstructed = CiStampSec + CiStampNanosec * 1.0e-9;
		if (FMath::Abs(CiReconstructed - ExpectedStamp) > 1.0e-6)
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: info stamp %.9f != capture time %.9f."),
				Iteration, CiReconstructed, ExpectedStamp));
			return false;
		}
		// image and camera_info stamps match (same frame).
		if (FMath::Abs(CiReconstructed - ImgReconstructed) > 1.0e-9)
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: info stamp %.9f != image stamp %.9f (same frame)."),
				Iteration, CiReconstructed, ImgReconstructed));
			return false;
		}

		// width == config width, height == config height
		double MW = -1.0, MH = -1.0;
		CiMsg->TryGetNumberField(TEXT("width"), MW);
		CiMsg->TryGetNumberField(TEXT("height"), MH);
		if (static_cast<int32>(MW) != W || static_cast<int32>(MH) != H)
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: CameraInfo %.0fx%.0f != config %dx%d."),
				Iteration, MW, MH, W, H));
			return false;
		}

		// distortion_model == "plumb_bob"
		FString Dist;
		if (!CiMsg->TryGetStringField(TEXT("distortion_model"), Dist) || Dist != TEXT("plumb_bob"))
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: distortion_model==\"%s\", expected \"plumb_bob\"."),
				Iteration, *Dist));
			return false;
		}

		// D all zero (5 entries)
		TArray<double> D;
		if (!ReadNumberArray(CiMsg, TEXT("d"), D) || D.Num() != 5)
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: CameraInfo D must have 5 numeric entries (got %d)."),
				Iteration, D.Num()));
			return false;
		}
		for (int32 i = 0; i < 5; ++i)
		{
			if (D[i] != 0.0)
			{
				AddError(FString::Printf(
					TEXT("Property 7 FAILED at iteration %d: D[%d]==%g, expected 0 (pinhole render)."),
					Iteration, i, D[i]));
				return false;
			}
		}

		// K equals the independently recomputed intrinsics (row-major 3x3)
		TArray<double> K;
		if (!ReadNumberArray(CiMsg, TEXT("k"), K) || K.Num() != 9)
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: CameraInfo K must have 9 numeric entries (got %d)."),
				Iteration, K.Num()));
			return false;
		}
		for (int32 i = 0; i < 9; ++i)
		{
			if (FMath::Abs(K[i] - ExpectK[i]) > IntrinsicTol)
			{
				AddError(FString::Printf(
					TEXT("Property 7 FAILED at iteration %d: K[%d]==%.9g != recomputed %.9g (fx=(W/2)/tan(hfov/2), %dx%d)."),
					Iteration, i, K[i], ExpectK[i], W, H));
				return false;
			}
		}

		// R == identity (row-major 3x3)
		TArray<double> R;
		if (!ReadNumberArray(CiMsg, TEXT("r"), R) || R.Num() != 9)
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: CameraInfo R must have 9 numeric entries (got %d)."),
				Iteration, R.Num()));
			return false;
		}
		for (int32 i = 0; i < 9; ++i)
		{
			if (FMath::Abs(R[i] - ExpectR[i]) > 1.0e-9)
			{
				AddError(FString::Printf(
					TEXT("Property 7 FAILED at iteration %d: R[%d]==%.9g != identity %.9g."),
					Iteration, i, R[i], ExpectR[i]));
				return false;
			}
		}

		// P equals the independently recomputed intrinsics (row-major 3x4, Tx=Ty=0)
		TArray<double> P;
		if (!ReadNumberArray(CiMsg, TEXT("p"), P) || P.Num() != 12)
		{
			AddError(FString::Printf(
				TEXT("Property 7 FAILED at iteration %d: CameraInfo P must have 12 numeric entries (got %d)."),
				Iteration, P.Num()));
			return false;
		}
		for (int32 i = 0; i < 12; ++i)
		{
			if (FMath::Abs(P[i] - ExpectP[i]) > IntrinsicTol)
			{
				AddError(FString::Printf(
					TEXT("Property 7 FAILED at iteration %d: P[%d]==%.9g != recomputed %.9g."),
					Iteration, i, P[i], ExpectP[i]));
				return false;
			}
		}

		++CasesChecked;
	}

	// Guard the generator itself: it must have produced >= 100 checked cases and
	// exercised both square and non-square resolutions (so the per-axis cx/cy
	// and width != height paths are covered), as the design calls out.
	TestTrue(TEXT("well-formedness checked at least 100 generated frames"), CasesChecked >= 100);
	TestTrue(TEXT("at least one generated frame was square"), CasesSquare >= 1);
	TestTrue(TEXT("at least one generated frame was non-square"), CasesNonSquare >= 1);
	return true;
}
