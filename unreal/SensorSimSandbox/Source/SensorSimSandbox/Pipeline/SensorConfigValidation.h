// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"

/**
 * Result of validating a candidate sensor configuration against its bounds.
 *
 * The config-bounds validation contract (Requirement 3.2, 4.2), shared by
 * every sensor: when any parameter is outside its stated bound the candidate
 * configuration is rejected as a whole (never partially applied), the error
 * names the offending parameter, and the holder retains its last valid
 * configuration.
 *
 * This result captures just that outcome. It is pure data with no engine
 * dependency, so the validation logic is unit-testable headlessly.
 */
struct FSensorConfigValidationResult
{
	/** True when every validated parameter was within its bound. */
	bool bValid = true;

	/** Name of the first out-of-bound parameter, empty when valid. */
	FString OffendingParameter;

	/** Human-readable error naming the parameter and its bound, empty when valid. */
	FString ErrorMessage;

	static FSensorConfigValidationResult Valid()
	{
		return FSensorConfigValidationResult{};
	}

	static FSensorConfigValidationResult Invalid(const FString& Parameter, const FString& Message)
	{
		FSensorConfigValidationResult R;
		R.bValid = false;
		R.OffendingParameter = Parameter;
		R.ErrorMessage = Message;
		return R;
	}
};

/**
 * A small, ordered bound checker. Sensors describe each configurable
 * parameter's inclusive/exclusive bounds once, then call Validate() on a
 * candidate; the first violation (in declared order) is reported, so the
 * "offending parameter" is deterministic.
 *
 * Bounds are expressed as doubles; integer parameters (e.g. lidar channels,
 * latency_ms) are validated by passing their value as a double and marking the
 * bound integer-only so a non-integral candidate is also rejected.
 *
 * Pure, engine-independent, reusable by lidar/camera/IMU/GPS.
 */
class FSensorBoundSet
{
public:
	/**
	 * Add a bound for `ParameterName`.
	 * @param Min            lower bound value
	 * @param Max            upper bound value
	 * @param bMinInclusive  true: value >= Min allowed; false: value > Min
	 * @param bMaxInclusive  true: value <= Max allowed; false: value < Max
	 * @param bIntegerOnly   true: a non-integral value is rejected
	 */
	FSensorBoundSet& Add(
		const FString& ParameterName,
		double Min,
		double Max,
		bool bMinInclusive = true,
		bool bMaxInclusive = true,
		bool bIntegerOnly = false);

	/** Check a single already-added parameter's candidate value. */
	FSensorConfigValidationResult Check(const FString& ParameterName, double Value) const;

	/**
	 * Validate a full candidate as a map of parameter name -> value. Every
	 * declared bound must have a matching entry; the first declared bound whose
	 * value is out of range (or missing) determines the rejection, so results
	 * are order-deterministic.
	 */
	FSensorConfigValidationResult Validate(const TMap<FString, double>& Candidate) const;

private:
	struct FBound
	{
		FString Name;
		double Min = 0.0;
		double Max = 0.0;
		bool bMinInclusive = true;
		bool bMaxInclusive = true;
		bool bIntegerOnly = false;
	};

	TArray<FBound> Bounds;

	static FSensorConfigValidationResult CheckBound(const FBound& Bound, double Value);
	static FString FormatBound(const FBound& Bound);
};
