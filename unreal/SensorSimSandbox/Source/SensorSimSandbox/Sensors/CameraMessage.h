// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"

struct FCameraFrame;
struct FCameraConfig;
class FJsonObject;

/**
 * FCameraMessage (design "Camera_Model" message/frame contract and the
 * "sensor_msgs/CompressedImage + sensor_msgs/CameraInfo (camera)" message
 * table, Requirement 4.3, 4.4).
 *
 * Builds a sensor_msgs/CompressedImage (JPEG) and an accompanying
 * sensor_msgs/CameraInfo from one published camera frame + its config, and
 * frames each as a rosbridge v2 `publish` op whose binary `data` field is
 * base64-encoded inside `msg` (design "Rosbridge op payloads"). The camera
 * actor publishes the pair through the Rosbridge_Client to
 * `/camera/image/compressed` and `/camera/camera_info` with the SAME stamp.
 *
 * ===== Engine-light separation (so task 14.2's property test runs headless) ==
 *
 * As with FLidarPointCloud / FTfPublisher / FRosbridgeFraming, all the JSON
 * framing and the intrinsics math is PURE and static here: no UObject, World,
 * socket, or engine state. Property 7 ("Camera message is well-formed") drives
 * these builders directly over generated frames — the CompressedImage is a
 * `"jpeg"` message carrying base64 JPEG bytes, and the CameraInfo width/height
 * equal the configured resolution and its intrinsics equal those used to render
 * the frame — with no engine running.
 *
 * The ONE engine-dependent step, encoding RGBA pixels to JPEG bytes, is
 * isolated in EncodeRgbaToJpeg (ImageWrapper module). The message builders take
 * ALREADY-ENCODED JPEG bytes, so the property test can feed known/synthetic
 * JPEG bytes and the live actor feeds the engine-encoded bytes. The live
 * ACameraModel path adds only EncodeRgbaToJpeg + the Rosbridge_Client transport.
 *
 * ===== Intrinsics convention (fixed by the design) ==========================
 *
 * The scene-capture camera is a PINHOLE camera with a horizontal field of view
 * of HorizontalFovDeg (set on the USceneCaptureComponent2D by ACameraModel from
 * this SAME constant, so CameraInfo matches the actual projection). Unreal's
 * SceneCaptureComponent2D::FOVAngle is the HORIZONTAL FOV; the capture uses
 * square pixels, so the focal length in pixels is equal in x and y:
 *
 *   fx = (W / 2) / tan(hfov / 2)      (W = configured width, hfov in radians)
 *   fy = fx                           (square pixels)
 *   cx = W / 2                        (principal point at image centre)
 *   cy = H / 2                        (H = configured height)
 *
 *   K = [ fx, 0,  cx,
 *          0, fy, cy,
 *          0,  0,  1 ]                (row-major 3x3)
 *
 *   P = [ fx, 0,  cx, 0,
 *          0, fy, cy, 0,
 *          0,  0,  1, 0 ]            (row-major 3x4; Tx = Ty = 0, monocular)
 *
 *   R = identity 3x3
 *   distortion_model = "plumb_bob"
 *   D = [0, 0, 0, 0, 0]              (pinhole render: no lens distortion)
 *
 * ===== Message layout (fixed by the design) =================================
 *
 *   CompressedImage.header.frame_id = SensorFrames::CameraLink ("camera_link")
 *   CompressedImage.header.stamp    = frame capture/acquisition time
 *   CompressedImage.format          = "jpeg"
 *   CompressedImage.data            = base64 of the JPEG-encoded published RGBA
 *   CameraInfo.header.{stamp,frame_id} = same stamp + "camera_link"
 *   CameraInfo.width / height          = configured resolution
 *   CameraInfo.distortion_model        = "plumb_bob"
 *   CameraInfo.d / k / r / p           = intrinsics above
 */
class FCameraMessage
{
public:
	/** sensor_msgs/CompressedImage topic the camera publishes to. */
	static FString GetImageTopic() { return TEXT("/camera/image/compressed"); }

	/** sensor_msgs/CameraInfo topic the camera publishes to. */
	static FString GetCameraInfoTopic() { return TEXT("/camera/camera_info"); }

	/** rosbridge v2 message type for the CompressedImage advertise op. */
	static FString GetImageMessageType() { return TEXT("sensor_msgs/msg/CompressedImage"); }

	/** rosbridge v2 message type for the CameraInfo advertise op. */
	static FString GetCameraInfoMessageType() { return TEXT("sensor_msgs/msg/CameraInfo"); }

	/** CompressedImage.format value for a JPEG payload. */
	static FString GetImageFormat() { return TEXT("jpeg"); }

	/** CameraInfo.distortion_model value (pinhole render -> zero distortion). */
	static FString GetDistortionModel() { return TEXT("plumb_bob"); }

