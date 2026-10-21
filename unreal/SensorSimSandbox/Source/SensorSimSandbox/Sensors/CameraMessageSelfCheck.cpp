// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Sensors/CameraMessage.h"
#include "Sensors/CameraConfig.h"
#include "Bridge/RosbridgeClient.h"   // FRosbridgeFraming::ExtractBinaryField

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/IConsoleManager.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogCameraMessageSelfCheck, Log, All);

namespace
{
	// Build a synthetic non-flat RGBA frame of WxH: a gradient so the JPEG
	// encoder produces real content (a flat frame would still decode to WxH,
	// but a gradient exercises the encoder more honestly). Row-major, 4
	// bytes/pixel, alpha 255.
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

	// Decode JPEG bytes and read back the width/height, so the check proves the
	// published bytes are a real JPEG of the configured size. Pure CPU
	// (ImageWrapper libjpeg); works under -nullrhi.
	bool DecodeJpegDimensions(const TArray<uint8>& Jpeg, int32& OutW, int32& OutH)
	{
		OutW = 0;
		OutH = 0;
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::JPEG);
		if (!Wrapper.IsValid())
		{
			return false;
		}
		if (!Wrapper->SetCompressed(Jpeg.GetData(), Jpeg.Num()))
		{
			return false;
		}
		OutW = static_cast<int32>(Wrapper->GetWidth());
		OutH = static_cast<int32>(Wrapper->GetHeight());
		return OutW > 0 && OutH > 0;
	}
}

