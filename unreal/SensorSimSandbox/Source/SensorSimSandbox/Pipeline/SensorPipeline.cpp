// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Pipeline/SensorPipeline.h"

FSensorPipeline::FSensorPipeline(int64 RunSeed, const FString& SensorName)
	: Rng(RunSeed, SensorName)
{
}

TArray<FSensorValue> FSensorPipeline::AddGaussianNoise(
	const TArray<FSensorValue>& Truth,
	double Sigma,
	bool bClamp,
	double ClampMin,
	double ClampMax)
{
	TArray<FSensorValue> Out;
	Out.Reserve(Truth.Num());

	// Sigma = 0 is an exact passthrough. Make NO RNG draw so a noise-free
	// sensor neither perturbs values nor advances the shared stream.
	const bool bNoiseless = (Sigma == 0.0);

	for (const FSensorValue& Value : Truth)
	{
		if (!Value.IsSet())
		{
			// No return: pass the gap through untouched.
			Out.Add(FSensorValue());
			continue;
		}

		double Result = Value.GetValue();
		if (!bNoiseless)
		{
			Result += Rng.NextGaussian(0.0, Sigma);
		}

		if (bClamp)
		{
			Result = FMath::Clamp(Result, ClampMin, ClampMax);
		}

		Out.Add(FSensorValue(Result));
	}

	return Out;
}

FSensorStageOutput FSensorPipeline::ApplyPerElementDropout(const TArray<FSensorValue>& Values, double DropoutProb)
{
	FSensorStageOutput Output;
	Output.Published.Reserve(Values.Num());
	Output.bDropped.Reserve(Values.Num());

	const bool bNeverDrop = (DropoutProb <= 0.0);
	const bool bAlwaysDrop = (DropoutProb >= 1.0);

	for (const FSensorValue& Value : Values)
	{
		if (!Value.IsSet())
		{
			// No return: not counted, not drawn for, not dropped.
			Output.Published.Add(FSensorValue());
			Output.bDropped.Add(false);
			continue;
		}

		bool bDrop;
		if (bNeverDrop)
		{
			bDrop = false;
		}
		else if (bAlwaysDrop)
		{
			bDrop = true;
		}
		else
		{
			// Drop iff u < p, u ~ Uniform[0, 1).
			bDrop = (Rng.NextUniform() < DropoutProb);
		}

		Output.bDropped.Add(bDrop);
		Output.Published.Add(bDrop ? FSensorValue() : Value);
	}

	return Output;
}

bool FSensorPipeline::ApplyFrameDropout(double DropoutProb)
{
	if (DropoutProb <= 0.0)
	{
		return false;
	}
	if (DropoutProb >= 1.0)
	{
		return true;
	}
	return Rng.NextUniform() < DropoutProb;
}

FSensorStageOutput FSensorPipeline::ProcessScan(
	const TArray<FSensorValue>& Truth,
	double Sigma,
	double DropoutProb,
	bool bClamp,
	double ClampMin,
	double ClampMax)
{
	// Fixed draw order for reproducibility: all noise draws first, then all
	// dropout draws. (Noise skips its draws entirely when Sigma == 0.)
	const TArray<FSensorValue> Noisy = AddGaussianNoise(Truth, Sigma, bClamp, ClampMin, ClampMax);
	return ApplyPerElementDropout(Noisy, DropoutProb);
}
