// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Pipeline/PipelineSelfCheck.h"

#include "Pipeline/SensorPipeline.h"
#include "Pipeline/SensorConfigValidation.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogPipelineSelfCheck, Log, All);

namespace
{
	// A fixed synthetic scan: a mix of present ranges and no-return gaps, so
	// the self-check exercises both the valued and empty paths deterministically.
	TArray<FSensorValue> MakeSyntheticTruth()
	{
		TArray<FSensorValue> Truth;
		const int32 NumRays = 512;
		Truth.Reserve(NumRays);
		for (int32 i = 0; i < NumRays; ++i)
		{
			if (i % 17 == 0)
			{
				Truth.Add(FSensorValue()); // periodic no-return
			}
			else
			{
				// Deterministic range profile, no RNG involved.
				const double R = 5.0 + 0.1 * static_cast<double>(i % 97);
				Truth.Add(FSensorValue(R));
			}
		}
		return Truth;
	}

	// Serialize a stage output's published values + dropped flags into a stable
	// string with no timestamps or paths, for byte-identical comparison.
	FString SerializeOutput(const FSensorStageOutput& Output)
	{
		FString S;
		S.Reserve(Output.Published.Num() * 24);
		for (int32 i = 0; i < Output.Published.Num(); ++i)
		{
			if (Output.Published[i].IsSet())
			{
				// Hex of the raw bits so the comparison is exact (no rounding).
				const double V = Output.Published[i].GetValue();
				uint64 Bits;
				FMemory::Memcpy(&Bits, &V, sizeof(double));
				S += FString::Printf(TEXT("%d:v=%016llx:d=%d|"), i, Bits, Output.bDropped[i] ? 1 : 0);
			}
			else
			{
				S += FString::Printf(TEXT("%d:null:d=%d|"), i, Output.bDropped[i] ? 1 : 0);
			}
		}
		return S;
	}

	// One deterministic pipeline run over the synthetic scan.
	FString RunOnce(int64 RunSeed)
	{
		const FString SensorName = TEXT("selfcheck");
		const double Sigma = 0.02;        // within lidar range_noise_sigma_m bound
		const double DropoutProb = 0.10;  // within dropout_prob bound

		FSensorPipeline Pipeline(RunSeed, SensorName);
		const TArray<FSensorValue> Truth = MakeSyntheticTruth();
		const FSensorStageOutput Output = Pipeline.ProcessScan(Truth, Sigma, DropoutProb);
		return SerializeOutput(Output);
	}

	// Exercise the shared config-bounds validation contract: an in-bound config
	// is accepted; an out-of-bound value is rejected and names the parameter;
	// the previously accepted config is retained.
	bool CheckValidationContract(FString& OutDetail)
	{
		FSensorBoundSet Bounds;
		Bounds
			.Add(TEXT("range_noise_sigma_m"), 0.0, 10.0)
			.Add(TEXT("dropout_prob"), 0.0, 1.0)
			.Add(TEXT("latency_ms"), 0.0, 1000.0, /*minInc*/ true, /*maxInc*/ true, /*intOnly*/ true);

		// Last valid config, retained across a rejected update.
		TMap<FString, double> LastValid;
		LastValid.Add(TEXT("range_noise_sigma_m"), 0.02);
		LastValid.Add(TEXT("dropout_prob"), 0.10);
		LastValid.Add(TEXT("latency_ms"), 200.0);

		const FSensorConfigValidationResult Accept = Bounds.Validate(LastValid);
		if (!Accept.bValid)
		{
			OutDetail = FString::Printf(TEXT("in-bound config unexpectedly rejected: %s"), *Accept.ErrorMessage);
			return false;
		}

		// Candidate with dropout_prob out of bounds (1.5 > 1.0).
		TMap<FString, double> BadCandidate = LastValid;
		BadCandidate[TEXT("dropout_prob")] = 1.5;

		const FSensorConfigValidationResult Reject = Bounds.Validate(BadCandidate);
		if (Reject.bValid)
		{
			OutDetail = TEXT("out-of-bound config was not rejected");
			return false;
		}
		if (Reject.OffendingParameter != TEXT("dropout_prob"))
		{
			OutDetail = FString::Printf(TEXT("rejection named '%s', expected 'dropout_prob'"), *Reject.OffendingParameter);
			return false;
		}

		// Last valid config is retained: it still validates unchanged.
		const FSensorConfigValidationResult StillValid = Bounds.Validate(LastValid);
		if (!StillValid.bValid)
		{
			OutDetail = TEXT("last valid config no longer validates after a rejected update");
			return false;
		}

		OutDetail = FString::Printf(TEXT("rejected '%s' (%s); last valid config retained"),
			*Reject.OffendingParameter, *Reject.ErrorMessage);
		return true;
	}
}

FPipelineSelfCheck::FResult FPipelineSelfCheck::Run(int64 RunSeed)
{
	FResult Result;

	const FString First = RunOnce(RunSeed);
	const FString Second = RunOnce(RunSeed);
	Result.bDeterministic = (First == Second);

	FString ValidationDetail;
	Result.bValidationContractHolds = CheckValidationContract(ValidationDetail);

	Result.Detail = FString::Printf(
		TEXT("determinism=%s (output %d bytes), validation=%s [%s]"),
		Result.bDeterministic ? TEXT("PASS") : TEXT("FAIL"),
		First.Len(),
		Result.bValidationContractHolds ? TEXT("PASS") : TEXT("FAIL"),
		*ValidationDetail);

	return Result;
}

// ---------------------------------------------------------------------------
// Console command: `PipelineSelfCheck [seed]`
//
// One-command headless determinism + validation self-check for task 5.1.
// Logs a single PASS/FAIL line the verify script can grep.
// ---------------------------------------------------------------------------
static void ExecutePipelineSelfCheck(const TArray<FString>& Args)
{
	int64 Seed = 1234;
	if (Args.Num() >= 1 && !Args[0].IsEmpty())
	{
		Seed = FCString::Atoi64(*Args[0]);
	}

	const FPipelineSelfCheck::FResult Result = FPipelineSelfCheck::Run(Seed);

	UE_LOG(LogPipelineSelfCheck, Log,
		TEXT("PipelineSelfCheck %s (seed=%lld): %s"),
		Result.Passed() ? TEXT("PASS") : TEXT("FAIL"), Seed, *Result.Detail);
}

static FAutoConsoleCommand GPipelineSelfCheckCommand(
	TEXT("PipelineSelfCheck"),
	TEXT("Run the shared SensorModel pipeline twice with the same seed/config and verify byte-identical output plus the config-bounds validation contract. Usage: PipelineSelfCheck [seed]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&ExecutePipelineSelfCheck));
