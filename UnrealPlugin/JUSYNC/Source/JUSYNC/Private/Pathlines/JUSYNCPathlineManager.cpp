#include "JUSYNCPathlineManager.h"

#include "Async/Async.h"
#include "Engine/World.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "JUSYNCPointCloudSpawner.h"
#include "Misc/ScopeLock.h"

#include <atomic>

namespace
{
	struct FPathlineKey
	{
		int32 Rank = 0;
		uint64 Id = 0;

		bool operator==(const FPathlineKey& Other) const
		{
			return Rank == Other.Rank && Id == Other.Id;
		}

		friend uint32 GetTypeHash(const FPathlineKey& Key)
		{
			uint64 H = Key.Id ^ (static_cast<uint64>(static_cast<uint32>(Key.Rank)) << 32);
			H ^= H >> 33;
			H *= 0xff51afd7ed558ccdull;
			H ^= H >> 33;
			return static_cast<uint32>(H);
		}
	};

	struct FPathlineNode
	{
		FVector Position = FVector::ZeroVector;
		float Scalar = 0.0f;
		float Width = 1.0f;
		bool bHasScalar = false;
		FColor Color = FColor::White;
		bool bHasColor = false;
		bool bBreak = false;
		uint64 SampleId = 0;
	};

	struct FPathlineTrack
	{
		FPathlineKey Key;
		TArray<FPathlineNode> Nodes;
		uint64 LastSeenCounter = 0;
	};

	struct FElementState
	{
		TMap<FPathlineKey, FPathlineTrack> Tracks;
		TMap<int32, TSet<FPathlineKey>> PreviousKeysByRank;
		TArray<FColor> Lut;
		uint64 SampleCounter = 0;
		uint64 LastSampleId = 0;
		bool bHasSample = false;
		bool bPendingBreak = false;
	};

	struct FSamplePoint
	{
		FVector Position = FVector::ZeroVector;
		float Scalar = 0.0f;
		float Width = 1.0f;
		bool bHasScalar = false;
		FColor Color = FColor::White;
		bool bHasColor = false;
	};

	struct FCandidate
	{
		FPathlineKey Key;
		uint64 Score = 0;
	};

	struct FWorkItem
	{
		enum class EType
		{
			Sample,
			RebuildAll
		};

		EType Type = EType::Sample;
		FString ElementName;
		FJUSYNCPointCloudRef Cloud;
		int32 Rank = 0;
		uint64 SampleId = 0;
		TArray<FColor> Lut;
		uint64 Generation = 0;
	};

	struct FPendingArrays
	{
		FString ElementName;
		TSharedPtr<FJUSYNCPathlineArrays, ESPMode::ThreadSafe> Arrays;
		uint64 Generation = 0;
	};

	uint64 Mix64(uint64 Value)
	{
		Value ^= Value >> 30;
		Value *= 0xbf58476d1ce4e5b9ull;
		Value ^= Value >> 27;
		Value *= 0x94d049bb133111ebull;
		Value ^= Value >> 31;
		return Value;
	}

	uint64 SelectionScore(const FString& ElementName, int32 Rank, uint64 Id, uint64 Seed)
	{
		uint64 Hash = Mix64(Seed ^ 0x9e3779b97f4a7c15ull);
		for (int32 i = 0; i < ElementName.Len(); ++i)
		{
			Hash = Mix64(Hash ^ static_cast<uint32>(ElementName[i]));
		}
		Hash = Mix64(Hash ^ static_cast<uint32>(Rank));
		Hash = Mix64(Hash ^ Id);
		return Hash;
	}

	uint64 DecodeParticleId(const FJUSYNCPointCloudData& Cloud, int32 Index,
		const FString& AttributeName, bool& bOutFromAttribute)
	{
		const FJUSYNCPointAttribute* Attribute = Cloud.FindAttribute(AttributeName);
		if (Attribute && Attribute->Components >= 1 && Attribute->Components <= 4)
		{
			const int64 Base = static_cast<int64>(Index) * Attribute->Components;
			if (Base >= 0 && Base + Attribute->Components <= Attribute->Values.Num())
			{
				uint64 Id = 0;
				for (int32 Component = 0; Component < Attribute->Components; ++Component)
				{
					const uint32 Chunk = static_cast<uint32>(
						FMath::RoundToInt(Attribute->Values[static_cast<int32>(Base) + Component]));
					Id |= static_cast<uint64>(Chunk & 0xFFFFu) << (16 * Component);
				}
				bOutFromAttribute = true;
				return Id;
			}
		}

		bOutFromAttribute = false;
		return static_cast<uint64>(static_cast<uint32>(Index));
	}

	const TArray<FColor>& DefaultScalarLut()
	{
		static const TArray<FColor> Ramp = []
		{
			const FLinearColor Stops[] = {
				FLinearColor(0.267f, 0.005f, 0.329f),
				FLinearColor(0.283f, 0.141f, 0.458f),
				FLinearColor(0.254f, 0.265f, 0.530f),
				FLinearColor(0.207f, 0.372f, 0.553f),
				FLinearColor(0.164f, 0.471f, 0.558f),
				FLinearColor(0.128f, 0.567f, 0.551f),
				FLinearColor(0.135f, 0.659f, 0.518f),
				FLinearColor(0.267f, 0.749f, 0.441f),
				FLinearColor(0.478f, 0.821f, 0.318f),
				FLinearColor(0.741f, 0.873f, 0.150f),
				FLinearColor(0.993f, 0.906f, 0.144f)
			};
			TArray<FColor> Out;
			Out.Reserve(UE_ARRAY_COUNT(Stops));
			for (const FLinearColor& Stop : Stops)
			{
				Out.Add(Stop.ToFColor(true));
			}
			return Out;
		}();
		return Ramp;
	}

