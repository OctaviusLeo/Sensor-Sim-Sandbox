// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"

struct FLidarScan;
class FJsonObject;

/**
 * FLidarPointCloud (design "Lidar_Model" message/frame contract and
 * "sensor_msgs/PointCloud2 (lidar)" message table, Requirement 3.3, 3.4).
 *
 * Builds a sensor_msgs/PointCloud2 from one FLidarScan and frames it as a
 * rosbridge v2 `publish` op whose binary `data` field is base64-encoded inside
 * `msg` (design "Rosbridge op payloads"). The lidar actor publishes the result
 * through the Rosbridge_Client to `/lidar/points`.
 *
 * ===== Engine-light separation (so task 12.2's property test runs headless) ==
 *
 * As with FTfPublisher / FRosbridgeFraming, all the byte-packing and JSON
 * framing is PURE and static here: no UObject, World, socket, or engine state.
 * Property 6 ("Lidar message is well-formed") drives these builders directly
 * over generated scans — the point count equals the scan's returns, the
 * frame_id equals the lidar sensor frame, and the timestamp equals the scan
 * acquisition time — with no engine running. The live ALidarModel path adds
 * only the Rosbridge_Client transport.
 *
 * ===== Message layout (fixed by the design) =================================
 *
 *   header.frame_id = SensorFrames::LidarLink ("lidar_link")   (Req 3.4)
 *   header.stamp    = scan acquisition (capture) time          (Req 3.4)
 *   fields          = x, y, z, each FLOAT32 (datatype 7), count 1
 *   point_step      = 12 bytes (three float32)
 *   height          = 1          (an UNORDERED cloud)
 *   width           = number of returns (hit rays with a published range)
 *   row_step        = point_step * width
 *   is_bigendian    = false      (bytes packed little-endian)
 *   is_dense        = true       (no invalid/NaN points; no-hit/dropped rays
 *                                 are excluded, not packed as NaN)
 *   data            = width points, each [x,y,z] little-endian float32
 *
 * ===== Point geometry =======================================================
 *
 * For each RETURN i (a ray whose PublishedRangeM is set), the point is the ray
 * direction scaled by the published range, expressed in the lidar sensor frame
 * and converted to REP 103 meters:
 *
 *   p_ue_cm = Rays[i].Direction (unit, UE sensor-local) * range_m * 100  (cm)
 *   p_ros   = FRep103::ConvertPosition(p_ue_cm)   -> (x/100, -y/100, z/100) m
 *
 * The /100 in ConvertPosition undoes the *100 above, so p_ros is the point in
 * meters with REP 103 handedness (Y flipped). The fixed converter is reused so
 * the handedness/units mapping is never re-derived here (design "Coordinate
 * conversion").
 */
class FLidarPointCloud
{
public:
	/** sensor_msgs/PointCloud2 topic the lidar publishes to. */
	static FString GetTopic() { return TEXT("/lidar/points"); }

	/** rosbridge v2 message type for the advertise op. */
	static FString GetMessageType() { return TEXT("sensor_msgs/msg/PointCloud2"); }

	/** FLOAT32 PointField datatype code (sensor_msgs/PointField::FLOAT32). */
	static constexpr int32 PointFieldFloat32 = 7;

	/** Bytes per point: three float32 (x, y, z). */
	static constexpr int32 PointStepBytes = 12;

	/** One packed REP 103 point in meters (float32 precision as packed). */
	struct FPackedPoint
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
	};

	/**
	 * Compute the REP 103 (meters) points for every RETURN in `Scan`, in ray
	 * order. No-hit and dropped rays (PublishedRangeM empty) are EXCLUDED, so
	 * the result length equals the scan's return count. Each point is
	 * Direction * published_range projected and REP-103 converted (see above).
	 */
	static TArray<FPackedPoint> ComputeReturnPoints(const FLidarScan& Scan);

	/**
	 * Pack points into the PointCloud2 `data` byte buffer: `width` points, each
	 * three little-endian float32 (x, y, z), 12 bytes per point. is_bigendian
	 * is false because of this packing.
	 */
	static TArray<uint8> PackPointData(const TArray<FPackedPoint>& Points);

	/**
	 * Build the sensor_msgs/PointCloud2 `msg` JSON object for `Scan`:
	 * header (stamp = CaptureTime, frame_id = lidar_link), the x/y/z PointField
	 * array, height = 1, width = return count, point_step = 12, row_step =
	 * 12*width, is_bigendian = false, is_dense = true, and `data` as the
	 * base64 of the packed little-endian point bytes. `FrameId` defaults to the
	 * lidar sensor frame; it is a parameter only so a test can assert the
	 * default is applied.
	 */
	static TSharedRef<FJsonObject> BuildMessage(const FLidarScan& Scan, const FString& FrameId = FString());

	/**
	 * Build the full rosbridge v2 `publish` op string for `Scan` on
	 * `/lidar/points`. This is exactly what the live client sends; the property
	 * test parses it to verify well-formedness and the base64 round-trip.
	 */
	static FString BuildPublishJson(const FLidarScan& Scan, const FString& FrameId = FString());

	/** Build the `advertise` op string for `/lidar/points`. */
	static FString BuildAdvertiseJson();

	/** The lidar sensor frame id ("lidar_link"); matches the PointCloud2 header. */
	static FString LidarFrameId();
};
