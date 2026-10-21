// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Coords/Rep103.h"

/**
 * TfPublisher (design "REP 103 Converter + TfPublisher", Requirement 7.1, 7.4).
 *
 * Publishes TF for the rover body frame and each sensor frame at >= 10 Hz.
 * Each frame's Unreal transform is converted to REP 103 (meters, right-handed)
 * through FRep103, then packed into a `tf2_msgs/msg/TFMessage` and sent on the
 * `/tf` topic through a rosbridge `publish` op.
 *
 * The pure message-building and >= 10 Hz scheduling logic lives here, free of
 * any live WebSocket, so Property/example tests can drive it headlessly
 * (consistent with how FSensorPipeline and FSmokePublisher separate pure
 * framing from transport). The actual transport is injected through
 * ITfTransport: the real Rosbridge_Client (task 11) implements it, and a
 * capturing stub sink is usable now and in tests.
 *
 * Frame tree (design: "rover body and each sensor frame"):
 *
 *     odom
 *       └── base_link            (rover body)
 *             ├── lidar_link      (lidar sensor frame)
 *             ├── camera_link     (camera sensor frame, M3)
 *             ├── imu_link        (M5)
 *             └── gps_link        (M5)
 *
 * The rover body transform is child of a fixed `odom` parent; each sensor frame
 * is a child of `base_link`. Frame ids match the `frame_id` the sensor messages
 * stamp (e.g. the lidar PointCloud2 header frame, task 12).
 */

/** Canonical TF frame ids used across the project. */
namespace SensorFrames
{
	/** Fixed world/odometry parent of the rover body. */
	static const TCHAR* Odom = TEXT("odom");
	/** Rover body frame (REP 105 `base_link`). */
	static const TCHAR* BaseLink = TEXT("base_link");
	/** Lidar sensor frame (matches PointCloud2 header.frame_id, task 12). */
	static const TCHAR* LidarLink = TEXT("lidar_link");
	/** Camera sensor frame (M3). */
	static const TCHAR* CameraLink = TEXT("camera_link");
	/** IMU sensor frame (M5). */
	static const TCHAR* ImuLink = TEXT("imu_link");
	/** GPS sensor frame (M5). */
	static const TCHAR* GpsLink = TEXT("gps_link");
}

/**
 * One frame's pose relative to its parent, already in Unreal units
 * (centimeters, left-handed). TfPublisher converts it to REP 103 before
 * serializing. `ParentFrameId`/`ChildFrameId` are the TF header frame and child
 * frame ids.
 */
struct FTfFrame
{
	FString ParentFrameId;
	FString ChildFrameId;
	FVector UePositionCm = FVector::ZeroVector;
	FQuat UeRotation = FQuat::Identity;

	FTfFrame() = default;
	FTfFrame(const FString& InParent, const FString& InChild, const FVector& InPos, const FQuat& InRot)
		: ParentFrameId(InParent), ChildFrameId(InChild), UePositionCm(InPos), UeRotation(InRot)
	{
	}
};

/**
 * Transport sink for the built rosbridge op string. The real Rosbridge_Client
 * (task 11) implements this over the WebSocket; the stub below just records the
 * ops for headless verification and tests.
 */
class ITfTransport
{
public:
	virtual ~ITfTransport() = default;
	/** Send one rosbridge op JSON string (an `advertise` or a `publish`). */
	virtual void Send(const FString& OpJson) = 0;
};

/**
 * A capturing stub transport: records every op string. Used by the headless
 * verification and by tests before the real Rosbridge_Client exists. Also logs
 * each op so a running game writes evidence to the log.
 */
class FCapturingTfTransport : public ITfTransport
{
public:
	virtual void Send(const FString& OpJson) override;

	const TArray<FString>& GetSentOps() const { return SentOps; }
	int32 NumPublishes() const;
	void Reset() { SentOps.Reset(); }

private:
	TArray<FString> SentOps;
};

class FTfPublisher
{
public:
	/** The TF topic. */
	static FString GetTopic() { return TEXT("/tf"); }
	/** The TF message type (rosbridge v2 publishes `tf2_msgs/msg/TFMessage`). */
	static FString GetMessageType() { return TEXT("tf2_msgs/msg/TFMessage"); }

	/**
	 * Minimum TF publish rate the contract requires (Requirement 7.4). The
	 * scheduler publishes at this rate or faster; it is never lowered.
	 */
	static constexpr double MinRateHz = 10.0;

	/**
	 * @param InTransport  sink for the built ops (not owned; must outlive this).
	 * @param InRateHz     target publish rate; clamped UP to MinRateHz so the
	 *                     >= 10 Hz contract always holds even if a caller passes
	 *                     a smaller value.
	 */
	explicit FTfPublisher(ITfTransport* InTransport, double InRateHz = MinRateHz);

	/** The effective publish rate (>= MinRateHz). */
	double GetRateHz() const { return RateHz; }
	/** The publish period in seconds (1 / rate). */
	double GetPeriodSeconds() const { return 1.0 / RateHz; }

	/** Advertise `/tf` once (idempotent); the real client re-advertises on reconnect. */
	void Advertise();

	/**
	 * Build and send one TFMessage carrying every frame in `Frames`, stamped at
	 * `StampSeconds` (UTC seconds; ROS header time). Converts each frame's
	 * Unreal transform to REP 103 first. Returns the published op JSON (also
	 * sent through the transport).
	 */
	FString Publish(const TArray<FTfFrame>& Frames, double StampSeconds);

	/**
	 * Scheduler: given the current time `NowSeconds` (seconds), publish iff at
	 * least one period has elapsed since the last publish, keeping the >= 10 Hz
	 * rate. Returns true iff a publish happened this call. `Frames` is captured
	 * fresh each publish so moving transforms are reflected. The first call
	 * always publishes.
	 */
	bool Tick(double NowSeconds, const TArray<FTfFrame>& Frames);

	/** Number of publishes emitted so far (for rate verification). */
	int32 GetPublishCount() const { return PublishCount; }

	// --- Pure builders (static, no transport) so tests can inspect framing ---

	/** Build the rosbridge v2 `advertise` op for `/tf`. */
	static FString BuildAdvertiseJson();

	/**
	 * Build the rosbridge v2 `publish` op for a TFMessage over the given frames.
	 * Each transform's translation/rotation is the REP 103 conversion of the
	 * frame's Unreal pose. `StampSeconds` fills every transform's
	 * `header.stamp` (split into sec / nanosec).
	 */
	static FString BuildPublishJson(const TArray<FTfFrame>& Frames, double StampSeconds);

private:
	ITfTransport* Transport = nullptr;
	double RateHz = MinRateHz;
	bool bAdvertised = false;

	// Scheduler state.
	bool bHasPublished = false;
	double LastPublishTime = 0.0;
	double NextDeadline = 0.0;
	int32 PublishCount = 0;
};
