// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Rover/RoverConfig.h"

#include "Misc/FileHelper.h"

namespace
{
	// --- Minimal YAML-subset parser helpers -------------------------------

	/** Count leading spaces (indentation). Tabs are not expected in the config. */
	int32 IndentOf(const FString& Line)
	{
		int32 Count = 0;
		for (TCHAR C : Line)
		{
			if (C == TEXT(' '))
			{
				++Count;
			}
			else
			{
				break;
			}
		}
		return Count;
	}

	/** Strip a trailing `# comment` that is not inside brackets, and trim. */
	FString StripComment(const FString& InLine)
	{
		FString Line = InLine;
		int32 HashPos = INDEX_NONE;
		int32 BracketDepth = 0;
		for (int32 i = 0; i < Line.Len(); ++i)
		{
			const TCHAR C = Line[i];
			if (C == TEXT('[')) { ++BracketDepth; }
			else if (C == TEXT(']')) { BracketDepth = FMath::Max(0, BracketDepth - 1); }
			else if (C == TEXT('#') && BracketDepth == 0)
			{
				HashPos = i;
				break;
			}
		}
		if (HashPos != INDEX_NONE)
		{
			Line = Line.Left(HashPos);
		}
		return Line.TrimEnd();
	}

	/** True for blank or comment-only lines after stripping. */
	bool IsSkippable(const FString& Stripped)
	{
		return Stripped.TrimStartAndEnd().IsEmpty();
	}

	/** Parse an inline flow array `[a, b, c]` into doubles. */
	bool ParseInlineDoubleArray(const FString& InValue, TArray<double>& Out)
	{
		FString Value = InValue.TrimStartAndEnd();
		if (!Value.StartsWith(TEXT("[")) || !Value.EndsWith(TEXT("]")))
		{
			return false;
		}
		Value = Value.Mid(1, Value.Len() - 2);
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","), true);
		Out.Reset();
		for (FString& P : Parts)
		{
			P = P.TrimStartAndEnd();
			if (P.IsEmpty())
			{
				continue;
			}
			if (!P.IsNumeric())
			{
				return false;
			}
			Out.Add(FCString::Atod(*P));
		}
		return true;
	}

	FString Unquote(const FString& In)
	{
		FString S = In.TrimStartAndEnd();
		if (S.Len() >= 2 &&
			((S.StartsWith(TEXT("\"")) && S.EndsWith(TEXT("\""))) ||
			 (S.StartsWith(TEXT("'")) && S.EndsWith(TEXT("'")))))
		{
			return S.Mid(1, S.Len() - 2);
		}
		return S;
	}

	/** Split `key: value` on the first colon. Returns false if no colon. */
	bool SplitKeyValue(const FString& Line, FString& OutKey, FString& OutValue)
	{
		int32 ColonPos = INDEX_NONE;
		if (!Line.FindChar(TEXT(':'), ColonPos))
		{
			return false;
		}
		OutKey = Line.Left(ColonPos).TrimStartAndEnd();
		OutValue = Line.Mid(ColonPos + 1).TrimStartAndEnd();
		return true;
	}

	FLinearColor ColorFromArray(const TArray<double>& A)
	{
		const float R = A.Num() > 0 ? static_cast<float>(A[0]) : 0.5f;
		const float G = A.Num() > 1 ? static_cast<float>(A[1]) : 0.5f;
		const float B = A.Num() > 2 ? static_cast<float>(A[2]) : 0.5f;
		return FLinearColor(R, G, B, 1.0f);
	}

	FVector VectorFromArray(const TArray<double>& A, const FVector& Default)
	{
		FVector V = Default;
		if (A.Num() > 0) { V.X = A[0]; }
		if (A.Num() > 1) { V.Y = A[1]; }
		if (A.Num() > 2) { V.Z = A[2]; }
		return V;
	}

	// Parse a block sequence of maps starting after the `name:` line. Returns
	// the index of the first line NOT consumed by the sequence.
	int32 ParsePlacedActorList(
		const TArray<FString>& Lines,
		int32 StartIdx,
		int32 ParentIndent,
		TArray<FRoverPlacedActor>& OutActors)
	{
		int32 i = StartIdx;
		FRoverPlacedActor* Current = nullptr;
		int32 ItemIndent = INDEX_NONE;

		while (i < Lines.Num())
		{
			const FString& Raw = Lines[i];
			const FString Stripped = StripComment(Raw);
			if (IsSkippable(Stripped))
			{
				++i;
				continue;
			}

			const int32 Indent = IndentOf(Raw);
			if (Indent <= ParentIndent)
			{
				break; // dedent: this list is done
			}

			FString Content = Stripped.TrimStartAndEnd();
			if (Content.StartsWith(TEXT("- ")))
			{
				// New list item (a map). The dash sets the item indent.
				ItemIndent = Indent;
				OutActors.Add(FRoverPlacedActor());
				Current = &OutActors.Last();
				Content = Content.Mid(2).TrimStartAndEnd();
			}
			else if (Content == TEXT("-"))
			{
				ItemIndent = Indent;
				OutActors.Add(FRoverPlacedActor());
				Current = &OutActors.Last();
				++i;
				continue;
			}

			if (Current == nullptr)
			{
				break; // malformed; let caller surface the dedent
			}

			FString Key, Value;
			if (SplitKeyValue(Content, Key, Value))
			{
				if (Key == TEXT("name")) { Current->Name = Unquote(Value); }
				else if (Key == TEXT("shape")) { Current->Shape = Unquote(Value); }
				else if (Key == TEXT("pos_cm"))
				{
					TArray<double> A;
					if (ParseInlineDoubleArray(Value, A)) { Current->PositionCm = VectorFromArray(A, FVector::ZeroVector); }
				}
				else if (Key == TEXT("scale"))
				{
					TArray<double> A;
					if (ParseInlineDoubleArray(Value, A)) { Current->Scale = VectorFromArray(A, FVector(1, 1, 1)); }
				}
				else if (Key == TEXT("color"))
				{
					TArray<double> A;
					if (ParseInlineDoubleArray(Value, A)) { Current->Color = ColorFromArray(A); }
				}
			}
			++i;
		}
		return i;
	}
}

