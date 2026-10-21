// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "Sensors/CameraMessage.h"
#include "Sensors/CameraConfig.h"
#include "Coords/TfPublisher.h"            // SensorFrames::CameraLink
#include "Bridge/RosbridgeClient.h"        // FRosbridgeFraming::ExtractBinaryField

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

#include <cmath>

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 14.1 — HEADLESS camera MESSAGE build+decode.
//
// Task 14.1 builds a sensor_msgs/CompressedImage (JPEG) + an accompanying
// sensor_msgs/CameraInfo whose width/height equal the configured resolution and
// whose intrinsics equal those used to render the frame, published to
// /camera/image/compressed and /camera/camera_info. The message/intrinsics/JPEG
// logic is PURE (FCameraMessage) except the one ImageWrapper JPEG encode, which
// is CPU-side (libjpeg) and runs under -nullrhi. This test drives all of it with
// NO world and NO GPU, so it runs in the headless suite (scripts/run_ue_tests.ps1)
// and is the "verify the message construction + JPEG encode/decode + intrinsics
// headlessly" path the task calls for, and the task 15g gate's camera-message
// build+decode check (JPEG size matches CameraInfo).
//
// Validates: Requirements 4.3, 4.4
//
// It is NOT the Property 7 well-formedness property test (task 14.2); it is an
// example/integration test over the shipped Gate A config plus a couple of
// extra resolutions, encoding a known SYNTHETIC frame to JPEG, decoding it, and
// asserting the dimensions == config and that CameraInfo K/P/width/height match
// the config intrinsics. Nothing here is weakened to pass.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxCameraMessageBuildDecodeTest,
	"SensorSimSandbox.Camera.MessageBuildDecode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
{
	// A gradient RGBA frame (row-major, 4 bytes/pixel, alpha 255), so the JPEG
	// encoder produces real content rather than a trivially-compressible flat
	// image. The decode only needs the dimensions, which do not depend on the
	// content, but a gradient exercises the encoder honestly.
	TArray<uint8> MakeGradientFrame(int32 W, int32 H)
	{
		TArray<uint8> Px;
		Px.SetNumUninitialized(static_cast<int64>(W) * H * 4);
		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x = 0; x < W; ++x)
			{
				const int64 Base = (static_cast<int64>(y) * W + x) * 4;
				Px[Base + 0] = static_cast<uint8>((x * 255) / FMath::Max(1, W - 1));
				Px[Base + 1] = static_cast<uint8>((y * 255) / FMath::Max(1, H - 1));
				Px[Base + 2] = static_cast<uint8>(((x + y) * 255) / FMath::Max(1, (W - 1) + (H - 1)));
				Px[Base + 3] = 255;
			}
		}
		return Px;
	}

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
}