// ---------------------------------------------------------------------------
// Console command: `CameraMessageSelfCheck [configPath]`
//
// One-command HEADLESS demo/verify + the task 15g gate's camera-message
// build+decode check. No world / RHI needed, so it runs under -nullrhi:
//
//   1. Load + validate the Gate A camera config (width/height).
//   2. Encode a synthetic WxH RGBA frame to JPEG (FCameraMessage::EncodeRgbaToJpeg).
//   3. Build the CompressedImage + CameraInfo publish ops with ONE stamp.
//   4. Parse the ops back as JSON and assert, exactly as Property 7 does:
//      - CompressedImage: op/topic, header.frame_id == "camera_link",
//        format == "jpeg", data base64-decodes to the JPEG bytes.
//      - the decoded JPEG decodes to WxH (dimensions == config).
//      - CameraInfo: op/topic, width/height == config, distortion_model
//        "plumb_bob", d all-zero, K/P carry fx,fy,cx,cy computed from the
//        config at the fixed horizontal FOV, same stamp as the image.
//
// Logs one PASS/FAIL line plus the measured JPEG size and intrinsics.
// ---------------------------------------------------------------------------
static void ExecuteCameraMessageSelfCheck(const TArray<FString>& Args)
{
	FString ConfigPath;
	if (Args.Num() >= 1 && !Args[0].IsEmpty())
	{
		ConfigPath = Args[0];
	}

	const FString ResolvedConfig = ConfigPath.IsEmpty() ? FCameraConfig::ResolveConfigPath() : ConfigPath;
	FCameraConfig Config;
	FString LoadError;
	if (!FCameraConfig::LoadFromFile(ResolvedConfig, Config, LoadError))
	{
		UE_LOG(LogCameraMessageSelfCheck, Error, TEXT("CameraMessageSelfCheck FAIL: could not load %s: %s"), *ResolvedConfig, *LoadError);
		return;
	}
	if (!Config.Validate().bValid)
	{
		UE_LOG(LogCameraMessageSelfCheck, Error, TEXT("CameraMessageSelfCheck FAIL: config %s out of bounds"), *ResolvedConfig);
		return;
	}

	const int32 W = Config.Width;
	const int32 H = Config.Height;
	const double Stamp = 12.345; // arbitrary non-integer stamp (exercises sec/nanosec split)

	// --- 2. Encode a synthetic frame to JPEG --------------------------------
	const TArray<uint8> Frame = MakeGradientFrame(W, H);
	TArray<uint8> Jpeg;
	if (!FCameraMessage::EncodeRgbaToJpeg(W, H, Frame, Jpeg))
	{
		UE_LOG(LogCameraMessageSelfCheck, Error, TEXT("CameraMessageSelfCheck FAIL: JPEG encode failed for %dx%d."), W, H);
		return;
	}

	// --- 3. Build both publish ops with ONE stamp ---------------------------
	const FString ImageOp = FCameraMessage::BuildImagePublishJson(Jpeg, Stamp);
	const FString InfoOp = FCameraMessage::BuildCameraInfoPublishJson(W, H, Stamp);

	bool bOk = true;
	FString FailDetail;
	auto Fail = [&](const FString& Why)
	{
		bOk = false;
		if (FailDetail.IsEmpty()) { FailDetail = Why; }
	};

	// --- 4a. CompressedImage round-trip -------------------------------------
	{
		TSharedPtr<FJsonObject> Op;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(ImageOp);
		if (!FJsonSerializer::Deserialize(Reader, Op) || !Op.IsValid()) { Fail(TEXT("image op not valid JSON")); }
		else
		{
			FString OpName, Topic;
			Op->TryGetStringField(TEXT("op"), OpName);
			Op->TryGetStringField(TEXT("topic"), Topic);
			if (OpName != TEXT("publish")) { Fail(TEXT("image op != publish")); }
			if (Topic != FCameraMessage::GetImageTopic()) { Fail(TEXT("image topic mismatch")); }

			const TSharedPtr<FJsonObject>* Msg = nullptr;
			if (!Op->TryGetObjectField(TEXT("msg"), Msg) || !Msg) { Fail(TEXT("image has no msg")); }
			else
			{
				FString Format;
				(*Msg)->TryGetStringField(TEXT("format"), Format);
				if (Format != FCameraMessage::GetImageFormat()) { Fail(TEXT("format != jpeg")); }

				const TSharedPtr<FJsonObject>* Header = nullptr;
				FString FrameId;
				if ((*Msg)->TryGetObjectField(TEXT("header"), Header) && Header)
				{
					(*Header)->TryGetStringField(TEXT("frame_id"), FrameId);
				}
				if (FrameId != FCameraMessage::CameraFrameId()) { Fail(TEXT("image frame_id != camera_link")); }
			}

			TArray<uint8> Decoded;
			if (!FRosbridgeFraming::ExtractBinaryField(ImageOp, TEXT("data"), Decoded)) { Fail(TEXT("image data base64 decode failed")); }
			else if (Decoded != Jpeg) { Fail(TEXT("image data != original JPEG bytes")); }
		}
	}

	// --- 4b. the JPEG decodes to the configured dimensions ------------------
	int32 DecW = 0, DecH = 0;
	if (!DecodeJpegDimensions(Jpeg, DecW, DecH)) { Fail(TEXT("published JPEG failed to decode")); }
	else if (DecW != W || DecH != H) { Fail(FString::Printf(TEXT("decoded JPEG %dx%d != config %dx%d"), DecW, DecH, W, H)); }

	// --- 4c. CameraInfo intrinsics + resolution -----------------------------
	const FCameraMessage::FIntrinsics Intr = FCameraMessage::ComputeIntrinsics(W, H);
	{
		TSharedPtr<FJsonObject> Op;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(InfoOp);
		if (!FJsonSerializer::Deserialize(Reader, Op) || !Op.IsValid()) { Fail(TEXT("info op not valid JSON")); }
		else
		{
			FString Topic;
			Op->TryGetStringField(TEXT("topic"), Topic);
			if (Topic != FCameraMessage::GetCameraInfoTopic()) { Fail(TEXT("info topic mismatch")); }

			const TSharedPtr<FJsonObject>* Msg = nullptr;
			if (!Op->TryGetObjectField(TEXT("msg"), Msg) || !Msg) { Fail(TEXT("info has no msg")); }
			else
			{
				double MW = -1, MH = -1;
				(*Msg)->TryGetNumberField(TEXT("width"), MW);
				(*Msg)->TryGetNumberField(TEXT("height"), MH);
				if (static_cast<int32>(MW) != W || static_cast<int32>(MH) != H) { Fail(TEXT("CameraInfo width/height != config")); }

				FString Dist;
				(*Msg)->TryGetStringField(TEXT("distortion_model"), Dist);
				if (Dist != FCameraMessage::GetDistortionModel()) { Fail(TEXT("distortion_model != plumb_bob")); }

				const TArray<TSharedPtr<FJsonValue>>* D = nullptr;
				if ((*Msg)->TryGetArrayField(TEXT("d"), D) && D)
				{
					for (const TSharedPtr<FJsonValue>& V : *D)
					{
						if (V.IsValid() && V->AsNumber() != 0.0) { Fail(TEXT("D not all zero")); break; }
					}
				}
				else { Fail(TEXT("CameraInfo has no d")); }

				const TArray<TSharedPtr<FJsonValue>>* K = nullptr;
				if ((*Msg)->TryGetArrayField(TEXT("k"), K) && K && K->Num() == 9)
				{
					const double Expect[9] = { Intr.Fx, 0, Intr.Cx, 0, Intr.Fy, Intr.Cy, 0, 0, 1 };
					for (int32 i = 0; i < 9; ++i)
					{
						if (FMath::Abs((*K)[i]->AsNumber() - Expect[i]) > 1e-6) { Fail(FString::Printf(TEXT("K[%d] mismatch"), i)); break; }
					}
				}
				else { Fail(TEXT("CameraInfo K not length 9")); }

				const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
				if ((*Msg)->TryGetArrayField(TEXT("p"), P) && P && P->Num() == 12)
				{
					const double Expect[12] = { Intr.Fx, 0, Intr.Cx, 0, 0, Intr.Fy, Intr.Cy, 0, 0, 0, 1, 0 };
					for (int32 i = 0; i < 12; ++i)
					{
						if (FMath::Abs((*P)[i]->AsNumber() - Expect[i]) > 1e-6) { Fail(FString::Printf(TEXT("P[%d] mismatch"), i)); break; }
					}
				}
				else { Fail(TEXT("CameraInfo P not length 12")); }
			}
		}
	}

	UE_LOG(LogCameraMessageSelfCheck, Log,
		TEXT("CameraMessageSelfCheck %s: config=%s %dx%d jpeg_bytes=%d decoded=%dx%d fx=%.3f fy=%.3f cx=%.3f cy=%.3f hfov=%.1f%s"),
		bOk ? TEXT("PASS") : TEXT("FAIL"), *ResolvedConfig, W, H, Jpeg.Num(), DecW, DecH,
		Intr.Fx, Intr.Fy, Intr.Cx, Intr.Cy, FCameraMessage::HorizontalFovDeg,
		bOk ? TEXT("") : *FString::Printf(TEXT(" | FAIL: %s"), *FailDetail));
}

static FAutoConsoleCommand GCameraMessageSelfCheckCommand(
	TEXT("CameraMessageSelfCheck"),
	TEXT("Headless camera MESSAGE build+decode demo/verify: encode a synthetic frame to JPEG, build CompressedImage + CameraInfo, decode the JPEG and assert dimensions == config and K/P/width/height match the config intrinsics. Usage: CameraMessageSelfCheck [configPath]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&ExecuteCameraMessageSelfCheck));