	float HueDistance(float HueA, float HueB)
	{
		const float Delta = FMath::Abs(HueA - HueB);
		return FMath::Min(Delta, 360.0f - Delta);
	}

	void RgbToHS(const FLinearColor& Color, float& OutHue, float& OutSaturation)
	{
		const float Max = FMath::Max3(Color.R, Color.G, Color.B);
		const float Min = FMath::Min3(Color.R, Color.G, Color.B);
		const float Delta = Max - Min;

		OutSaturation = Max > KINDA_SMALL_NUMBER ? Delta / Max : 0.0f;

		if (Delta <= KINDA_SMALL_NUMBER)
		{
			OutHue = 0.0f;
			return;
		}
		if (Max == Color.R)
		{
			OutHue = 60.0f * FMath::Fmod((Color.G - Color.B) / Delta, 6.0f);
		}
		else if (Max == Color.G)
		{
			OutHue = 60.0f * ((Color.B - Color.R) / Delta + 2.0f);
		}
		else
		{
			OutHue = 60.0f * ((Color.R - Color.G) / Delta + 4.0f);
		}
		if (OutHue < 0.0f)
		{
			OutHue += 360.0f;
		}
	}

	FColor ResolveColor(const FElementState& Element, const FJUSYNCPathlineSettings& Settings,
		const FPathlineNode& Node)
	{
		FColor Flat = Settings.FlatColor.ToFColor(true);
		if (!Settings.bColorByScalar)
		{
			return Flat;
		}
		if (Node.bHasColor)
		{
			return Node.Color;
		}
		if (!Node.bHasScalar)
		{
			return Flat;
		}

		const TArray<FColor>& Lut = Element.Lut.Num() > 0 ? Element.Lut : DefaultScalarLut();
		const float Range = Settings.ColorMax - Settings.ColorMin;
		const float Alpha = FMath::IsNearlyZero(Range)
			? 0.0f
			: FMath::Clamp((Node.Scalar - Settings.ColorMin) / Range, 0.0f, 1.0f);
		const int32 Index = FMath::Clamp(
			FMath::RoundToInt(Alpha * static_cast<float>(Lut.Num() - 1)),
			0, Lut.Num() - 1);
		return Lut[Index];
	}
}

FString FJUSYNCPathlineManager::ResolveAttributeName(const FJUSYNCPointCloudData& Cloud,
	const FString& Name)
{
	if (Name.IsEmpty() || Cloud.FindAttribute(Name))
	{
		return Name;
	}
	const FString Prefixed = FString(TEXT("vtk_")) + Name;
	return Cloud.FindAttribute(Prefixed) ? Prefixed : Name;
}

struct FJUSYNCPathlineManager::FImpl
{
	FJUSYNCPathlineSettings Settings;
	FJUSYNCPathlineStats Stats;

	mutable FCriticalSection StateLock;
	FCriticalSection QueueLock;
	FCriticalSection PendingLock;

	TMap<FString, FElementState> Elements;
	TArray<TSharedPtr<FWorkItem>> Queue;
	TArray<FPendingArrays> PendingArrays;

	std::atomic<bool> bWorkerRunning{ false };
	std::atomic<bool> bShuttingDown{ false };
	std::atomic<uint64> Generation{ 0 };

	uint64 DisplaySampleId = TNumericLimits<uint64>::Max();
	bool bResampleRequested = false;
	bool bRebuildQueued = false;

	float TipBlendSeconds = 0.0f;
	float TipBlendAlpha = 0.0f;
	double TipBlendStartSeconds = 0.0;
	bool bTipBlendActive = false;

	void ClearLocked()
	{
		Elements.Reset();
		Stats = FJUSYNCPathlineStats();
		DisplaySampleId = TNumericLimits<uint64>::Max();
		bTipBlendActive = false;
		TipBlendAlpha = 0.0f;
		Generation.fetch_add(1);
	}

	void RefreshTotalsLocked()
	{
		Stats.ActiveTracks = 0;
		Stats.TotalNodes = 0;
		for (const TPair<FString, FElementState>& ElementPair : Elements)
		{
			Stats.ActiveTracks += ElementPair.Value.Tracks.Num();
			for (const TPair<FPathlineKey, FPathlineTrack>& TrackPair : ElementPair.Value.Tracks)
			{
				Stats.TotalNodes += TrackPair.Value.Nodes.Num();
			}
		}
	}

