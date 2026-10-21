// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Pipeline/SensorRng.h"

#include <cmath>

namespace
{
	// FNV-1a 64-bit constants (fixed by the algorithm; identical everywhere).
	constexpr uint64 FNV_OFFSET_BASIS = 1469598103934665603ULL;
	constexpr uint64 FNV_PRIME = 1099511628211ULL;

	/** Fold one byte into an FNV-1a hash accumulator. */
	FORCEINLINE uint64 FnvByte(uint64 Hash, uint8 Byte)
	{
		Hash ^= static_cast<uint64>(Byte);
		Hash *= FNV_PRIME;
		return Hash;
	}

	/**
	 * SplitMix64: a tiny, well-specified 64-bit generator used to expand a
	 * seed into a stream. Chosen for being fully portable and deterministic.
	 */
	FORCEINLINE uint64 SplitMix64Next(uint64& State)
	{
		State += 0x9E3779B97F4A7C15ULL;
		uint64 Z = State;
		Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBULL;
		return Z ^ (Z >> 31);
	}
}

uint64 FSensorRng::DeriveSensorSeed(int64 RunSeed, const FString& SensorName)
{
	// Hash the run seed's 8 little-endian bytes, then the sensor name's UTF-8
	// bytes. This gives sensor_seed = hash(run_seed, sensor_name) with the two
	// inputs clearly separated, so distinct names produce distinct streams and
	// the same (seed, name) always maps to the same sensor seed.
	uint64 Hash = FNV_OFFSET_BASIS;

	const uint64 SeedBits = static_cast<uint64>(RunSeed);
	for (int32 i = 0; i < 8; ++i)
	{
		Hash = FnvByte(Hash, static_cast<uint8>((SeedBits >> (i * 8)) & 0xFF));
	}

	// A separator byte so e.g. seed concatenation can't collide with a name.
	Hash = FnvByte(Hash, 0x00);

	const FTCHARToUTF8 Utf8(*SensorName);
	const uint8* Bytes = reinterpret_cast<const uint8*>(Utf8.Get());
	const int32 Len = Utf8.Length();
	for (int32 i = 0; i < Len; ++i)
	{
		Hash = FnvByte(Hash, Bytes[i]);
	}

	// Avoid a zero state (SplitMix64 handles zero fine, but a non-zero seed
	// keeps the first draw well-mixed regardless of inputs).
	return Hash != 0 ? Hash : FNV_OFFSET_BASIS;
}

FSensorRng::FSensorRng(uint64 InSensorSeed)
	: InitialSeed(InSensorSeed)
	, State(InSensorSeed)
{
}

FSensorRng::FSensorRng(int64 RunSeed, const FString& SensorName)
	: FSensorRng(DeriveSensorSeed(RunSeed, SensorName))
{
}

void FSensorRng::Reset()
{
	State = InitialSeed;
	bHasCachedGaussian = false;
	CachedGaussian = 0.0;
}

uint64 FSensorRng::NextUInt64()
{
	return SplitMix64Next(State);
}

double FSensorRng::NextUniform()
{
	// Use the top 53 bits so every representable double in [0, 1) is reachable
	// with uniform spacing (standard trick for high-quality uniforms).
	const uint64 Bits = NextUInt64() >> 11; // keep 53 bits
	return static_cast<double>(Bits) * (1.0 / 9007199254740992.0); // 2^-53
}

double FSensorRng::NextGaussian()
{
	if (bHasCachedGaussian)
	{
		bHasCachedGaussian = false;
		return CachedGaussian;
	}

	// Box-Muller. Guard u1 away from 0 so log() is finite.
	double U1 = NextUniform();
	const double U2 = NextUniform();
	if (U1 <= 0.0)
	{
		U1 = 1.0 / 9007199254740992.0; // smallest positive uniform step (2^-53)
	}

	const double R = std::sqrt(-2.0 * std::log(U1));
	const double Theta = 2.0 * PI * U2;

	const double Z0 = R * std::cos(Theta);
	const double Z1 = R * std::sin(Theta);

	CachedGaussian = Z1;
	bHasCachedGaussian = true;
	return Z0;
}

double FSensorRng::NextGaussian(double Mean, double Sigma)
{
	return Mean + Sigma * NextGaussian();
}
