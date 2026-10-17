// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"

/**
 * Deterministic, engine-independent per-sensor random number generator.
 *
 * All randomness in the sensor pipeline flows from a single run seed. Each
 * sensor derives its own independent sub-stream from a *named* seed so that
 * one sensor's draws never perturb another's, and so that re-running with the
 * same (run_seed, sensor_name) reproduces every draw exactly across runs and
 * machines (Requirement 0.2, 0.3).
 *
 * Determinism is the core property, so this class deliberately does NOT use
 * Unreal's FMath RNG (whose algorithm is an engine implementation detail that
 * may change between versions). Instead it uses:
 *   - FNV-1a 64-bit to combine the run seed and sensor name into a sensor seed
 *     (`sensor_seed = hash(run_seed, sensor_name)`), and
 *   - SplitMix64 to expand that seed into a stream of 64-bit values,
 * both of which are small, fully specified, and identical on every platform.
 *
 * Gaussian samples use the Box-Muller transform over two uniforms, so a given
 * seed yields the same sequence of normal draws everywhere.
 *
 * This type has no UObject/World/Engine dependency and is unit-testable
 * headlessly. It is reused by lidar (task 6), camera (task 13), IMU and GPS
 * (tasks 20, 21).
 */
class FSensorRng
{
public:
	/**
	 * Derive a per-sensor seed from the run seed and sensor name using FNV-1a
	 * (64-bit). The same (RunSeed, SensorName) always yields the same value;
	 * different sensor names yield independent sub-streams.
	 */
	static uint64 DeriveSensorSeed(int64 RunSeed, const FString& SensorName);

	/** Construct directly from an already-derived 64-bit sensor seed. */
	explicit FSensorRng(uint64 InSensorSeed);

	/** Construct from a run seed and sensor name (the common path). */
	FSensorRng(int64 RunSeed, const FString& SensorName);

	/** The sensor seed this generator was constructed with. */
	uint64 GetSeed() const { return InitialSeed; }

	/** Reset the stream back to its initial seed (reproduces the same draws). */
	void Reset();

	/** Next raw 64-bit value from the SplitMix64 stream. */
	uint64 NextUInt64();

	/**
	 * Next uniform double in [0, 1). Uses the top 53 bits of a 64-bit draw so
	 * the result has full double precision and is exactly reproducible.
	 */
	double NextUniform();

	/**
	 * Next standard normal sample N(0, 1) via Box-Muller. One of the two
	 * values produced per transform is cached and returned on the next call,
	 * so the stream advances by one 2-uniform transform every two samples.
	 */
	double NextGaussian();

	/** Next normal sample N(Mean, Sigma^2). Sigma must be >= 0. */
	double NextGaussian(double Mean, double Sigma);

private:
	uint64 InitialSeed;
	uint64 State;

	// Box-Muller produces samples in pairs; cache the second one.
	bool bHasCachedGaussian = false;
	double CachedGaussian = 0.0;
};