	/**
	 * Horizontal field of view (degrees) of the scene-capture camera. This is
	 * the SINGLE source of truth: ACameraModel sets the
	 * USceneCaptureComponent2D::FOVAngle to this value, and the intrinsics below
	 * are computed from it, so CameraInfo matches the real projection. Unreal's
	 * SceneCaptureComponent2D defaults FOVAngle to 90 degrees; we pin it here so
	 * the calibration never depends on an implicit engine default.
	 */
	static constexpr double HorizontalFovDeg = 90.0;

	/** The camera sensor frame id ("camera_link"); matches both message headers. */
	static FString CameraFrameId();

	/** Intrinsics for a width x height pinhole render at HorizontalFovDeg. */
	struct FIntrinsics
	{
		double Fx = 0.0;
		double Fy = 0.0;
		double Cx = 0.0;
		double Cy = 0.0;

		/** Row-major 3x3 K. */
		TArray<double> K() const;
		/** Row-major 3x4 P (monocular: Tx = Ty = 0). */
		TArray<double> P() const;
	};

	/**
	 * Compute pinhole intrinsics for a `Width` x `Height` image at
	 * `HorizontalFovDegrees` horizontal FOV (square pixels). Width/Height are
	 * clamped to >= 1 so a degenerate 0-size never divides by zero; the FOV is
	 * clamped to (0, 180) so tan(hfov/2) is finite and positive.
	 */
	static FIntrinsics ComputeIntrinsics(int32 Width, int32 Height, double HorizontalFovDegrees = HorizontalFovDeg);

	/**
	 * Encode a published RGBA frame to JPEG bytes using Unreal's ImageWrapper
	 * module (EImageFormat::JPEG). This is the ONLY engine-dependent step; the
	 * message builders take the result. Returns false (OutJpeg empty) when the
	 * pixel buffer size does not match Width*Height*4 or the encoder is
	 * unavailable. `Quality` is the JPEG quality in [1, 100].
	 */
	static bool EncodeRgbaToJpeg(int32 Width, int32 Height, const TArray<uint8>& RgbaPixels, TArray<uint8>& OutJpeg, int32 Quality = 90);

	/**
	 * Encode a Ground_Truth RGBA frame to lossless PNG bytes using Unreal's
	 * ImageWrapper module (EImageFormat::PNG). The design's logging format saves
	 * the truth frame as a `.png` and the published frame as a `.jpg` for a
	 * sampled frame ("frames/cam_000017.png" / "frames/cam_000017_pub.jpg"), so
	 * the Ground_Truth is stored losslessly. This is an engine-dependent step
	 * (the ImageWrapper module) exactly like EncodeRgbaToJpeg; it is CPU-only
	 * (libpng) so it works under -nullrhi. Returns false (OutPng empty) when the
	 * pixel buffer size does not match Width*Height*4 or the encoder is
	 * unavailable.
	 */
	static bool EncodeRgbaToPng(int32 Width, int32 Height, const TArray<uint8>& RgbaPixels, TArray<uint8>& OutPng);

	/**
	 * Build the sensor_msgs/CompressedImage `msg` JSON object: header
	 * (stamp = StampSeconds, frame_id = camera_link), format = "jpeg", and
	 * `data` as the base64 of `JpegBytes`. `FrameId` defaults to the camera
	 * sensor frame; it is a parameter only so a test can assert the default.
	 */
	static TSharedRef<FJsonObject> BuildCompressedImageMessage(const TArray<uint8>& JpegBytes, double StampSeconds, const FString& FrameId = FString());

	/**
	 * Build the sensor_msgs/CameraInfo `msg` JSON object: header
	 * (stamp = StampSeconds, frame_id = camera_link), width/height equal to the
	 * configured resolution, distortion_model = "plumb_bob", d = zeros, and
	 * k / r / p from the intrinsics computed for (Width, Height) at the fixed
	 * horizontal FOV.
	 */
	static TSharedRef<FJsonObject> BuildCameraInfoMessage(int32 Width, int32 Height, double StampSeconds, const FString& FrameId = FString());

	/** Full rosbridge v2 `publish` op string for the CompressedImage. */
	static FString BuildImagePublishJson(const TArray<uint8>& JpegBytes, double StampSeconds, const FString& FrameId = FString());

	/** Full rosbridge v2 `publish` op string for the CameraInfo. */
	static FString BuildCameraInfoPublishJson(int32 Width, int32 Height, double StampSeconds, const FString& FrameId = FString());

	/** `advertise` op string for `/camera/image/compressed`. */
	static FString BuildImageAdvertiseJson();

	/** `advertise` op string for `/camera/camera_info`. */
	static FString BuildCameraInfoAdvertiseJson();
};
