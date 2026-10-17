// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"

/**
 * One payload waiting in the latency queue, parameterised on the payload type.
 *
 * The queue is pure and engine-independent: it works on a caller-supplied
 * clock (game time in the engine, a synthetic clock in tests), so the latency
 * stage can be unit/property tested without a running world.
 */
template <typename PayloadType>
struct TLatencySample
{
	/** Per-sensor capture counter, from 0. */
	int64 Seq = 0;

	/** Time (seconds) the sample was captured / enqueued. */
	double CaptureTime = 0.0;

	/** The sample payload (e.g. a scan's noisy ranges, a frame reference). */
	PayloadType Payload;
};

/** A sample released from the queue, carrying its stamped publish time. */
template <typename PayloadType>
struct TReleasedSample
{
	int64 Seq = 0;
	double CaptureTime = 0.0;
	double PublishTime = 0.0;
	PayloadType Payload;

	/** Realised delay = PublishTime - CaptureTime (>= 0). */
	double Delay() const { return PublishTime - CaptureTime; }
};

/**
 * Latency queue: holds captured samples for a configured latency `L` (seconds)
 * and releases them on demand against a clock (Requirement 3.7, 4.6).
 *
 * Release semantics, exactly as the design specifies:
 *   - A sample captured at `CaptureTime` with latency `L` is released at the
 *     first check where `CaptureTime + L <= now`.
 *   - On release the sample's `PublishTime` is stamped with `now`.
 *   - With `L = 0` a sample is released in the same step it was captured
 *     (delay exactly 0 when checked at its capture time).
 *
 * Enqueue order is preserved; the queue never reorders samples. The queue does
 * not clamp, scale, or otherwise alter payloads — it only delays them.
 */
template <typename PayloadType>
class TLatencyQueue
{
public:
	TLatencyQueue() = default;

	explicit TLatencyQueue(double InLatencySeconds)
		: LatencySeconds(InLatencySeconds)
	{
	}

	/** Configured latency in seconds (>= 0). */
	double GetLatencySeconds() const { return LatencySeconds; }

	/** Set the configured latency in seconds (must be >= 0). */
	void SetLatencySeconds(double InLatencySeconds)
	{
		LatencySeconds = InLatencySeconds;
	}

	/** Number of samples currently waiting. */
	int32 Num() const { return Pending.Num(); }

	/** True when no sample is waiting. */
	bool IsEmpty() const { return Pending.Num() == 0; }

	/** Enqueue a captured sample. Enqueue order is preserved. */
	void Enqueue(int64 Seq, double CaptureTime, const PayloadType& Payload)
	{
		TLatencySample<PayloadType> Sample;
		Sample.Seq = Seq;
		Sample.CaptureTime = CaptureTime;
		Sample.Payload = Payload;
		Pending.Add(MoveTemp(Sample));
	}

	/**
	 * Release every sample whose `CaptureTime + L <= Now`, in enqueue order,
	 * stamping each with `PublishTime = Now`. Returns the released samples;
	 * samples not yet due remain queued.
	 */
	TArray<TReleasedSample<PayloadType>> ReleaseDue(double Now)
	{
		TArray<TReleasedSample<PayloadType>> Released;

		int32 ReleaseCount = 0;
		for (const TLatencySample<PayloadType>& Sample : Pending)
		{
			// First check at which capture_time + L <= now.
			if (Sample.CaptureTime + LatencySeconds <= Now)
			{
				TReleasedSample<PayloadType> Out;
				Out.Seq = Sample.Seq;
				Out.CaptureTime = Sample.CaptureTime;
				Out.PublishTime = Now;
				Out.Payload = Sample.Payload;
				Released.Add(MoveTemp(Out));
				++ReleaseCount;
			}
			else
			{
				// Pending is in non-decreasing capture-time order for a single
				// sensor, but to stay correct even if a caller enqueues out of
				// order we keep scanning rather than breaking early.
				continue;
			}
		}

		// Remove the released entries (match by identity of position: a sample
		// is released iff it was due). Rebuild the pending list with the ones
		// that were not due, preserving order.
		if (ReleaseCount > 0)
		{
			TArray<TLatencySample<PayloadType>> StillPending;
			StillPending.Reserve(Pending.Num() - ReleaseCount);
			for (TLatencySample<PayloadType>& Sample : Pending)
			{
				if (Sample.CaptureTime + LatencySeconds > Now)
				{
					StillPending.Add(MoveTemp(Sample));
				}
			}
			Pending = MoveTemp(StillPending);
		}

		return Released;
	}

private:
	double LatencySeconds = 0.0;
	TArray<TLatencySample<PayloadType>> Pending;
};
