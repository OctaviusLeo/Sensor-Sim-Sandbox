// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"

/**
 * REP 103 converter (design "Coordinate conversion", Requirement 7.1, 7.2, 7.3).
 *
 * Unreal uses a LEFT-handed frame measured in CENTIMETERS. ROS REP 103 is a
 * RIGHT-handed frame measured in METERS, with x forward, y left, z up. The
 * conversion is a Y-axis flip plus a cm->m rescale, with the matching
 * handedness change applied to orientation.
 *
 * The mapping is FIXED by the design and must not be re-derived here:
 *
 *     position:  x_ros =  x_ue / 100
 *                y_ros = -y_ue / 100
 *                z_ros =  z_ue / 100
 *
 *     rotation:  q_ros = (w, -x, y, -z)   for a UE quaternion (w, x, y, z)
 *
 * This class is PURE: no UObject, World, Engine, or rendering dependency, so it
 * is unit/property-testable headlessly (task 9.2 drives Property 12 and 13
 * directly), consistent with how the other pure cores (FSensorPipeline,
 * FLidarPipelineRunner) are isolated from the engine.
 *
 * All values are double precision internally so the conversion and its inverse
 * round-trip to well within the 1e-6 tolerance the properties require.
 */

/** A REP 103 position in meters (right-handed, x forward / y left / z up). */
struct FRep103Position
{
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;

	FRep103Position() = default;
	FRep103Position(double InX, double InY, double InZ) : X(InX), Y(InY), Z(InZ) {}
};

/**
 * A REP 103 orientation quaternion, stored (w, x, y, z) to match the design's
 * notation and the Run_Log `quat` ordering ([w, x, y, z]). ROS message
 * serialization orders the fields x, y, z, w; the TF message builder reorders
 * at serialization time.
 */
struct FRep103Quat
{
	double W = 1.0;
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;

	FRep103Quat() = default;
	FRep103Quat(double InW, double InX, double InY, double InZ) : W(InW), X(InX), Y(InY), Z(InZ) {}
};

class FRep103
{
public:
	/** Unreal -> REP 103 centimeters-to-meters scale factor (design: /100). */
	static constexpr double CmToMeters = 1.0 / 100.0;

	/**
	 * Convert an Unreal position (centimeters, left-handed) to REP 103 meters:
	 *     (x/100, -y/100, z/100).
	 */
	static FRep103Position ConvertPosition(const FVector& UePositionCm)
	{
		return FRep103Position(
			 UePositionCm.X * CmToMeters,
			-UePositionCm.Y * CmToMeters,
			 UePositionCm.Z * CmToMeters);
	}

	/**
	 * Convert an Unreal quaternion (w, x, y, z) to REP 103: q_ros = (w, -x, y, -z).
	 * The Y-axis flip is a handedness change, so the X and Z rotational
	 * components negate to preserve the right-handed sense (design).
	 */
	static FRep103Quat ConvertRotation(const FQuat& UeRotation)
	{
		return FRep103Quat(
			 UeRotation.W,
			-UeRotation.X,
			 UeRotation.Y,
			-UeRotation.Z);
	}

	/**
	 * Inverse of ConvertPosition (REP 103 meters -> Unreal centimeters), used by
	 * the round-trip identity check. Scale back by 100 and un-flip Y.
	 */
	static FVector InvertPosition(const FRep103Position& RosPositionM)
	{
		return FVector(
			 RosPositionM.X / CmToMeters,
			-RosPositionM.Y / CmToMeters,
			 RosPositionM.Z / CmToMeters);
	}

	/**
	 * Inverse of ConvertRotation (REP 103 -> Unreal). The mapping (w,-x,y,-z) is
	 * its own inverse in component form, so applying it a second time recovers
	 * the original Unreal quaternion. Used by the forward-then-back identity
	 * check in Property 13.
	 */
	static FQuat InvertRotation(const FRep103Quat& RosRotation)
	{
		return FQuat(
			-RosRotation.X,  // X
			 RosRotation.Y,  // Y
			-RosRotation.Z,  // Z
			 RosRotation.W); // W (FQuat ctor is (X, Y, Z, W))
	}
};