TArray<TPair<double, double>> FRoverWaypointRoute::GetWaypointsXYMeters() const
{
	TArray<TPair<double, double>> Out;
	Out.Reserve(RouteCm.Num());
	for (const TPair<double, double>& P : RouteCm)
	{
		Out.Add(TPair<double, double>(P.Key / 100.0, P.Value / 100.0));
	}
	return Out;
}

int32 FRoverLevelConfig::CountDistinguishableLandmarks() const
{
	TSet<FString> Keys;
	for (const FRoverPlacedActor& L : Landmarks)
	{
		// Quantize color to 2 decimals so "distinguishable" is tolerant of float noise.
		const FString Key = FString::Printf(TEXT("%s|%0.2f,%0.2f,%0.2f"),
			*L.Shape.ToLower(), L.Color.R, L.Color.G, L.Color.B);
		Keys.Add(Key);
	}
	return Keys.Num();
}

bool FRoverLevelConfig::MeetsMinimumContent(FString& OutReason) const
{
	if (Obstacles.Num() < 3)
	{
		OutReason = FString::Printf(TEXT("level needs >= 3 obstacles, found %d"), Obstacles.Num());
		return false;
	}
	if (Landmarks.Num() < 3)
	{
		OutReason = FString::Printf(TEXT("level needs >= 3 landmarks, found %d"), Landmarks.Num());
		return false;
	}
	if (CountDistinguishableLandmarks() < 3)
	{
		OutReason = FString::Printf(
			TEXT("level needs >= 3 distinguishable landmarks (by shape or color), only %d distinct"),
			CountDistinguishableLandmarks());
		return false;
	}
	OutReason.Reset();
	return true;
}