	void IngestLocked(const FString& ElementName, const FJUSYNCPointCloudRef& Cloud,
		int32 Rank, uint64 SampleId, const TArray<FColor>& Lut)
	{
		const double StartSeconds = FPlatformTime::Seconds();

		if (!Cloud.IsValid() || !Cloud->bFromRawGeo || !Cloud->bComplete ||
			Cloud->PointCount <= 0)
		{
			++Stats.IgnoredSamples;
			Stats.LastIngestMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
			return;
		}

		FElementState& Element = Elements.FindOrAdd(ElementName);
		if (!Element.bHasSample)
		{
			Element.bHasSample = true;
			Element.LastSampleId = SampleId;
		}
		else if (Element.LastSampleId != SampleId)
		{
			++Element.SampleCounter;
			Element.LastSampleId = SampleId;
		}
		const uint64 ThisCounter = Element.SampleCounter;
		Element.Lut = Lut;

		TMap<FPathlineKey, FSamplePoint> Current;
		Current.Reserve(Cloud->PointCount);

		const FString IdAttribute =
			FJUSYNCPathlineManager::ResolveAttributeName(*Cloud, Settings.IdAttribute);
		const FString ScalarAttribute =
			FJUSYNCPathlineManager::ResolveAttributeName(*Cloud, Settings.ScalarAttribute);

		int32 Duplicates = 0;
		for (int32 PointIndex = 0; PointIndex < Cloud->PointCount; ++PointIndex)
		{
			bool bFromAttribute = false;
			const uint64 Id = DecodeParticleId(*Cloud, PointIndex, IdAttribute, bFromAttribute);
			const FPathlineKey Key{ Rank, Id };
			if (Current.Contains(Key))
			{
				++Duplicates;
				continue;
			}

			FSamplePoint Point;
			Point.Position = Cloud->GetPosition(PointIndex);
			Point.Width = Cloud->GetWidth(PointIndex);
			Point.bHasColor = false;
			if (Cloud->bHasColors && Cloud->Colors.IsValidIndex(PointIndex))
			{
				Point.Color = Cloud->Colors[PointIndex];
				Point.bHasColor = true;
			}
			Point.bHasScalar = Cloud->GetAttributeScalar(
				PointIndex, ScalarAttribute, Settings.ScalarComponent, Point.Scalar);
			if (!FMath::IsFinite(Point.Scalar))
			{
				Point.bHasScalar = false;
				Point.Scalar = 0.0f;
			}
			Current.Add(Key, Point);
		}
		Stats.DuplicateIds += Duplicates;

		static std::atomic<bool> bScalarDiagLogged{ false };
		if (!bScalarDiagLogged.exchange(true))
		{
			bool bAnyScalar = false;
			float SampleValue = 0.0f;
			bool bAnyPointColor = false;
			FColor PointColor = FColor::Black;
			for (const TPair<FPathlineKey, FSamplePoint>& Pair : Current)
			{
				if (Pair.Value.bHasScalar)
				{
					bAnyScalar = true;
					SampleValue = Pair.Value.Scalar;
				}
				if (Pair.Value.bHasColor)
				{
					bAnyPointColor = true;
					PointColor = Pair.Value.Color;
				}
				if (bAnyScalar && bAnyPointColor)
				{
					break;
				}
			}
			const FString LutFirst = Element.Lut.Num() > 0
				? Element.Lut[0].ToString() : FString(TEXT("-"));
			const FString LutMid = Element.Lut.Num() > 1
				? Element.Lut[Element.Lut.Num() / 2].ToString() : FString(TEXT("-"));
			const TCHAR* Verdict = !Settings.bColorByScalar ? TEXT("flat")
				: (bAnyPointColor ? TEXT("POINT-COLOURED")
					: (!bAnyScalar ? TEXT("FLAT WHITE: no scalar, no point colours")
								   : TEXT("scalar-coloured")));
			UE_LOG(LogTemp, Display,
				TEXT("[Pathlines] color diag: pointColor=%d point=%s scalar='%s' present=%d "
					 "value=%.4f lut=%d first=%s mid=%s (trails will be %s)"),
				bAnyPointColor ? 1 : 0, *PointColor.ToString(),
				*ScalarAttribute, bAnyScalar ? 1 : 0, SampleValue, Element.Lut.Num(),
				*LutFirst, *LutMid, Verdict);
		}

		const int32 ExpirySamples = FMath::Max(0, Settings.TrackExpirySamples);
		for (auto It = Element.Tracks.CreateIterator(); It; ++It)
		{
			if (It.Key().Rank == Rank &&
				!Current.Contains(It.Key()) &&
				ThisCounter - It.Value().LastSeenCounter > static_cast<uint64>(ExpirySamples))
			{
				It.RemoveCurrent();
			}
		}

		TArray<FCandidate> Candidates;
		Candidates.Reserve(Current.Num() + Element.Tracks.Num());
		for (const TPair<FPathlineKey, FSamplePoint>& Pair : Current)
		{
			Candidates.Add({ Pair.Key,
				SelectionScore(ElementName, Pair.Key.Rank, Pair.Key.Id, Settings.SelectionSeed) });
		}
		for (const TPair<FPathlineKey, FPathlineTrack>& Pair : Element.Tracks)
		{
			if (!Current.Contains(Pair.Key))
			{
				Candidates.Add({ Pair.Key,
					SelectionScore(ElementName, Pair.Key.Rank, Pair.Key.Id, Settings.SelectionSeed) });
			}
		}

		Candidates.Sort([](const FCandidate& A, const FCandidate& B)
		{
			if (A.Score != B.Score)
			{
				return A.Score > B.Score;
			}
			if (A.Key.Id != B.Key.Id)
			{
				return A.Key.Id < B.Key.Id;
			}
			return A.Key.Rank < B.Key.Rank;
		});

		int32 SelectCount = FMath::Min(FMath::Max(0, Settings.MaxTracks), Candidates.Num());
		if (Settings.MaxNodes > 0)
		{
			const int32 BudgetTracks = Settings.MaxNodes / 2;
			SelectCount = FMath::Min(SelectCount, BudgetTracks);
		}

		TMap<FPathlineKey, FPathlineTrack> NextTracks;
		NextTracks.Reserve(SelectCount);
		for (int32 CandidateIndex = 0; CandidateIndex < SelectCount; ++CandidateIndex)
		{
			const FPathlineKey Key = Candidates[CandidateIndex].Key;
			FPathlineTrack Track;
			if (FPathlineTrack* Existing = Element.Tracks.Find(Key))
			{
				Track = MoveTemp(*Existing);
			}
			Track.Key = Key;

			if (const FSamplePoint* Point = Current.Find(Key))
			{
				const bool bWasPrevious = Element.PreviousKeysByRank.Contains(Rank) &&
					Element.PreviousKeysByRank[Rank].Contains(Key);
				const bool bBreak = Element.bPendingBreak ||
					(Track.Nodes.Num() > 0 && !bWasPrevious);
				const bool bSameSample = Track.Nodes.Num() > 0 &&
					Track.Nodes.Last().SampleId == SampleId;
				if (bSameSample)
				{
					FPathlineNode& Last = Track.Nodes.Last();
					Last.Position = Point->Position;
					Last.Scalar = Point->Scalar;
					Last.Width = Point->Width;
					Last.bHasScalar = Point->bHasScalar;
					Last.Color = Point->Color;
					Last.bHasColor = Point->bHasColor;
					Last.bBreak = Last.bBreak || bBreak;
				}
				else
				{
					FPathlineNode Node;
					Node.Position = Point->Position;
					Node.Scalar = Point->Scalar;
					Node.Width = Point->Width;
					Node.bHasScalar = Point->bHasScalar;
					Node.Color = Point->Color;
					Node.bHasColor = Point->bHasColor;
					Node.SampleId = SampleId;
					Node.bBreak = bBreak;
					Track.Nodes.Add(Node);
				}
				Track.LastSeenCounter = ThisCounter;
			}

			NextTracks.Add(Key, MoveTemp(Track));
		}
		Element.Tracks = MoveTemp(NextTracks);

		int32 EffectiveHistory = FMath::Max(2, FMath::Max(Settings.HistorySamples, Settings.RetentionSamples));
		if (Settings.MaxNodes > 0 && Element.Tracks.Num() > 0)
		{
			EffectiveHistory = FMath::Clamp(
				Settings.MaxNodes / Element.Tracks.Num(), 2, EffectiveHistory);
		}
		for (TPair<FPathlineKey, FPathlineTrack>& Pair : Element.Tracks)
		{
			if (Pair.Value.Nodes.Num() > EffectiveHistory)
			{
				Pair.Value.Nodes.RemoveAt(0,
					Pair.Value.Nodes.Num() - EffectiveHistory, EAllowShrinking::No);
			}
		}

		if (Settings.MaxNodes > 0)
		{
			int64 TotalNodes = 0;
			for (const TPair<FPathlineKey, FPathlineTrack>& Pair : Element.Tracks)
			{
				TotalNodes += Pair.Value.Nodes.Num();
			}
			while (TotalNodes > Settings.MaxNodes)
			{
				FPathlineTrack* Victim = nullptr;
				for (TPair<FPathlineKey, FPathlineTrack>& Pair : Element.Tracks)
				{
					if (!Victim || Pair.Value.Nodes.Num() > Victim->Nodes.Num())
					{
						Victim = &Pair.Value;
					}
				}
				if (!Victim || Victim->Nodes.Num() <= 1)
				{
					break;
				}
				Victim->Nodes.RemoveAt(0, 1, EAllowShrinking::No);
				--TotalNodes;
			}
		}

		TSet<FPathlineKey>& RankPreviousKeys = Element.PreviousKeysByRank.FindOrAdd(Rank);
		RankPreviousKeys.Reset();
		for (const TPair<FPathlineKey, FSamplePoint>& Pair : Current)
		{
			RankPreviousKeys.Add(Pair.Key);
		}
		Element.bPendingBreak = false;

		RefreshTotalsLocked();
		Stats.LastIngestMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	}

