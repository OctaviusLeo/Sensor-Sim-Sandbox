// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Pipeline/SensorConfigValidation.h"

#include <cmath>

FSensorBoundSet& FSensorBoundSet::Add(
	const FString& ParameterName,
	double Min,
	double Max,
	bool bMinInclusive,
	bool bMaxInclusive,
	bool bIntegerOnly)
{
	FBound Bound;
	Bound.Name = ParameterName;
	Bound.Min = Min;
	Bound.Max = Max;
	Bound.bMinInclusive = bMinInclusive;
	Bound.bMaxInclusive = bMaxInclusive;
	Bound.bIntegerOnly = bIntegerOnly;
	Bounds.Add(Bound);
	return *this;
}

FString FSensorBoundSet::FormatBound(const FBound& Bound)
{
	const TCHAR* LeftBracket = Bound.bMinInclusive ? TEXT("[") : TEXT("(");
	const TCHAR* RightBracket = Bound.bMaxInclusive ? TEXT("]") : TEXT(")");
	return FString::Printf(TEXT("%s%g, %g%s"), LeftBracket, Bound.Min, Bound.Max, RightBracket);
}

FSensorConfigValidationResult FSensorBoundSet::CheckBound(const FBound& Bound, double Value)
{
	// Reject non-finite candidates outright (NaN/Inf never satisfy a bound).
	if (!FMath::IsFinite(Value))
	{
		return FSensorConfigValidationResult::Invalid(
			Bound.Name,
			FString::Printf(TEXT("parameter '%s' value %g is not a finite number; expected within %s"),
				*Bound.Name, Value, *FormatBound(Bound)));
	}

	if (Bound.bIntegerOnly && std::floor(Value) != Value)
	{
		return FSensorConfigValidationResult::Invalid(
			Bound.Name,
			FString::Printf(TEXT("parameter '%s' value %g must be an integer"),
				*Bound.Name, Value));
	}

	const bool bAboveMin = Bound.bMinInclusive ? (Value >= Bound.Min) : (Value > Bound.Min);
	const bool bBelowMax = Bound.bMaxInclusive ? (Value <= Bound.Max) : (Value < Bound.Max);
	if (!bAboveMin || !bBelowMax)
	{
		return FSensorConfigValidationResult::Invalid(
			Bound.Name,
			FString::Printf(TEXT("parameter '%s' value %g is out of bounds; expected within %s"),
				*Bound.Name, Value, *FormatBound(Bound)));
	}

	return FSensorConfigValidationResult::Valid();
}

FSensorConfigValidationResult FSensorBoundSet::Check(const FString& ParameterName, double Value) const
{
	for (const FBound& Bound : Bounds)
	{
		if (Bound.Name == ParameterName)
		{
			return CheckBound(Bound, Value);
		}
	}

	return FSensorConfigValidationResult::Invalid(
		ParameterName,
		FString::Printf(TEXT("parameter '%s' has no declared bound"), *ParameterName));
}

FSensorConfigValidationResult FSensorBoundSet::Validate(const TMap<FString, double>& Candidate) const
{
	// Walk bounds in declared order so the first violation is deterministic.
	for (const FBound& Bound : Bounds)
	{
		const double* Value = Candidate.Find(Bound.Name);
		if (Value == nullptr)
		{
			return FSensorConfigValidationResult::Invalid(
				Bound.Name,
				FString::Printf(TEXT("parameter '%s' is missing from the candidate configuration"),
					*Bound.Name));
		}

		const FSensorConfigValidationResult Result = CheckBound(Bound, *Value);
		if (!Result.bValid)
		{
			return Result;
		}
	}

	return FSensorConfigValidationResult::Valid();
}