bool FRoverLevelConfig::ParseFromString(const FString& YamlText, FRoverLevelConfig& OutConfig, FString& OutError)
{
	OutConfig = FRoverLevelConfig();
	OutError.Reset();

	TArray<FString> Lines;
	YamlText.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

	int32 i = 0;
	while (i < Lines.Num())
	{
		const FString& Raw = Lines[i];
		const FString Stripped = StripComment(Raw);
		if (IsSkippable(Stripped))
		{
			++i;
			continue;
		}

		const int32 Indent = IndentOf(Raw);
		FString Key, Value;
		if (Indent == 0 && SplitKeyValue(Stripped.TrimStartAndEnd(), Key, Value))
		{
			if (Key == TEXT("ground"))
			{
				++i;
				while (i < Lines.Num())
				{
					const FString Sub = StripComment(Lines[i]);
					if (IsSkippable(Sub)) { ++i; continue; }
					if (IndentOf(Lines[i]) == 0) { break; }
					FString K, V;
					if (SplitKeyValue(Sub.TrimStartAndEnd(), K, V))
					{
						if (K == TEXT("size_m")) { OutConfig.Ground.SizeM = FCString::Atod(*V); }
						else if (K == TEXT("z_cm")) { OutConfig.Ground.ZCm = FCString::Atod(*V); }
					}
					++i;
				}
				continue;
			}
			else if (Key == TEXT("lighting"))
			{
				++i;
				FString Section; // "directional" or "sky"
				while (i < Lines.Num())
				{
					const FString Sub = StripComment(Lines[i]);
					if (IsSkippable(Sub)) { ++i; continue; }
					const int32 SubIndent = IndentOf(Lines[i]);
					if (SubIndent == 0) { break; }
					FString K, V;
					if (SplitKeyValue(Sub.TrimStartAndEnd(), K, V))
					{
						if (V.IsEmpty() && (K == TEXT("directional") || K == TEXT("sky")))
						{
							Section = K;
						}
						else if (Section == TEXT("directional"))
						{
							if (K == TEXT("pitch_deg")) { OutConfig.Lighting.DirectionalPitchDeg = FCString::Atod(*V); }
							else if (K == TEXT("yaw_deg")) { OutConfig.Lighting.DirectionalYawDeg = FCString::Atod(*V); }
							else if (K == TEXT("intensity_lux")) { OutConfig.Lighting.DirectionalIntensityLux = FCString::Atod(*V); }
						}
						else if (Section == TEXT("sky"))
						{
							if (K == TEXT("intensity")) { OutConfig.Lighting.SkyIntensity = FCString::Atod(*V); }
						}
					}
					++i;
				}
				continue;
			}
			else if (Key == TEXT("obstacles"))
			{
				i = ParsePlacedActorList(Lines, i + 1, 0, OutConfig.Obstacles);
				continue;
			}
			else if (Key == TEXT("landmarks"))
			{
				i = ParsePlacedActorList(Lines, i + 1, 0, OutConfig.Landmarks);
				continue;
			}
			else if (Key == TEXT("waypoints"))
			{
				++i;
				while (i < Lines.Num())
				{
					const FString Sub = StripComment(Lines[i]);
					if (IsSkippable(Sub)) { ++i; continue; }
					const int32 SubIndent = IndentOf(Lines[i]);
					if (SubIndent == 0) { break; }
					FString K, V;
					const FString SubContent = Sub.TrimStartAndEnd();
					if (SubContent.StartsWith(TEXT("- ")))
					{
						// A route point: `- [x, y]`
						TArray<double> A;
						if (ParseInlineDoubleArray(SubContent.Mid(2).TrimStartAndEnd(), A) && A.Num() >= 2)
						{
							OutConfig.Waypoints.RouteCm.Add(TPair<double, double>(A[0], A[1]));
						}
					}
					else if (SplitKeyValue(SubContent, K, V))
					{
						if (K == TEXT("arrival_tolerance_m")) { OutConfig.Waypoints.ArrivalToleranceM = FCString::Atod(*V); }
						else if (K == TEXT("waypoint_timeout_s")) { OutConfig.Waypoints.WaypointTimeoutS = FCString::Atod(*V); }
						// `route_cm:` header line (empty value) is skipped; items follow.
					}
					++i;
				}
				continue;
			}
		}
		++i;
	}

	if (OutConfig.Waypoints.RouteCm.Num() == 0)
	{
		OutError = TEXT("level config parsed but contains no waypoints route");
		return false;
	}
	return true;
}

bool FRoverLevelConfig::LoadFromFile(const FString& FilePath, FRoverLevelConfig& OutConfig, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		OutError = FString::Printf(TEXT("could not read level config file: %s"), *FilePath);
		return false;
	}
	return ParseFromString(Text, OutConfig, OutError);
}