	bool BuildArraysLocked(const FString& ElementName, uint64 CutoffSampleId,
		FJUSYNCPathlineArrays& OutArrays, int32& OutVisibleTracks)
	{
		const double StartSeconds = FPlatformTime::Seconds();
		OutArrays.Reset();
		OutVisibleTracks = 0;

		const FElementState* Element = Elements.Find(ElementName);
		if (!Element || !Settings.bEnabled)
		{
			Stats.LastBuildMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
			return false;
		}

		const float BaseRadius = FMath::Max(0.001f, Settings.Radius * Settings.RadiusScale);
		const float ColorRange = Settings.ColorMax - Settings.ColorMin;
		const float TipBlend = FMath::Clamp(TipBlendAlpha, 0.0f, 1.0f);

		TArray<const FPathlineNode*> Visible;
		Visible.Reserve(Settings.HistorySamples + 4);

		float Order = 0.0f;
		bool bHasPreviousRun = false;
		FVector PreviousPosition = FVector::ZeroVector;
		FLinearColor PreviousColor = FLinearColor::White;

		auto ResolvePoint = [&](const FPathlineNode& Node, float& OutWidth,
			FLinearColor& OutColor, float& OutScalar)
		{
			float WidthScale = 1.0f;
			if (Settings.bUsePointWidth && Node.Width > 0.0f)
			{
				WidthScale = FMath::Clamp(Node.Width, 0.1f, 8.0f);
			}
			OutWidth = FMath::Max(BaseRadius * 2.0f * WidthScale, 0.01f);

			OutScalar = 0.0f;
			if (Node.bHasScalar && !FMath::IsNearlyZero(ColorRange))
			{
				OutScalar = FMath::Clamp(
					(Node.Scalar - Settings.ColorMin) / ColorRange, 0.0f, 1.0f);
			}

			OutColor = FLinearColor(ResolveColor(*Element, Settings, Node));
		};

		auto EmitPoint = [&](const FVector& Position, float Width,
			const FLinearColor& Color, float Scalar)
		{
			OutArrays.Positions.Add(Position);
			OutArrays.LinkOrders.Add(Order);
			OutArrays.Widths.Add(Width);
			OutArrays.Colors.Add(Color);
			OutArrays.Scalars.Add(Scalar);
			Order += 1.0f;
		};

		auto EmitRun = [&](int32 StartIndex, int32 EndIndex,
			const FPathlineNode* TipNext, float TipBlendAmount)
		{
			if (EndIndex <= StartIndex)
			{
				return;
			}

			float FirstWidth = 0.0f;
			float FirstScalar = 0.0f;
			FLinearColor FirstColor = FLinearColor::White;
			ResolvePoint(*Visible[StartIndex], FirstWidth, FirstColor, FirstScalar);

			if (bHasPreviousRun)
			{
				EmitPoint(PreviousPosition, 0.0f, PreviousColor, 0.0f);
				EmitPoint(Visible[StartIndex]->Position, 0.0f, FirstColor, 0.0f);
			}

			for (int32 NodeIndex = StartIndex; NodeIndex <= EndIndex; ++NodeIndex)
			{
				const FPathlineNode& Node = *Visible[NodeIndex];
				float Width = 0.0f;
				float Scalar = 0.0f;
				FLinearColor Color = FLinearColor::White;
				ResolvePoint(Node, Width, Color, Scalar);

				FVector Position = Node.Position;
				if (TipNext && TipBlendAmount > 0.0f && !TipNext->bBreak &&
					NodeIndex == Visible.Num() - 1)
				{
					Position = FMath::Lerp(Node.Position, TipNext->Position, TipBlendAmount);
				}
				EmitPoint(Position, Width, Color, Scalar);
			}

			PreviousPosition = Visible[EndIndex]->Position;
			if (TipNext && TipBlendAmount > 0.0f && EndIndex == Visible.Num() - 1)
			{
				PreviousPosition = FMath::Lerp(Visible[EndIndex]->Position, TipNext->Position, TipBlendAmount);
			}
			float UnusedWidth = 0.0f;
			float UnusedScalar = 0.0f;
			ResolvePoint(*Visible[EndIndex], UnusedWidth, PreviousColor, UnusedScalar);
			bHasPreviousRun = true;
		};

		for (const TPair<FPathlineKey, FPathlineTrack>& TrackPair : Element->Tracks)
		{
			const FPathlineTrack& Track = TrackPair.Value;
			Visible.Reset();
			const FPathlineNode* TipNext = nullptr;
			for (const FPathlineNode& Node : Track.Nodes)
			{
				if (Node.SampleId <= CutoffSampleId)
				{
					Visible.Add(&Node);
				}
				else if (!TipNext)
				{
					TipNext = &Node;
					break;
				}
			}
			if (Visible.Num() > Settings.HistorySamples)
			{
				Visible.RemoveAt(0, Visible.Num() - Settings.HistorySamples);
			}
			if (Visible.Num() == 0)
			{
				continue;
			}

			const FPathlineNode& Latest = *Visible.Last();
			if (Settings.bFilterEnabled)
			{
				if (!Latest.bHasScalar ||
					Latest.Scalar < Settings.FilterMin ||
					Latest.Scalar > Settings.FilterMax)
				{
					continue;
				}
			}
			if (Settings.bFilterByHue)
			{
				float Hue = 0.0f;
				float Saturation = 0.0f;
				RgbToHS(FLinearColor(ResolveColor(*Element, Settings, Latest)), Hue, Saturation);
				if (HueDistance(Hue, Settings.FilterHue) > Settings.FilterHueTolerance)
				{
					continue;
				}
			}

			++OutVisibleTracks;
			if (Visible.Num() < 2)
			{
				continue;
			}

			int32 RunStart = 0;
			for (int32 VisibleIndex = 1; VisibleIndex < Visible.Num(); ++VisibleIndex)
			{
				if (Visible[VisibleIndex]->bBreak)
				{
					EmitRun(RunStart, VisibleIndex - 1, nullptr, 0.0f);
					RunStart = VisibleIndex;
				}
			}
			EmitRun(RunStart, Visible.Num() - 1, TipNext, TipBlend);
		}

		Stats.VisibleTracks = OutVisibleTracks;
		Stats.LastBuildMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		return true;
	}

