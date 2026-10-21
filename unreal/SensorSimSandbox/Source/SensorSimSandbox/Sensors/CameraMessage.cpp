// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Sensors/CameraMessage.h"

#include "Coords/TfPublisher.h"      // SensorFrames::CameraLink
#include "Bridge/RosbridgeClient.h"  // FRosbridgeFraming::EncodeBase64

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

namespace
{
	/**
	 * Serialize a JSON object to a COMPACT (condensed, single-line) string.
	 * Mirrors FLidarPointCloud / FTfPublisher so every rosbridge op in the
	 * project has the same deterministic, whitespace-free shape.
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
	 * Identical construction to FLidarPointCloud::MakeStamp / FTfPublisher, so
	 * the camera, lidar and TF stamps are built the same way.
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

	/** A header object { stamp, frame_id } shared by both camera messages. */
	TSharedRef<FJsonObject> MakeHeader(double StampSeconds, const FString& FrameId)
	{
		TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
		Header->SetObjectField(TEXT("stamp"), MakeStamp(StampSeconds));
		Header->SetStringField(TEXT("frame_id"), FrameId);
		return Header;
	}

	/** Pack a double array into a JSON number array (for d/k/r/p). */
	TArray<TSharedPtr<FJsonValue>> ToJsonNumberArray(const TArray<double>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (double V : Values)
		{
			Out.Add(MakeShared<FJsonValueNumber>(V));
		}
		return Out;
	}
}

FString FCameraMessage::CameraFrameId()
{
	return SensorFrames::CameraLink;
}

TArray<double> FCameraMessage::FIntrinsics::K() const
{
	// Row-major 3x3: [ fx 0 cx | 0 fy cy | 0 0 1 ].
	return TArray<double>({ Fx, 0.0, Cx,
							 0.0, Fy, Cy,
							 0.0, 0.0, 1.0 });
}

TArray<double> FCameraMessage::FIntrinsics::P() const
{
	// Row-major 3x4 (monocular, no stereo baseline so Tx = Ty = 0):
	// [ fx 0 cx 0 | 0 fy cy 0 | 0 0 1 0 ].
	return TArray<double>({ Fx, 0.0, Cx, 0.0,
							 0.0, Fy, Cy, 0.0,
							 0.0, 0.0, 1.0, 0.0 });
}

FCameraMessage::FIntrinsics FCameraMessage::ComputeIntrinsics(int32 Width, int32 Height, double HorizontalFovDegrees)
{
	// Clamp to a non-degenerate pinhole: W,H >= 1 (avoid a zero principal point
	// / zero focal length), and hfov strictly inside (0, 180) so tan(hfov/2) is
	// finite and positive.
	const double W = static_cast<double>(FMath::Max(1, Width));
	const double H = static_cast<double>(FMath::Max(1, Height));
	const double FovDeg = FMath::Clamp(HorizontalFovDegrees, 1.0e-3, 180.0 - 1.0e-3);
	const double HalfFovRad = FMath::DegreesToRadians(FovDeg) * 0.5;

	FIntrinsics Out;
	// fx = (W/2) / tan(hfov/2); square pixels so fy = fx (Unreal's FOVAngle is
	// the horizontal FOV and the capture uses square pixels).
	Out.Fx = (W * 0.5) / FMath::Tan(HalfFovRad);
	Out.Fy = Out.Fx;
	Out.Cx = W * 0.5;
	Out.Cy = H * 0.5;
	return Out;
}

bool FCameraMessage::EncodeRgbaToJpeg(int32 Width, int32 Height, const TArray<uint8>& RgbaPixels, TArray<uint8>& OutJpeg, int32 Quality)
{
	OutJpeg.Reset();

	if (Width <= 0 || Height <= 0)
	{
		return false;
	}
	const int64 Expected = static_cast<int64>(Width) * static_cast<int64>(Height) * 4;
	if (RgbaPixels.Num() != Expected)
	{
		return false;
	}

	IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::JPEG);
	if (!Wrapper.IsValid())
	{
		return false;
	}

	// Our pixels are row-major RGBA, 8 bits/channel (CameraPipelineRunner's
	// FCameraFrameBuffer layout). Hand them to the JPEG encoder as RGBA8.
	if (!Wrapper->SetRaw(RgbaPixels.GetData(), RgbaPixels.Num(), Width, Height, ERGBFormat::RGBA, 8))
	{
		return false;
	}

	const int32 ClampedQuality = FMath::Clamp(Quality, 1, 100);
	const TArray64<uint8>& Compressed = Wrapper->GetCompressed(ClampedQuality);
	if (Compressed.Num() <= 0)
	{
		return false;
	}

	OutJpeg.SetNumUninitialized(Compressed.Num());
	FMemory::Memcpy(OutJpeg.GetData(), Compressed.GetData(), Compressed.Num());
	return true;
}