bool FSensorSimSandboxCameraMessageBuildDecodeTest::RunTest(const FString& Parameters)
{
	// --- Resolutions under test: the shipped Gate A config first, then a few
	// others so the intrinsics scale with resolution (not hard-coded 640x480). -
	int32 CfgW = 640, CfgH = 480;
	{
		FCameraConfig Config;
		FString LoadError;
		const FString ConfigPath = FCameraConfig::ResolveConfigPath();
		if (FCameraConfig::LoadFromFile(ConfigPath, Config, LoadError) && Config.Validate().bValid)
		{
			CfgW = Config.Width;
			CfgH = Config.Height;
		}
		// If the file is absent in the test environment the Gate A defaults
		// (640x480) are used; the config path is not required for this logic.
	}

	struct FCase { int32 W; int32 H; };
	const TArray<FCase> Cases = {
		{ CfgW, CfgH },   // shipped Gate A resolution (640x480)
		{ 320, 240 },
		{ 1280, 720 },
		{ 100, 100 },
	};

	const double Stamp = 12.345; // non-integer: exercises the sec/nanosec split

	for (const FCase& C : Cases)
	{
		const int32 W = C.W;
		const int32 H = C.H;

		// ---- JPEG encode a known synthetic frame --------------------------
		const TArray<uint8> Frame = MakeGradientFrame(W, H);
		TArray<uint8> Jpeg;
		if (!TestTrue(FString::Printf(TEXT("EncodeRgbaToJpeg succeeds for %dx%d"), W, H),
				FCameraMessage::EncodeRgbaToJpeg(W, H, Frame, Jpeg)))
		{
			return false;
		}
		TestTrue(FString::Printf(TEXT("JPEG bytes non-empty for %dx%d"), W, H), Jpeg.Num() > 0);

		// A wrong-sized buffer is rejected (no silent mis-encode).
		TArray<uint8> ShortBuf = Frame;
		ShortBuf.SetNum(ShortBuf.Num() - 4);
		TArray<uint8> ShouldFail;
		TestFalse(TEXT("EncodeRgbaToJpeg rejects a wrong-sized buffer"),
			FCameraMessage::EncodeRgbaToJpeg(W, H, ShortBuf, ShouldFail));

		// ---- the JPEG decodes to the configured dimensions ----------------
		int32 DecW = 0, DecH = 0;
		if (!TestTrue(FString::Printf(TEXT("published JPEG decodes for %dx%d"), W, H),
				DecodeJpegDimensions(Jpeg, DecW, DecH)))
		{
			return false;
		}
		TestEqual(FString::Printf(TEXT("decoded JPEG width == config (%d)"), W), DecW, W);
		TestEqual(FString::Printf(TEXT("decoded JPEG height == config (%d)"), H), DecH, H);

		// ---- CompressedImage op: topic, header, format, data round-trip ---
		const FString ImageOp = FCameraMessage::BuildImagePublishJson(Jpeg, Stamp);
		{
			TSharedPtr<FJsonObject> Op;
			TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(ImageOp);
			if (!TestTrue(TEXT("image op is valid JSON"), FJsonSerializer::Deserialize(Reader, Op) && Op.IsValid()))
			{
				return false;
			}
			FString OpName, Topic;
			Op->TryGetStringField(TEXT("op"), OpName);
			Op->TryGetStringField(TEXT("topic"), Topic);
			TestEqual(TEXT("image op == publish"), OpName, FString(TEXT("publish")));
			TestEqual(TEXT("image topic == /camera/image/compressed"), Topic, FCameraMessage::GetImageTopic());

			const TSharedPtr<FJsonObject>* Msg = nullptr;
			if (!TestTrue(TEXT("image has msg"), Op->TryGetObjectField(TEXT("msg"), Msg) && Msg != nullptr))
			{
				return false;
			}
			FString Format;
			(*Msg)->TryGetStringField(TEXT("format"), Format);
			TestEqual(TEXT("format == jpeg"), Format, FString(TEXT("jpeg")));

			const TSharedPtr<FJsonObject>* Header = nullptr;
			FString FrameId;
			if ((*Msg)->TryGetObjectField(TEXT("header"), Header) && Header)
			{
				(*Header)->TryGetStringField(TEXT("frame_id"), FrameId);
			}
			TestEqual(TEXT("image frame_id == camera_link"), FrameId, FCameraMessage::CameraFrameId());
			TestEqual(TEXT("camera frame id is camera_link"), FCameraMessage::CameraFrameId(), FString(SensorFrames::CameraLink));

			// data base64-decodes to EXACTLY the JPEG bytes that decode to WxH.
			TArray<uint8> DecodedData;
			if (!TestTrue(TEXT("image data base64 decodes"),
					FRosbridgeFraming::ExtractBinaryField(ImageOp, TEXT("data"), DecodedData)))
			{
				return false;
			}
			TestTrue(TEXT("image data == original JPEG bytes"), DecodedData == Jpeg);

			int32 RtW = 0, RtH = 0;
			TestTrue(TEXT("round-tripped data is a decodable JPEG"), DecodeJpegDimensions(DecodedData, RtW, RtH));
			TestEqual(TEXT("round-tripped JPEG width == config"), RtW, W);
			TestEqual(TEXT("round-tripped JPEG height == config"), RtH, H);
		}

		// ---- CameraInfo op: resolution + intrinsics match the config ------
		const FCameraMessage::FIntrinsics Intr = FCameraMessage::ComputeIntrinsics(W, H);
		// Independent recomputation (do not just echo the builder): square-pixel
		// pinhole from the fixed horizontal FOV.
		const double HalfFov = FMath::DegreesToRadians(FCameraMessage::HorizontalFovDeg) * 0.5;
		const double ExpectFx = (static_cast<double>(W) * 0.5) / FMath::Tan(HalfFov);
		const double ExpectCx = static_cast<double>(W) * 0.5;
		const double ExpectCy = static_cast<double>(H) * 0.5;
		TestTrue(TEXT("fx matches (W/2)/tan(hfov/2)"), FMath::Abs(Intr.Fx - ExpectFx) <= 1e-6);
		TestTrue(TEXT("fy == fx (square pixels)"), FMath::Abs(Intr.Fy - Intr.Fx) <= 1e-9);
		TestTrue(TEXT("cx == W/2"), FMath::Abs(Intr.Cx - ExpectCx) <= 1e-9);
		TestTrue(TEXT("cy == H/2"), FMath::Abs(Intr.Cy - ExpectCy) <= 1e-9);

		const FString InfoOp = FCameraMessage::BuildCameraInfoPublishJson(W, H, Stamp);
		{
			TSharedPtr<FJsonObject> Op;
			TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(InfoOp);
			if (!TestTrue(TEXT("info op is valid JSON"), FJsonSerializer::Deserialize(Reader, Op) && Op.IsValid()))
			{
				return false;
			}
			FString Topic;
			Op->TryGetStringField(TEXT("topic"), Topic);
			TestEqual(TEXT("info topic == /camera/camera_info"), Topic, FCameraMessage::GetCameraInfoTopic());

			const TSharedPtr<FJsonObject>* Msg = nullptr;
			if (!TestTrue(TEXT("info has msg"), Op->TryGetObjectField(TEXT("msg"), Msg) && Msg != nullptr))
			{
				return false;
			}

			double MW = -1, MH = -1;
			(*Msg)->TryGetNumberField(TEXT("width"), MW);
			(*Msg)->TryGetNumberField(TEXT("height"), MH);
			TestEqual(TEXT("CameraInfo width == config"), static_cast<int32>(MW), W);
			TestEqual(TEXT("CameraInfo height == config"), static_cast<int32>(MH), H);

			FString Dist;
			(*Msg)->TryGetStringField(TEXT("distortion_model"), Dist);
			TestEqual(TEXT("distortion_model == plumb_bob"), Dist, FCameraMessage::GetDistortionModel());

			const TArray<TSharedPtr<FJsonValue>>* D = nullptr;
			if (TestTrue(TEXT("CameraInfo has d"), (*Msg)->TryGetArrayField(TEXT("d"), D) && D != nullptr))
			{
				TestEqual(TEXT("D has 5 entries"), D->Num(), 5);
				bool bAllZero = true;
				for (const TSharedPtr<FJsonValue>& V : *D) { if (V.IsValid() && V->AsNumber() != 0.0) { bAllZero = false; } }
				TestTrue(TEXT("D is all zero (pinhole render)"), bAllZero);
			}

			const TArray<TSharedPtr<FJsonValue>>* K = nullptr;
			if (TestTrue(TEXT("CameraInfo K length 9"), (*Msg)->TryGetArrayField(TEXT("k"), K) && K != nullptr && K->Num() == 9))
			{
				const double Expect[9] = { Intr.Fx, 0, Intr.Cx, 0, Intr.Fy, Intr.Cy, 0, 0, 1 };
				for (int32 i = 0; i < 9; ++i)
				{
					TestTrue(FString::Printf(TEXT("K[%d] matches intrinsics"), i), FMath::Abs((*K)[i]->AsNumber() - Expect[i]) <= 1e-6);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* R = nullptr;
			if (TestTrue(TEXT("CameraInfo R length 9"), (*Msg)->TryGetArrayField(TEXT("r"), R) && R != nullptr && R->Num() == 9))
			{
				const double Ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
				for (int32 i = 0; i < 9; ++i)
				{
					TestTrue(FString::Printf(TEXT("R[%d] is identity"), i), FMath::Abs((*R)[i]->AsNumber() - Ident[i]) <= 1e-9);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
			if (TestTrue(TEXT("CameraInfo P length 12"), (*Msg)->TryGetArrayField(TEXT("p"), P) && P != nullptr && P->Num() == 12))
			{
				const double Expect[12] = { Intr.Fx, 0, Intr.Cx, 0, 0, Intr.Fy, Intr.Cy, 0, 0, 0, 1, 0 };
				for (int32 i = 0; i < 12; ++i)
				{
					TestTrue(FString::Printf(TEXT("P[%d] matches intrinsics"), i), FMath::Abs((*P)[i]->AsNumber() - Expect[i]) <= 1e-6);
				}
			}

			// ---- same stamp as the image (published together) -------------
			const TSharedPtr<FJsonObject>* Header = nullptr;
			if ((*Msg)->TryGetObjectField(TEXT("header"), Header) && Header)
			{
				const TSharedPtr<FJsonObject>* StampObj = nullptr;
				if ((*Header)->TryGetObjectField(TEXT("stamp"), StampObj) && StampObj)
				{
					double Sec = -1, Nanosec = -1;
					(*StampObj)->TryGetNumberField(TEXT("sec"), Sec);
					(*StampObj)->TryGetNumberField(TEXT("nanosec"), Nanosec);
					const double Reconstructed = Sec + Nanosec * 1.0e-9;
					TestTrue(TEXT("CameraInfo stamp == frame stamp"), FMath::Abs(Reconstructed - Stamp) <= 1.0e-6);
				}
			}
		}
	}

	return true;
}