	void PostArrays(const FString& ElementName, FJUSYNCPathlineArrays&& Arrays,
		uint64 ItemGeneration)
	{
		if (ItemGeneration != Generation.load())
		{
			return;
		}

		TSharedPtr<FJUSYNCPathlineArrays, ESPMode::ThreadSafe> Shared =
			MakeShared<FJUSYNCPathlineArrays, ESPMode::ThreadSafe>();
		*Shared = MoveTemp(Arrays);

		FScopeLock Lock(&PendingLock);
		if (ItemGeneration != Generation.load())
		{
			return;
		}
		FPendingArrays Pending;
		Pending.ElementName = ElementName;
		Pending.Arrays = Shared;
		Pending.Generation = ItemGeneration;
		PendingArrays.Add(MoveTemp(Pending));
	}

	void BuildElementAndPost(const FString& ElementName, uint64 ItemGeneration)
	{
		FJUSYNCPathlineArrays Arrays;
		int32 VisibleTracks = 0;
		bool bFound = false;
		{
			FScopeLock Lock(&StateLock);
			bFound = BuildArraysLocked(ElementName, DisplaySampleId, Arrays, VisibleTracks);
		}
		if (bFound)
		{
			PostArrays(ElementName, MoveTemp(Arrays), ItemGeneration);
		}
	}

