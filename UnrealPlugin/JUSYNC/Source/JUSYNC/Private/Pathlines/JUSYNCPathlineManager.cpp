#include "JUSYNCPathlineManager.h"

#include "Async/Async.h"
#include "Engine/World.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "JUSYNCPathlineActor.h"
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
		// One previous-sample key set per source rank. Ranks arrive as separate
		// clouds for the same logical sample, so a global set would mark the
		// other ranks' tracks as gaps.
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

	struct FPendingMesh
	{
		FString ElementName;
		FJUSYNCCompactMeshRef Mesh;
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

	FColor ResolveColor(const FElementState& Element, const FJUSYNCPathlineSettings& Settings,
		const FPathlineNode& Latest)
	{
		FColor Flat = Settings.FlatColor.ToFColor(true);
		if (!Settings.bColorByScalar || !Latest.bHasScalar || Element.Lut.Num() == 0)
		{
			return Flat;
		}

		const float Range = Settings.ColorMax - Settings.ColorMin;
		const float Alpha = FMath::IsNearlyZero(Range)
			? 0.0f
			: FMath::Clamp((Latest.Scalar - Settings.ColorMin) / Range, 0.0f, 1.0f);
		const int32 Index = FMath::Clamp(
			FMath::RoundToInt(Alpha * static_cast<float>(Element.Lut.Num() - 1)),
			0, Element.Lut.Num() - 1);
		return Element.Lut[Index];
	}
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
	TArray<FPendingMesh> PendingMeshes;

	// Game-thread only. Never touched by the worker.
	TMap<FString, TWeakObjectPtr<AJUSYNCPathlineActor>> PathlineActors;

	std::atomic<bool> bWorkerRunning{ false };
	std::atomic<bool> bShuttingDown{ false };
	std::atomic<uint64> Generation{ 0 };

	uint64 DisplaySampleId = TNumericLimits<uint64>::Max();
	bool bResampleRequested = false;

	void ClearLocked()
	{
		Elements.Reset();
		Stats = FJUSYNCPathlineStats();
		DisplaySampleId = TNumericLimits<uint64>::Max();
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
		// A logical sample can span multiple rank clouds. Advance the absence
		// clock and retain the last sample id only when the sample id changes.
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

		int32 Duplicates = 0;
		for (int32 PointIndex = 0; PointIndex < Cloud->PointCount; ++PointIndex)
		{
			bool bFromAttribute = false;
			const uint64 Id = DecodeParticleId(*Cloud, PointIndex, Settings.IdAttribute, bFromAttribute);
			const FPathlineKey Key{ Rank, Id };
			if (Current.Contains(Key))
			{
				++Duplicates;
				continue;
			}

			FSamplePoint Point;
			Point.Position = Cloud->GetPosition(PointIndex);
			Point.Width = Cloud->GetWidth(PointIndex);
			Point.bHasScalar = Cloud->GetAttributeScalar(
				PointIndex, Settings.ScalarAttribute, Settings.ScalarComponent, Point.Scalar);
			if (!FMath::IsFinite(Point.Scalar))
			{
				Point.bHasScalar = false;
				Point.Scalar = 0.0f;
			}
			Current.Add(Key, Point);
		}
		Stats.DuplicateIds += Duplicates;

		// Expire tracks before selection so vacant slots can be reused this
		// sample. Only the rank that just emitted can prove an id is absent;
		// a rank cloud that has not arrived yet must not age its tracks.
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
			// A drawable track needs at least two nodes.
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
					// A delayed re-delivery of the same committed sample must
					// replace the node instead of appending a zero-length step.
					FPathlineNode& Last = Track.Nodes.Last();
					Last.Position = Point->Position;
					Last.Scalar = Point->Scalar;
					Last.Width = Point->Width;
					Last.bHasScalar = Point->bHasScalar;
					Last.bBreak = Last.bBreak || bBreak;
				}
				else
				{
					FPathlineNode Node;
					Node.Position = Point->Position;
					Node.Scalar = Point->Scalar;
					Node.Width = Point->Width;
					Node.bHasScalar = Point->bHasScalar;
					Node.SampleId = SampleId;
					Node.bBreak = bBreak;
					Track.Nodes.Add(Node);
				}
				Track.LastSeenCounter = ThisCounter;
			}

			NextTracks.Add(Key, MoveTemp(Track));
		}
		Element.Tracks = MoveTemp(NextTracks);

		int32 EffectiveHistory = FMath::Max(2, Settings.HistorySamples);
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

		// Defensive global cap. With MaxNodes smaller than twice the track
		// count the per-track clamp cannot satisfy the budget on its own.
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

	bool BuildLocked(const FString& ElementName, uint64 CutoffSampleId,
		FJUSYNCCompactMeshData& OutMesh, int32& OutVisibleTracks)
	{
		const double StartSeconds = FPlatformTime::Seconds();
		OutMesh = FJUSYNCCompactMeshData();
		OutMesh.ElementName = ElementName;
		OutMesh.TypeName = TEXT("Pathlines");
		OutVisibleTracks = 0;

		const FElementState* Element = Elements.Find(ElementName);
		if (!Element || !Settings.bEnabled)
		{
			Stats.LastBuildMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
			return false;
		}

		const int32 Sides = FMath::Clamp(Settings.TubeSides, 3, 16);
		const float BaseRadius = FMath::Max(0.001f, Settings.Radius * Settings.RadiusScale);

		for (const TPair<FPathlineKey, FPathlineTrack>& TrackPair : Element->Tracks)
		{
			const FPathlineTrack& Track = TrackPair.Value;
			TArray<const FPathlineNode*> Visible;
			Visible.Reserve(Track.Nodes.Num());
			for (const FPathlineNode& Node : Track.Nodes)
			{
				if (Node.SampleId <= CutoffSampleId)
				{
					Visible.Add(&Node);
				}
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

			++OutVisibleTracks;
			if (Visible.Num() < 2)
			{
				continue;
			}

			const FColor Color = ResolveColor(*Element, Settings, Latest);
			auto AddRun = [&](int32 StartIndex, int32 EndIndex)
			{
				for (int32 NodeIndex = StartIndex + 1; NodeIndex <= EndIndex; ++NodeIndex)
				{
					const FPathlineNode& A = *Visible[NodeIndex - 1];
					const FPathlineNode& B = *Visible[NodeIndex];
					const FVector Delta = B.Position - A.Position;
					if (Delta.SizeSquared() <= SMALL_NUMBER)
					{
						continue;
					}

					const FVector Direction = Delta.GetSafeNormal();
					const FVector Reference = FMath::Abs(Direction.Z) < 0.9
						? FVector::UpVector : FVector::RightVector;
					const FVector Side = FVector::CrossProduct(Direction, Reference).GetSafeNormal();
					const FVector Up = FVector::CrossProduct(Side, Direction).GetSafeNormal();

					float Radius = BaseRadius;
					if (Settings.bUsePointWidth)
					{
						const float Width = 0.5f * (A.Width + B.Width);
						if (Width > 0.0f)
						{
							Radius *= Width;
						}
					}
					Radius = FMath::Max(Radius, 0.001f);

					const int32 BaseIndex = OutMesh.Points.Num();
					for (int32 Ring = 0; Ring < 2; ++Ring)
					{
						const FVector Center = Ring == 0 ? A.Position : B.Position;
						for (int32 SideIndex = 0; SideIndex < Sides; ++SideIndex)
						{
							const float Angle = 2.0f * PI * static_cast<float>(SideIndex) /
								static_cast<float>(Sides);
							const FVector Offset = (FMath::Cos(Angle) * Side + FMath::Sin(Angle) * Up) * Radius;
							const FVector Position = Center + Offset;
							OutMesh.Points.Add(FVector3f(
								static_cast<float>(Position.X),
								static_cast<float>(Position.Y),
								static_cast<float>(Position.Z)));
							const FVector Normal = Offset.GetSafeNormal();
							OutMesh.Normals.Add(FVector3f(
								static_cast<float>(Normal.X),
								static_cast<float>(Normal.Y),
								static_cast<float>(Normal.Z)));
							OutMesh.UVs.Add(FVector2f(
								static_cast<float>(SideIndex) / static_cast<float>(Sides),
								static_cast<float>(Ring)));
							OutMesh.VertexColors.Add(Color);
						}
					}

					const int32 Ring0 = BaseIndex;
					const int32 Ring1 = BaseIndex + Sides;
					for (int32 SideIndex = 0; SideIndex < Sides; ++SideIndex)
					{
						const int32 Next = (SideIndex + 1) % Sides;
						OutMesh.Triangles.Add(Ring0 + SideIndex);
						OutMesh.Triangles.Add(Ring1 + SideIndex);
						OutMesh.Triangles.Add(Ring0 + Next);

						OutMesh.Triangles.Add(Ring0 + Next);
						OutMesh.Triangles.Add(Ring1 + SideIndex);
						OutMesh.Triangles.Add(Ring1 + Next);
					}
				}
			};

			int32 RunStart = 0;
			for (int32 VisibleIndex = 1; VisibleIndex < Visible.Num(); ++VisibleIndex)
			{
				if (Visible[VisibleIndex]->bBreak)
				{
					AddRun(RunStart, VisibleIndex - 1);
					RunStart = VisibleIndex;
				}
			}
			AddRun(RunStart, Visible.Num() - 1);
		}

		OutMesh.bHasBakedColors = OutMesh.VertexColors.Num() > 0;
		Stats.VisibleTracks = OutVisibleTracks;
		Stats.LastBuildMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		return true;
	}

	void PostMesh(const FString& ElementName, FJUSYNCCompactMeshData&& Mesh, uint64 ItemGeneration)
	{
		if (ItemGeneration != Generation.load())
		{
			return;
		}

		FJUSYNCCompactMeshRef MeshRef = MakeShared<FJUSYNCCompactMeshData, ESPMode::ThreadSafe>();
		*MeshRef = MoveTemp(Mesh);

		FScopeLock Lock(&PendingLock);
		if (ItemGeneration != Generation.load())
		{
			return;
		}
		FPendingMesh Pending;
		Pending.ElementName = ElementName;
		Pending.Mesh = MeshRef;
		Pending.Generation = ItemGeneration;
		PendingMeshes.Add(MoveTemp(Pending));
	}

	void BuildElementAndPost(const FString& ElementName, uint64 ItemGeneration)
	{
		FJUSYNCCompactMeshData Mesh;
		int32 VisibleTracks = 0;
		bool bFound = false;
		{
			FScopeLock Lock(&StateLock);
			bFound = BuildLocked(ElementName, DisplaySampleId, Mesh, VisibleTracks);
		}
		if (bFound)
		{
			PostMesh(ElementName, MoveTemp(Mesh), ItemGeneration);
		}
	}

	void RebuildAllAndPost(uint64 ItemGeneration)
	{
		TArray<TPair<FString, FJUSYNCCompactMeshData>> LocalMeshes;
		{
			FScopeLock Lock(&StateLock);
			if (!Settings.bEnabled)
			{
				return;
			}
			for (const TPair<FString, FElementState>& Pair : Elements)
			{
				FJUSYNCCompactMeshData Mesh;
				int32 VisibleTracks = 0;
				if (BuildLocked(Pair.Key, DisplaySampleId, Mesh, VisibleTracks))
				{
					LocalMeshes.Emplace(Pair.Key, MoveTemp(Mesh));
				}
			}
		}

		for (TPair<FString, FJUSYNCCompactMeshData>& Pair : LocalMeshes)
		{
			PostMesh(Pair.Key, MoveTemp(Pair.Value), ItemGeneration);
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
			NewSettings.TubeSides != Impl->Settings.TubeSides ||
			NewSettings.Radius != Impl->Settings.Radius ||
			NewSettings.RadiusScale != Impl->Settings.RadiusScale ||
			NewSettings.bUsePointWidth != Impl->Settings.bUsePointWidth ||
			NewSettings.bFilterEnabled != Impl->Settings.bFilterEnabled ||
			NewSettings.FilterMin != Impl->Settings.FilterMin ||
			NewSettings.FilterMax != Impl->Settings.FilterMax ||
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
		Impl->PendingMeshes.Reset();
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
			// Invalidate queued work/pending meshes for the retired element.
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

bool FJUSYNCPathlineManager::BuildGeometry(const FString& ElementName,
	uint64 CutoffSampleId, FJUSYNCCompactMeshData& OutMesh, int32& OutVisibleTracks)
{
	FScopeLock Lock(&Impl->StateLock);
	return Impl->BuildLocked(ElementName, CutoffSampleId, OutMesh, OutVisibleTracks);
}

void FJUSYNCPathlineManager::SetDisplaySampleId(uint64 SampleId)
{
	bool bChanged = false;
	{
		FScopeLock Lock(&Impl->StateLock);
		if (Impl->DisplaySampleId != SampleId)
		{
			Impl->DisplaySampleId = SampleId;
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

void FJUSYNCPathlineManager::RequestRebuild()
{
	Impl->QueueRebuildAll();
}

void FJUSYNCPathlineManager::PumpGameThread(UWorld* World,
	FJUSYNCPointCloudSpawner* Spawner, UMaterialInterface* Material)
{
	if (!IsInGameThread())
	{
		return;
	}

	TArray<FPendingMesh> Pending;
	{
		FScopeLock Lock(&Impl->PendingLock);
		Pending = MoveTemp(Impl->PendingMeshes);
	}

	bool bEnabled = false;
	UMaterialInterface* SettingsMaterial = nullptr;
	TSet<FString> LiveElements;
	{
		FScopeLock Lock(&Impl->StateLock);
		bEnabled = Impl->Settings.bEnabled;
		SettingsMaterial = Impl->Settings.Material;
		for (const TPair<FString, FElementState>& Pair : Impl->Elements)
		{
			LiveElements.Add(Pair.Key);
		}
	}

	UMaterialInterface* UseMaterial = Material ? Material : SettingsMaterial;
	const FVector SpawnLocation = Spawner
		? Spawner->GetSpawnLocation() : FVector::ZeroVector;
	const float SpawnScale = Spawner ? Spawner->GetSpawnScale() : 1.0f;
	const FVector SpawnScale3D(SpawnScale);

	if (!bEnabled)
	{
		for (auto It = Impl->PathlineActors.CreateIterator(); It; ++It)
		{
			if (AJUSYNCPathlineActor* Actor = It.Value().Get())
			{
				Actor->ClearMesh();
				Actor->Destroy();
			}
			It.RemoveCurrent();
		}
		return;
	}

	// Remove actors for elements that no longer exist. This also handles Reset
	// and structural re-sampling: the map is pruned before new actors arrive.
	for (auto It = Impl->PathlineActors.CreateIterator(); It; ++It)
	{
		AJUSYNCPathlineActor* Actor = It.Value().Get();
		if (!Actor || !LiveElements.Contains(It.Key()))
		{
			if (Actor)
			{
				Actor->ClearMesh();
				Actor->Destroy();
			}
			It.RemoveCurrent();
		}
		else
		{
			Actor->SetActorLocation(SpawnLocation);
			Actor->SetActorScale3D(SpawnScale3D);
		}
	}

	for (FPendingMesh& Result : Pending)
	{
		if (Result.Generation != Impl->Generation.load() ||
			!LiveElements.Contains(Result.ElementName) ||
			!Result.Mesh.IsValid())
		{
			continue;
		}

		AJUSYNCPathlineActor* Actor = nullptr;
		if (TWeakObjectPtr<AJUSYNCPathlineActor>* Existing =
			Impl->PathlineActors.Find(Result.ElementName))
		{
			Actor = Existing->Get();
		}

		if (!Actor && World)
		{
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParameters.ObjectFlags |= RF_Transient;
			Actor = World->SpawnActor<AJUSYNCPathlineActor>(
				AJUSYNCPathlineActor::StaticClass(), FTransform::Identity, SpawnParameters);
			if (Actor)
			{
				Impl->PathlineActors.Add(Result.ElementName, Actor);
			}
		}

		if (Actor)
		{
			Actor->SetActorLocation(SpawnLocation);
			Actor->SetActorScale3D(SpawnScale3D);
			Actor->ApplyMesh(*Result.Mesh, UseMaterial);
		}
	}
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