bool FCameraMessage::EncodeRgbaToPng(int32 Width, int32 Height, const TArray<uint8>& RgbaPixels, TArray<uint8>& OutPng)
{
	OutPng.Reset();

	if (Width <= 0 || Height <= 0)
	{
		return false;
	}
	const int64 Expected = static_cast<int64>(Width) * static_cast<int64>(Height) * 4;
	if (RgbaPixels.Num() != Expected)
	{
		return false;
	}

	IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
	if (!Wrapper.IsValid())
	{
		return false;
	}

	// Row-major RGBA, 8 bits/channel (FCameraFrameBuffer layout) — same as the
	// JPEG path. PNG is lossless so the stored Ground_Truth is byte-exact.
	if (!Wrapper->SetRaw(RgbaPixels.GetData(), RgbaPixels.Num(), Width, Height, ERGBFormat::RGBA, 8))
	{
		return false;
	}

	// PNG ignores the quality argument; pass a nominal value.
	const TArray64<uint8>& Compressed = Wrapper->GetCompressed(100);
	if (Compressed.Num() <= 0)
	{
		return false;
	}

	OutPng.SetNumUninitialized(Compressed.Num());
	FMemory::Memcpy(OutPng.GetData(), Compressed.GetData(), Compressed.Num());
	return true;
}

TSharedRef<FJsonObject> FCameraMessage::BuildCompressedImageMessage(const TArray<uint8>& JpegBytes, double StampSeconds, const FString& FrameId)
{
	const FString Frame = FrameId.IsEmpty() ? CameraFrameId() : FrameId;

	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetObjectField(TEXT("header"), MakeHeader(StampSeconds, Frame));
	Msg->SetStringField(TEXT("format"), GetImageFormat());
	// Binary field base64-encoded inside msg (design "Rosbridge op payloads").
	Msg->SetStringField(TEXT("data"), FRosbridgeFraming::EncodeBase64(JpegBytes));
	return Msg;
}

TSharedRef<FJsonObject> FCameraMessage::BuildCameraInfoMessage(int32 Width, int32 Height, double StampSeconds, const FString& FrameId)
{
	const FString Frame = FrameId.IsEmpty() ? CameraFrameId() : FrameId;
	const FIntrinsics Intr = ComputeIntrinsics(Width, Height);

	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetObjectField(TEXT("header"), MakeHeader(StampSeconds, Frame));
	// width/height EQUAL the configured resolution (Requirement 4.4).
	Msg->SetNumberField(TEXT("height"), static_cast<double>(Height));
	Msg->SetNumberField(TEXT("width"), static_cast<double>(Width));
	Msg->SetStringField(TEXT("distortion_model"), GetDistortionModel());
	// plumb_bob with zero distortion: a pristine pinhole render.
	Msg->SetArrayField(TEXT("d"), ToJsonNumberArray(TArray<double>({ 0.0, 0.0, 0.0, 0.0, 0.0 })));
	Msg->SetArrayField(TEXT("k"), ToJsonNumberArray(Intr.K()));
	// R is the identity rectification matrix for a monocular camera.
	Msg->SetArrayField(TEXT("r"), ToJsonNumberArray(TArray<double>({ 1.0, 0.0, 0.0,
																	  0.0, 1.0, 0.0,
																	  0.0, 0.0, 1.0 })));
	Msg->SetArrayField(TEXT("p"), ToJsonNumberArray(Intr.P()));
	return Msg;
}

FString FCameraMessage::BuildImagePublishJson(const TArray<uint8>& JpegBytes, double StampSeconds, const FString& FrameId)
{
	TSharedRef<FJsonObject> Msg = BuildCompressedImageMessage(JpegBytes, StampSeconds, FrameId);

	TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("op"), TEXT("publish"));
	Op->SetStringField(TEXT("topic"), GetImageTopic());
	Op->SetObjectField(TEXT("msg"), Msg);
	return JsonToString(Op);
}

FString FCameraMessage::BuildCameraInfoPublishJson(int32 Width, int32 Height, double StampSeconds, const FString& FrameId)
{
	TSharedRef<FJsonObject> Msg = BuildCameraInfoMessage(Width, Height, StampSeconds, FrameId);

	TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("op"), TEXT("publish"));
	Op->SetStringField(TEXT("topic"), GetCameraInfoTopic());
	Op->SetObjectField(TEXT("msg"), Msg);
	return JsonToString(Op);
}

FString FCameraMessage::BuildImageAdvertiseJson()
{
	return FRosbridgeFraming::BuildAdvertiseJson(GetImageTopic(), GetImageMessageType());
}

FString FCameraMessage::BuildCameraInfoAdvertiseJson()
{
	return FRosbridgeFraming::BuildAdvertiseJson(GetCameraInfoTopic(), GetCameraInfoMessageType());
}