	void RebuildAllAndPost(uint64 ItemGeneration)
	{
		TArray<TPair<FString, FJUSYNCPathlineArrays>> LocalArrays;
		{
			FScopeLock Lock(&StateLock);
			if (!Settings.bEnabled)
			{
				return;
			}
			for (const TPair<FString, FElementState>& Pair : Elements)
			{
				FJUSYNCPathlineArrays Arrays;
				int32 VisibleTracks = 0;
				if (BuildArraysLocked(Pair.Key, DisplaySampleId, Arrays, VisibleTracks))
				{
					LocalArrays.Emplace(Pair.Key, MoveTemp(Arrays));
				}
			}
		}

		for (TPair<FString, FJUSYNCPathlineArrays>& Pair : LocalArrays)
		{
			PostArrays(Pair.Key, MoveTemp(Pair.Value), ItemGeneration);
		}
	}

	void ProcessItem(const TSharedPtr<FWorkItem>& Item)
	{
		if (!Item || Item->Generation != Generation.load())
		{
			return;
		}

		if (Item->Type == FWorkItem::EType::RebuildAll)
		{
			bRebuildQueued = false;
			RebuildAllAndPost(Item->Generation);
			return;
		}

		IngestSampleNow(Item->ElementName, Item->Cloud, Item->Rank, Item->SampleId, Item->Lut,
			Item->Generation);
		if (Item->Generation != Generation.load())
		{
			return;
		}
		BuildElementAndPost(Item->ElementName, Item->Generation);
	}

	void IngestSampleNow(const FString& ElementName, const FJUSYNCPointCloudRef& Cloud,
		int32 Rank, uint64 SampleId, const TArray<FColor>& Lut, uint64 ExpectedGeneration)
	{
		FScopeLock Lock(&StateLock);
		if (!Settings.bEnabled || ExpectedGeneration != Generation.load())
		{
			return;
		}
		IngestLocked(ElementName, Cloud, Rank, SampleId, Lut);
	}

	void EnqueueWorkItem(const TSharedPtr<FWorkItem>& Item)
	{
		bool bStartWorker = false;
		{
			FScopeLock Lock(&QueueLock);
			Queue.Add(Item);
			if (!bWorkerRunning.exchange(true))
			{
				bStartWorker = true;
			}
		}
		if (bStartWorker)
		{
			AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]()
			{
				WorkerLoop();
			});
		}
	}

	void QueueRebuildAll()
	{
		bool bHasWork = false;
		{
			FScopeLock Lock(&StateLock);
			bHasWork = Settings.bEnabled && Elements.Num() > 0;
		}
		if (!bHasWork)
		{
			return;
		}

		if (bRebuildQueued)
		{
			return;
		}
		bRebuildQueued = true;

		TSharedPtr<FWorkItem> Item = MakeShared<FWorkItem>();
		Item->Type = FWorkItem::EType::RebuildAll;
		Item->Generation = Generation.load();
		EnqueueWorkItem(Item);
	}

	void WorkerLoop()
	{
		for (;;)
		{
			TSharedPtr<FWorkItem> Item;
			{
				FScopeLock Lock(&QueueLock);
				if (bShuttingDown.load() || Queue.Num() == 0)
				{
					bWorkerRunning.store(false);
					break;
				}
				Item = Queue[0];
				Queue.RemoveAt(0, 1, EAllowShrinking::No);
			}
			ProcessItem(Item);
		}
	}
};

FJUSYNCPathlineManager::FJUSYNCPathlineManager()
	: Impl(MakeUnique<FImpl>())
{
}

FJUSYNCPathlineManager::~FJUSYNCPathlineManager()
{
	Impl->bShuttingDown.store(true);
	{
		FScopeLock Lock(&Impl->QueueLock);
		Impl->Queue.Reset();
	}
	while (Impl->bWorkerRunning.load())
	{
		FPlatformProcess::Sleep(0.001f);
	}
}

void FJUSYNCPathlineManager::SetSettings(const FJUSYNCPathlineSettings& NewSettings)
{
	bool bRebuild = false;
	{
		FScopeLock Lock(&Impl->StateLock);
		const bool bWasEnabled = Impl->Settings.bEnabled;
		const bool bStructural =
			NewSettings.IdAttribute != Impl->Settings.IdAttribute ||
			NewSettings.ScalarAttribute != Impl->Settings.ScalarAttribute ||
			NewSettings.ScalarComponent != Impl->Settings.ScalarComponent ||
			NewSettings.MaxTracks != Impl->Settings.MaxTracks ||
			NewSettings.HistorySamples != Impl->Settings.HistorySamples ||
			NewSettings.MaxNodes != Impl->Settings.MaxNodes ||
			NewSettings.TrackExpirySamples != Impl->Settings.TrackExpirySamples ||
			NewSettings.SelectionSeed != Impl->Settings.SelectionSeed;
		const bool bVisual =
			NewSettings.NiagaraSystem != Impl->Settings.NiagaraSystem ||
			NewSettings.Radius != Impl->Settings.Radius ||
			NewSettings.RadiusScale != Impl->Settings.RadiusScale ||
			NewSettings.bUsePointWidth != Impl->Settings.bUsePointWidth ||
			NewSettings.bFilterEnabled != Impl->Settings.bFilterEnabled ||
			NewSettings.FilterMin != Impl->Settings.FilterMin ||
			NewSettings.FilterMax != Impl->Settings.FilterMax ||
			NewSettings.bFilterByHue != Impl->Settings.bFilterByHue ||
			NewSettings.FilterHue != Impl->Settings.FilterHue ||
			NewSettings.FilterHueTolerance != Impl->Settings.FilterHueTolerance ||
			NewSettings.bColorByScalar != Impl->Settings.bColorByScalar ||
			NewSettings.ColorMin != Impl->Settings.ColorMin ||
			NewSettings.ColorMax != Impl->Settings.ColorMax ||
			NewSettings.FlatColor != Impl->Settings.FlatColor ||
			NewSettings.Material != Impl->Settings.Material;

		Impl->Settings = NewSettings;

		if (!NewSettings.bEnabled)
		{
			if (bWasEnabled || Impl->Elements.Num() > 0)
			{
				Impl->ClearLocked();
			}
			Impl->bResampleRequested = false;
		}
		else if (!bWasEnabled || bStructural)
		{
			Impl->ClearLocked();
			Impl->bResampleRequested = true;
			bRebuild = Impl->Elements.Num() > 0;
		}
		else if (bVisual)
		{
			bRebuild = Impl->Elements.Num() > 0;
		}
	}

	if (bRebuild)
	{
		Impl->QueueRebuildAll();
	}
}

const FJUSYNCPathlineSettings& FJUSYNCPathlineManager::GetSettings() const
{
	return Impl->Settings;
}

bool FJUSYNCPathlineManager::ConsumeResampleRequest()
{
	FScopeLock Lock(&Impl->StateLock);
	const bool bRequested = Impl->bResampleRequested;
	Impl->bResampleRequested = false;
	return bRequested;
}

void FJUSYNCPathlineManager::Reset()
{
	{
		FScopeLock Lock(&Impl->StateLock);
		Impl->ClearLocked();
		Impl->bResampleRequested = false;
	}
	{
		FScopeLock Lock(&Impl->QueueLock);
		Impl->Queue.Reset();
	}
	{
		FScopeLock Lock(&Impl->PendingLock);
		Impl->PendingArrays.Reset();
	}
}

void FJUSYNCPathlineManager::RemoveElement(const FString& ElementName)
{
	bool bRemoved = false;
	{
		FScopeLock Lock(&Impl->StateLock);
		bRemoved = Impl->Elements.Remove(ElementName) > 0;
		if (bRemoved)
		{
			Impl->RefreshTotalsLocked();
			Impl->Generation.fetch_add(1);
		}
	}
	if (bRemoved)
	{
		Impl->QueueRebuildAll();
	}
}

void FJUSYNCPathlineManager::EnqueueSample(const FString& ElementName,
	const FJUSYNCPointCloudRef& Cloud, int32 Rank, uint64 SampleId, const TArray<FColor>& Lut)
{
	if (!Cloud.IsValid())
	{
		FScopeLock Lock(&Impl->StateLock);
		++Impl->Stats.IgnoredSamples;
		return;
	}

	TSharedPtr<FWorkItem> Item = MakeShared<FWorkItem>();
	{
		FScopeLock Lock(&Impl->StateLock);
		if (!Impl->Settings.bEnabled)
		{
			return;
		}
		Item->Generation = Impl->Generation.load();
	}
	Item->Type = FWorkItem::EType::Sample;
	Item->ElementName = ElementName;
	Item->Cloud = Cloud;
	Item->Rank = Rank;
	Item->SampleId = SampleId;
	Item->Lut = Lut;

	bool bDropped = false;
	bool bStartWorker = false;
	{
		FScopeLock Lock(&Impl->QueueLock);
		if (Impl->Queue.Num() >= 64)
		{
			bDropped = true;
		}
		else
		{
			Impl->Queue.Add(Item);
			if (!Impl->bWorkerRunning.exchange(true))
			{
				bStartWorker = true;
			}
		}
	}

	if (bDropped)
	{
		FScopeLock Lock(&Impl->StateLock);
		for (TPair<FString, FElementState>& Pair : Impl->Elements)
		{
			Pair.Value.bPendingBreak = true;
		}
		++Impl->Stats.DroppedSamples;
		return;
	}

	if (bStartWorker)
	{
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]()
		{
			Impl->WorkerLoop();
		});
	}
}

void FJUSYNCPathlineManager::IngestSampleNow(const FString& ElementName,
	const FJUSYNCPointCloudRef& Cloud, int32 Rank, uint64 SampleId, const TArray<FColor>& Lut)
{
	Impl->IngestSampleNow(ElementName, Cloud, Rank, SampleId, Lut,
		Impl->Generation.load());
}

bool FJUSYNCPathlineManager::BuildArrays(const FString& ElementName,
	uint64 CutoffSampleId, FJUSYNCPathlineArrays& OutArrays, int32& OutVisibleTracks)
{
	FScopeLock Lock(&Impl->StateLock);
	return Impl->BuildArraysLocked(ElementName, CutoffSampleId, OutArrays, OutVisibleTracks);
}

void FJUSYNCPathlineManager::SetDisplaySampleId(uint64 SampleId)
{
	bool bChanged = false;
	{
		FScopeLock Lock(&Impl->StateLock);
		if (Impl->DisplaySampleId != SampleId)
		{
			Impl->DisplaySampleId = SampleId;
			Impl->TipBlendAlpha = 0.0f;
			Impl->bTipBlendActive =
				Impl->TipBlendSeconds > 0.0f && SampleId != TNumericLimits<uint64>::Max();
			Impl->TipBlendStartSeconds = FPlatformTime::Seconds();
			bChanged = true;
		}
	}
	if (bChanged)
	{
		Impl->QueueRebuildAll();
	}
}

uint64 FJUSYNCPathlineManager::GetDisplaySampleId() const
{
	FScopeLock Lock(&Impl->StateLock);
	return Impl->DisplaySampleId;
}

void FJUSYNCPathlineManager::SetDisplayTipBlendSeconds(float Seconds)
{
	FScopeLock Lock(&Impl->StateLock);
	Impl->TipBlendSeconds = FMath::Clamp(Seconds, 0.0f, 4.0f);
	if (Impl->TipBlendSeconds <= 0.0f)
	{
		Impl->bTipBlendActive = false;
		Impl->TipBlendAlpha = 0.0f;
	}
}

void FJUSYNCPathlineManager::TickDisplayBlend()
{
	bool bNeedsRebuild = false;
	{
		FScopeLock Lock(&Impl->StateLock);
		if (Impl->bTipBlendActive)
		{
			const float Elapsed = static_cast<float>(FPlatformTime::Seconds() - Impl->TipBlendStartSeconds);
			const float U = FMath::Clamp(Elapsed / FMath::Max(Impl->TipBlendSeconds, 0.001f), 0.0f, 1.0f);
			const float Alpha = U * U * (3.0f - 2.0f * U);
			if (Alpha >= 1.0f)
			{
				Impl->TipBlendAlpha = 1.0f;
				Impl->bTipBlendActive = false;
				bNeedsRebuild = true;
			}
			else if (!FMath::IsNearlyEqual(Alpha, Impl->TipBlendAlpha, 0.004f))
			{
				Impl->TipBlendAlpha = Alpha;
				bNeedsRebuild = true;
			}
		}
	}
	if (bNeedsRebuild)
	{
		Impl->QueueRebuildAll();
	}
}

void FJUSYNCPathlineManager::RequestRebuild()
{
	Impl->QueueRebuildAll();
}

void FJUSYNCPathlineManager::DrainArrays(TArray<FCompletedArrays>& OutArrays)
{
	FScopeLock Lock(&Impl->PendingLock);
	OutArrays.Reset();
	OutArrays.Reserve(Impl->PendingArrays.Num());

	const uint64 CurrentGeneration = Impl->Generation.load();
	for (FPendingArrays& Pending : Impl->PendingArrays)
	{
		if (Pending.Generation != CurrentGeneration || !Pending.Arrays.IsValid())
		{
			continue;
		}
		FCompletedArrays Completed;
		Completed.ElementName = MoveTemp(Pending.ElementName);
		Completed.Arrays = MoveTemp(Pending.Arrays);
		OutArrays.Add(MoveTemp(Completed));
	}
	Impl->PendingArrays.Reset();
}

FJUSYNCPathlineStats FJUSYNCPathlineManager::GetStats() const
{
	FScopeLock Lock(&Impl->StateLock);
	FJUSYNCPathlineStats Out = Impl->Stats;
	Out.ActiveTracks = 0;
	Out.TotalNodes = 0;
	for (const TPair<FString, FElementState>& ElementPair : Impl->Elements)
	{
		Out.ActiveTracks += ElementPair.Value.Tracks.Num();
		for (const TPair<FPathlineKey, FPathlineTrack>& TrackPair : ElementPair.Value.Tracks)
		{
			Out.TotalNodes += TrackPair.Value.Nodes.Num();
		}
	}
	return Out;
}

TArray<FString> FJUSYNCPathlineManager::GetElementKeys() const
{
	FScopeLock Lock(&Impl->StateLock);
	TArray<FString> Keys;
	Impl->Elements.GetKeys(Keys);
	return Keys;
}

TArray<uint64> FJUSYNCPathlineManager::GetTrackIds(const FString& ElementName, int32 Rank) const
{
	FScopeLock Lock(&Impl->StateLock);
	TArray<uint64> Ids;
	if (const FElementState* Element = Impl->Elements.Find(ElementName))
	{
		Ids.Reserve(Element->Tracks.Num());
		for (const TPair<FPathlineKey, FPathlineTrack>& Pair : Element->Tracks)
		{
			if (Pair.Key.Rank == Rank)
			{
				Ids.Add(Pair.Key.Id);
			}
		}
	}
	Ids.Sort();
	return Ids;
}

int32 FJUSYNCPathlineManager::GetElementNodeCount(const FString& ElementName) const
{
	FScopeLock Lock(&Impl->StateLock);
	int32 Count = 0;
	if (const FElementState* Element = Impl->Elements.Find(ElementName))
	{
		for (const TPair<FPathlineKey, FPathlineTrack>& Pair : Element->Tracks)
		{
			Count += Pair.Value.Nodes.Num();
		}
	}
	return Count;
}

int32 FJUSYNCPathlineManager::GetTrackNodeCount(const FString& ElementName,
	int32 Rank, uint64 Id) const
{
	FScopeLock Lock(&Impl->StateLock);
	if (const FElementState* Element = Impl->Elements.Find(ElementName))
	{
		if (const FPathlineTrack* Track = Element->Tracks.Find(FPathlineKey{ Rank, Id }))
		{
			return Track->Nodes.Num();
		}
	}
	return 0;
}
