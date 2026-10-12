#include "JUSYNCPointCloudSpawner.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/Engine.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformTime.h"

#ifdef WITH_ANARI_USD_MIDDLEWARE
#include "LidarPointCloud.h"
#include "LidarPointCloudComponent.h"
#include "LidarPointCloudActor.h"

// 1 (default): streamed clouds build their octree on Epic's async import thread
// (ULidarPointCloud::CreateFromData + FLidarPointCloudAsyncParameters) and swap
// into the component on completion - the game thread never sits in the multi-second
// SetData. 0: legacy in-place SetData on the game thread.
static TAutoConsoleVariable<int32> CVarJusyncAsyncCloudBuild(
    TEXT("jusync.AsyncCloudBuild"), 1,
    TEXT("Build streamed point clouds off the game thread (Epic async CreateFromData) and swap on completion."),
    ECVF_Default);
#endif

namespace
{
    // ULidarPointCloud refuses a cloud whose bounds are degenerate on any axis
    // (min == max) - e.g. genuinely planar simulation data (a z=0 slice / flat
    // sheet), which otherwise silently spawns as an EMPTY actor with
    // "Provided bounds are incorrect". Nudge a single point per flat axis by a
    // sub-visual epsilon so the octree can be built. 0.01 cm is invisible.
    void JUSYNCPadDegenerateBounds(TArray64<FLidarPointCloudPoint>& Points)
    {
        if (Points.Num() == 0)
        {
            return;
        }

        float MinX = FLT_MAX, MinY = FLT_MAX, MinZ = FLT_MAX;
        float MaxX = -FLT_MAX, MaxY = -FLT_MAX, MaxZ = -FLT_MAX;
        for (const FLidarPointCloudPoint& P : Points)
        {
            MinX = FMath::Min(MinX, (float)P.Location.X);
            MinY = FMath::Min(MinY, (float)P.Location.Y);
            MinZ = FMath::Min(MinZ, (float)P.Location.Z);
            MaxX = FMath::Max(MaxX, (float)P.Location.X);
            MaxY = FMath::Max(MaxY, (float)P.Location.Y);
            MaxZ = FMath::Max(MaxZ, (float)P.Location.Z);
        }

        constexpr float Eps = 0.01f;
        float PadX = (MaxX - MinX <= 1e-4f) ? Eps : 0.0f;
        float PadY = (MaxY - MinY <= 1e-4f) ? Eps : 0.0f;
        float PadZ = (MaxZ - MinZ <= 1e-4f) ? Eps : 0.0f;
        if (PadX != 0.0f || PadY != 0.0f || PadZ != 0.0f)
        {
            FLidarPointCloudPoint& P = Points[Points.Num() - 1];
            P.Location.X += PadX;
            P.Location.Y += PadY;
            P.Location.Z += PadZ;
        }
    }
} // namespace

FJUSYNCPointCloudSpawner::FJUSYNCPointCloudSpawner(TWeakObjectPtr<UObject> InOwner)
    : Owner(InOwner), MaxPoolSize(8), SpawnLocation(FVector::ZeroVector),
      SpawnScale(1.0f), BudgetMs(16.0f)
{
}

FJUSYNCPointCloudSpawner::~FJUSYNCPointCloudSpawner()
{
    AsyncBuildsAlive->store(false);
}

bool FJUSYNCPointCloudSpawner::BeginTask()
{
    if (bShuttingDown.load())
    {
        return false;
    }
    InFlightTasks.fetch_add(1);
    return true;
}

void FJUSYNCPointCloudSpawner::EndTask()
{
    InFlightTasks.fetch_sub(1);
}

void FJUSYNCPointCloudSpawner::WaitForCompletion(float TimeoutSeconds)
{
    bShuttingDown.store(true);
    const double Start = FPlatformTime::Seconds();
    while ((InFlightTasks.load() > 0 || AsyncBuildsOutstanding->load() > 0)
        && (FPlatformTime::Seconds() - Start) < FMath::Max(0.0f, TimeoutSeconds))
    {
        FPlatformProcess::Sleep(0.001f);
    }
}

void FJUSYNCPointCloudSpawner::SetGradientLUT(const TArray<FColor>& InLUT)
{
    TArray<FPendingNoColorCloud> ToFlush;
    {
        FScopeLock Lock(&GradientMutex);
        GradientLUT = InLUT;
        ++LUTVersion;
    }
    {
        FScopeLock Lock(&QueueMutex);
        ToFlush = MoveTemp(PendingNoColorClouds);
        PendingNoColorClouds.Empty();
    }

    for (FPendingNoColorCloud& Pending : ToFlush)
    {
        EnqueuePointCloud(MoveTemp(Pending.Data), Pending.Rank);
    }
}

void FJUSYNCPointCloudSpawner::FlushExpiredNoColorClouds()
{
    const TArray<FColor> LUT = GetGradientLUT();
    const bool bLUTReady = LUT.Num() > 0;

    TArray<FPendingNoColorCloud> ToFlush;
    {
        FScopeLock Lock(&QueueMutex);
        if (PendingNoColorClouds.Num() == 0)
        {
            return;
        }

        const double Now = FPlatformTime::Seconds();
        TArray<FPendingNoColorCloud> Remaining;
        for (FPendingNoColorCloud& Pending : PendingNoColorClouds)
        {
            if (bLUTReady || (Now - Pending.QueuedTime) > 2.0)
            {
                ToFlush.Add(MoveTemp(Pending));
            }
            else
            {
                Remaining.Add(MoveTemp(Pending));
            }
        }
        PendingNoColorClouds = MoveTemp(Remaining);
    }

    for (FPendingNoColorCloud& Pending : ToFlush)
    {
        // Expired entries spawn WITHOUT the gradient rather than ping-ponging
        // in the queue forever (which also pins PendingAsyncPCS and blocks
        // initial-spawn completion / live updates).
        EnqueuePointCloud(MoveTemp(Pending.Data), Pending.Rank, /*bAllowNoGradient=*/!bLUTReady);
    }
}

void FJUSYNCPointCloudSpawner::EnqueuePointCloud(FJUSYNCPointCloudRef&& PCRef, int32 InRank)
{
    if (!PCRef || !PCRef->IsValid())
    {
        return;
    }

    if (PCRef.IsUnique())
    {
        FJUSYNCPointCloudData Owned = MoveTemp(*PCRef);
        EnqueuePointCloud(MoveTemp(Owned), InRank);
        return;
    }

    FJUSYNCPointCloudData Copy = *PCRef;
    EnqueuePointCloud(MoveTemp(Copy), InRank);
}

void FJUSYNCPointCloudSpawner::EnqueuePointCloud(const FJUSYNCPointCloudData& PCData, int32 InRank)
{
    // Copy once, then move into the async task via the rvalue overload.
    EnqueuePointCloud(FJUSYNCPointCloudData(PCData), InRank);
}

void FJUSYNCPointCloudSpawner::EnqueuePointCloud(FJUSYNCPointCloudData&& PCData, int32 InRank, bool bAllowNoGradient)
{
    if (!PCData.IsValid() || !Owner.IsValid() || bShuttingDown.load()) return;

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (PCData.HasLidarPoints())
    {
        const int32 DirectPointCount = PCData.PointCount;
        const bool bHasBakedColors = PCData.HasBakedColors();

        if (!bAllowNoGradient && !bHasBakedColors && PCData.Widths.Num() > 0)
        {
            const TArray<FColor> LocalLUT = GetGradientLUT();
            if (LocalLUT.Num() == 0)
            {
                FScopeLock Lock(&QueueMutex);
                FPendingNoColorCloud Pending;
                Pending.Data = MoveTemp(PCData);
                Pending.Rank = InRank;
                Pending.QueuedTime = FPlatformTime::Seconds();
                const FString ElementNameForLog = Pending.Data.ElementName;
                PendingNoColorClouds.Add(MoveTemp(Pending));
                UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: delaying PC '%s' for gradient LUT"), *ElementNameForLog);
                return;
            }
        }

        if (!BeginTask())
        {
            return;
        }

        UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: queued PC '%s' for direct LiDAR spawn (%d points)"),
               *PCData.ElementName, DirectPointCount);

        auto* DirectPointsPtr = new TArray64<FLidarPointCloudPoint>(MoveTemp(PCData.LidarPoints));
        auto* DirectWidthsPtr = new TArray<float>(MoveTemp(PCData.Widths));

        AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
            [this, DirectPointCount, bHasBakedColors,
             DirectPointsPtr, DirectWidthsPtr,
             ElementName = MoveTemp(PCData.ElementName),
             InRank]()
        {
            TArray64<FLidarPointCloudPoint>& Points = *DirectPointsPtr;
            TArray<float>& Widths = *DirectWidthsPtr;

            if (!bHasBakedColors && Widths.Num() > 0)
            {
                TArray<FColor> LocalLUT;
                {
                    FScopeLock Lock(&this->GradientMutex);
                    LocalLUT = this->GradientLUT;
                }

                if (LocalLUT.Num() > 0)
                {
                    // ParallelFor'd: per-point LUT lookup writes only Points[i].
                    ParallelFor(static_cast<int32>(Points.Num()), [&](int32 i)
                    {
                        const float Attr0 = (i < Widths.Num()) ? Widths[i] : 0.0f;
                        const int32 LUTIdx = FMath::Clamp(
                            FMath::RoundToInt(Attr0 * static_cast<float>(LocalLUT.Num() - 1)),
                            0,
                            LocalLUT.Num() - 1
                        );
                        Points[i].Color = LocalLUT[LUTIdx];
                    });
                }
            }

            FPointCloudReadyEntry Entry;
            Entry.Points = MoveTemp(Points);
            Entry.ElementName = ElementName;
            Entry.Key = ElementName.IsEmpty()
                ? FString::Printf(TEXT("jusync_pc_r%d"), InRank)
                : FString::Printf(TEXT("%s_r%d"), *ElementName, InRank);
            Entry.Rank = InRank;
            Entry.PointSize = PointSize;
            Entry.bSpawned = false;
            Entry.bNeedsRecolor = false;

            delete DirectPointsPtr;
            delete DirectWidthsPtr;

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [this, Entry = MoveTemp(Entry)]() mutable
                {
                    {
                        FScopeLock Lock(&QueueMutex);
                        ReadyQueue.Add(MoveTemp(Entry));

                        if (ReadyQueue.Num() >= 8)
                        {
                            DrainReadyQueueLocked();
                        }
                    }
                },
                TStatId(), nullptr, ENamedThreads::GameThread);

            EndTask();
        });
        return;
    }
#endif

    const bool bLegacyHasColors = PCData.HasColors();
    if (!bAllowNoGradient && !bLegacyHasColors && PCData.Widths.Num() > 0)
    {
        const TArray<FColor> LocalLUT = GetGradientLUT();
        if (LocalLUT.Num() == 0)
        {
            FScopeLock Lock(&QueueMutex);
            FPendingNoColorCloud Pending;
            Pending.Data = MoveTemp(PCData);
            Pending.Rank = InRank;
            Pending.QueuedTime = FPlatformTime::Seconds();
            const FString ElementNameForLog = Pending.Data.ElementName;
            PendingNoColorClouds.Add(MoveTemp(Pending));
            UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: delaying PC '%s' for gradient LUT"), *ElementNameForLog);
            return;
        }
    }

    if (!BeginTask())
    {
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: queued PC '%s' for async conversion (%d points)"),
           *PCData.ElementName, PCData.PointCount);

    // Spawn async task — doesn't block the game thread. The rvalue overload
    // moves the arrays into the task instead of copying the whole cloud.
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
        [this, PointCount = PCData.PointCount, Positions = MoveTemp(PCData.Positions),
          Colors = MoveTemp(PCData.Colors), bHasColors = bLegacyHasColors,
          Widths = MoveTemp(PCData.Widths), ElementName = MoveTemp(PCData.ElementName),
          InRank]()
    {
#ifdef WITH_ANARI_USD_MIDDLEWARE
        // Capture gradient LUT lazily at conversion time (after possible SetGradientLUT)
        TArray<FColor> LocalLUT;
        {
            FScopeLock Lock(&this->GradientMutex);
            LocalLUT = this->GradientLUT;
        }
        bool bUseGradient = LocalLUT.Num() > 0;

        // Build LiDAR points on background thread (parallel: every index
        // reads its own input slots and writes its own output slot).
        TArray64<FLidarPointCloudPoint> Points;
        Points.SetNum(PointCount);
        ParallelFor(PointCount, [&](int32 i)
        {
            FVector3f Pos(Positions[i].X, Positions[i].Y, Positions[i].Z);
            FColor Col;
            if (bHasColors)
            {
                Col = Colors[i];
            }
            else if (bUseGradient && Widths.Num() > i)
            {
                // attribute0 scalar value (0-1) maps to gradient LUT index
                float Attr0 = Widths[i];
                int32 LUTIdx = FMath::Clamp(FMath::RoundToInt(Attr0 * (LocalLUT.Num() - 1)), 0, LocalLUT.Num() - 1);
                Col = LocalLUT[LUTIdx];
            }
            else
            {
                Col = FColor::White;
            }
            Points[i] = FLidarPointCloudPoint(Pos.X, Pos.Y, Pos.Z, Col.R / 255.f, Col.G / 255.f, Col.B / 255.f, Col.A / 255.f);
        });

        // Marshal to game thread
        FPointCloudReadyEntry Entry;
        Entry.Points = MoveTemp(Points);
        Entry.ElementName = ElementName;
        Entry.Key = FString::Printf(TEXT("%s_r%d"), *ElementName, InRank);
        Entry.Rank = InRank;
        Entry.PointSize = PointSize;
        Entry.bSpawned = false;
        Entry.bNeedsRecolor = !bUseGradient && !bHasColors;
        if (Entry.bNeedsRecolor)
        {
            Entry.Positions = Positions;
            Entry.Widths = Widths;
        }

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [this, Entry = MoveTemp(Entry)]() mutable
            {
                {
                    FScopeLock Lock(&QueueMutex);
                    ReadyQueue.Add(MoveTemp(Entry));

                    // Threshold flush: if queue exceeds 8, drain immediately.
                    // QueueMutex is already held by the enclosing game-thread task.
                    if (ReadyQueue.Num() >= 8)
                    {
                        DrainReadyQueueLocked();
                    }
                }
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
#else
        (void)PointCount;
        (void)Positions;
        (void)Colors;
        (void)bHasColors;
        (void)Widths;
        (void)ElementName;
        (void)InRank;
        (void)LocalLUT;
        (void)bUseGradient;
#endif
        EndTask();
    });
}

#ifdef WITH_ANARI_USD_MIDDLEWARE
ULidarPointCloud* FJUSYNCPointCloudSpawner::GetOrCreateCloudForComponent(ULidarPointCloudComponent* Comp)
{
    if (!Comp)
    {
        return nullptr;
    }

    ULidarPointCloud* Cloud = Comp->GetPointCloud();
    if (!Cloud || Cloud->HasAnyFlags(RF_BeginDestroyed))
    {
        Cloud = NewObject<ULidarPointCloud>(Comp);
        Comp->SetPointCloud(Cloud);
    }

    // Static (non-dynamic) optimization builds the octree LOD hierarchy, which gives distance-
    // based decimation: far nodes render a coarse LOD, close nodes the full-resolution LOD.
    // Dynamic optimization skips the LOD pipeline entirely (no granular density control).
    Cloud->SetOptimizedForDynamicData(false);
    return Cloud;
}

// ---------------------------------------------------------------------------
// Async streaming rebuild: Epic's sanctioned async path (CreateFromData +
// FLidarPointCloudAsyncParameters builds the octree on a worker thread; see
// LidarPointCloud.cpp CreateFromData_Internal). Exactly ONE build in flight
// per actor key; later updates for the same key are stashed latest-wins and
// start when the current build finishes. The finished asset is swapped in on
// the game thread; the old asset becomes unreferenced for GC.
// ---------------------------------------------------------------------------
void FJUSYNCPointCloudSpawner::StartAsyncCloudBuild(const FString& Key, FPointCloudReadyEntry&& Entry, AActor* Actor)
{
    if (Entry.Points.Num() == 0 || bShuttingDown.load())
    {
        return;
    }

    TSharedRef<FInFlightCloudBuild>* Existing = InFlightBuilds.Find(Key);
    TSharedRef<FInFlightCloudBuild> F = Existing ? *Existing : MakeShared<FInFlightCloudBuild>();
    if (!Existing)
    {
        InFlightBuilds.Add(Key, F);
    }

    if (F->bBuilding)
    {
        // Latest-wins: replace the stash outright (old stashed points freed here).
        F->PendingReplacement = MoveTemp(Entry);
        F->bHasPending = true;
        UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: build in flight for '%s' - stashed newer update (latest-wins)"), *Key);
        return;
    }

    F->Actor = Actor;
    F->Points = new TArray64<FLidarPointCloudPoint>(MoveTemp(Entry.Points));
    F->Generation = ++NextCloudBuildGeneration;
    F->bBuilding = true;
    F->BuildStarted = FPlatformTime::Seconds();

    const uint64 Generation = F->Generation;
    const int32 NumPts = static_cast<int32>(F->Points->Num());

    // Completion fires on Epic's build worker thread; swap work hops to the GT.
    // The worker may outlive shutdown waits, so it must not dereference the
    // spawner: it decrements the shared outstanding count directly, and the
    // GT hop is dropped via the alive flag if the spawner died meanwhile.
    FLidarPointCloudAsyncParameters Params(
        /*bUseAsync=*/true,
        /*ProgressCallback=*/nullptr,
        [this, Key, Generation,
         Ctr = AsyncBuildsOutstanding, Alive = AsyncBuildsAlive](bool bSuccess)
        {
            Ctr->fetch_sub(1);
            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [this, Key, Generation, bSuccess, Alive]()
                {
                    if (Alive->load())
                    {
                        OnAsyncCloudBuildFinished(Key, Generation, bSuccess);
                    }
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });

    AsyncBuildsOutstanding->fetch_add(1);
    JUSYNCPadDegenerateBounds(*F->Points);
    ULidarPointCloud* NewCloud = ULidarPointCloud::CreateFromData(*F->Points, Params);
    // GT-only strong ref: keeps the asset GC-safe while the worker fills it.
    F->CloudRef = MakeUnique<TStrongObjectPtr<UObject>>(NewCloud);

    UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: started async build '%s' (%d points)"), *Key, NumPts);
}

void FJUSYNCPointCloudSpawner::OnAsyncCloudBuildFinished(const FString& Key, uint64 Generation, bool bSuccess)
{
    TSharedRef<FInFlightCloudBuild>* Found = InFlightBuilds.Find(Key);
    if (Found && (*Found)->Generation == Generation && (*Found)->bBuilding)
    {
        FInFlightCloudBuild& F = **Found;
        const double BuildMs = (FPlatformTime::Seconds() - F.BuildStarted) * 1000.0;

        delete F.Points;
        F.Points = nullptr;

        ULidarPointCloud* Cloud = F.CloudRef.IsValid() ? Cast<ULidarPointCloud>(F.CloudRef->Get()) : nullptr;
        F.CloudRef.Reset(); // ownership moves to the component (swap) or GC (drop)
        F.bBuilding = false;

        AActor* Actor = F.Actor.Get();
        bool bSwapped = false;
        ULidarPointCloud* ReplacedCloud = nullptr;

        if (bSuccess && IsValid(Cloud) && IsValid(Actor) && !bShuttingDown.load()
            && FindActorByElementKey(Key) == Actor) // actor may have been released to the pool meanwhile
        {
            if (ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor))
            {
                if (ULidarPointCloudComponent* Comp = LidarActor->GetPointCloudComponent())
                {
                    const double SwapStart = FPlatformTime::Seconds();
                    ReplacedCloud = Comp->GetPointCloud();
                    Comp->SetPointCloud(Cloud);
                    ApplyVisualSettings(Comp);
                    Actor->SetActorHiddenInGame(false);
                    F.bEverSwapped = true;
                    bSwapped = true;

                    if (bPerfLogging)
                    {
                        int32 LiveClouds = 0;
                        for (TObjectIterator<ULidarPointCloud> It; It; ++It)
                        {
                            if (IsValid(*It) && !It->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
                            {
                                ++LiveClouds;
                            }
                        }
                        const FPlatformMemoryStats Mem = FPlatformMemory::GetStats();
                        UE_LOG(LogTemp, Display,
                               TEXT("JUSYNC PERF PC async-build '%s' build %.1f ms  swap %.3f ms  liveClouds %d  usedPhys %.0f MB"),
                               *Key, BuildMs, (FPlatformTime::Seconds() - SwapStart) * 1000.0,
                               LiveClouds, (double)Mem.UsedPhysical / (1024.0 * 1024.0));
                    }
                    OnPointCloudSpawned.Broadcast(Key, Actor);
                }
            }
        }

        if (!bSwapped && IsValid(Cloud))
        {
            Cloud->MarkAsGarbage();
        }
        else if (bSwapped && IsValid(ReplacedCloud) && ReplacedCloud != Cloud)
        {
            // Detached last-frame asset: nothing renders it anymore, so flag
            // it for GC immediately instead of waiting for the default purge
            // interval (streamed clouds would otherwise pile up between sweeps).
            ReplacedCloud->MarkAsGarbage();
            ++ReplacementsSinceGC;
            if (ReplacementsSinceGC >= GCSwapInterval)
            {
                const double GCStart = FPlatformTime::Seconds();
                CollectGarbage(RF_NoFlags, true);
                ReplacementsSinceGC = 0;
                UE_LOG(LogTemp, Display, TEXT("JUSYNC PERF GC sweep %0.1f ms"),
                       (FPlatformTime::Seconds() - GCStart) * 1000.0);
            }
        }

        if (F.bHasPending)
        {
            FPointCloudReadyEntry Next = MoveTemp(F.PendingReplacement);
            F.bHasPending = false;
            if (!bSwapped && !IsValid(Actor))
            {
                Actor = AllocateActor();
            }
            if (Actor)
            {
                ApplyTransformToActor(Actor);
                StartAsyncCloudBuild(Key, MoveTemp(Next), Actor);
            }
        }
            else if (!bSwapped)
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: async build for '%s' %s (%.1f ms)"),
                   *Key, bSuccess ? TEXT("discarded") : TEXT("FAILED"), BuildMs);
            if (IsValid(Actor) && !F.bEverSwapped)
            {
                // First frame never landed: give the actor back and forget the key.
                ReleaseActor(Actor);
                InFlightBuilds.Remove(Key);
            }
        }
    }
}
#endif

#ifdef WITH_ANARI_USD_MIDDLEWARE
void FJUSYNCPointCloudSpawner::ApplyVisualSettings(ULidarPointCloudComponent* Comp)
{
    if (!Comp)
    {
        return;
    }

    Comp->PointSize = FMath::Max(0.0f, PointSize);
    Comp->PointSizeBias = FMath::Clamp(PointSizeBias, 0.0f, 0.15f);
    Comp->GapFillingStrength = FMath::Max(0.0f, GapFillingStrength);
    Comp->PointOrientation = PointOrientation;
    Comp->ScalingMethod = PointScaling;

    if (Comp->GetPointShape() != PointShape)
    {
        Comp->SetPointShape(PointShape);
    }
}

void FJUSYNCPointCloudSpawner::ApplyPointShapeToActiveActors(ELidarPointCloudSpriteShape InShape)
{
    PointShape = InShape;
    for (AActor* Actor : ActiveActors)
    {
        if (ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor))
        {
            if (ULidarPointCloudComponent* Comp = LidarActor->GetPointCloudComponent())
            {
                // SetPointShape re-derives the material but does not dirty the render state.
                Comp->SetPointShape(InShape);
                Comp->MarkRenderStateDirty();
            }
        }
    }
}

void FJUSYNCPointCloudSpawner::ApplyVisualSettingsToActor(AActor* Actor)
{
    if (!Actor || !Actor->IsValidLowLevel())
    {
        return;
    }

    if (ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor))
    {
        ApplyVisualSettings(LidarActor->GetPointCloudComponent());
    }
}

void FJUSYNCPointCloudSpawner::MaybeCalculateNormals(ULidarPointCloud* Cloud, AActor* Actor, int32 PointCount)
{
    if (!Cloud || !bCalculateNormals || PointCount <= 0 || PointCount > MaxPointsForNormals)
    {
        return;
    }

    const double Now = FPlatformTime::Seconds();
    if (Actor)
    {
        if (const double* LastTime = LastNormalCalcTime.Find(Actor))
        {
            if (Now - *LastTime < NormalsCooldownSeconds)
            {
                return;
            }
        }
        LastNormalCalcTime.Add(Actor, Now);
    }
    else
    {
        static double LastGlobalNormalCalcTime = 0.0;
        if (Now - LastGlobalNormalCalcTime < NormalsCooldownSeconds)
        {
            return;
        }
        LastGlobalNormalCalcTime = Now;
    }

    Cloud->NormalsQuality = FMath::Clamp(NormalsQuality, 1, 100);
    Cloud->NormalsNoiseTolerance = FMath::Max(0.0f, NormalsNoiseTolerance);
    if (bPerfLogging)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC PERF PC normals requested for %d points (quality %d)"), PointCount, Cloud->NormalsQuality);
    }
    Cloud->CalculateNormals(nullptr, TFunction<void(void)>());
}
#endif

AActor* FJUSYNCPointCloudSpawner::AllocateActor()
{
    if (AvailablePool.Num() > 0)
    {
        AActor* Actor = AvailablePool.Pop();
        if (Actor && Actor->IsValidLowLevel())
        {
            UntrackActorElement(Actor);
            Actor->SetActorEnableCollision(false);
#ifdef WITH_ANARI_USD_MIDDLEWARE
            ApplyVisualSettingsToActor(Actor);
#endif
            ApplyTransformToActor(Actor);
            ActiveActors.Add(Actor);
            return Actor;
        }
    }

    if (ActiveActors.Num() >= MaxPoolSize)
    {
        if (AActor* Oldest = ActiveActors.Array().Num() > 0 ? ActiveActors.Array()[0] : nullptr)
        {
            if (Oldest && Oldest->IsValidLowLevel())
            {
                ActiveActors.Remove(Oldest);
                UntrackActorElement(Oldest);
                Oldest->SetActorHiddenInGame(false);
#ifdef WITH_ANARI_USD_MIDDLEWARE
                ApplyVisualSettingsToActor(Oldest);
#endif
                ApplyTransformToActor(Oldest);
                ActiveActors.Add(Oldest);
                return Oldest;
            }
        }
        return nullptr;
    }

    if (!Owner.IsValid()) return nullptr;

    UWorld* World = GEngine->GetWorldFromContextObject(Owner.Get(), EGetWorldErrorMode::ReturnNull);
    if (!World) return nullptr;

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

#ifdef WITH_ANARI_USD_MIDDLEWARE
    ALidarPointCloudActor* SpawnedActor = World->SpawnActor<ALidarPointCloudActor>(SpawnLocation, FRotator::ZeroRotator, SpawnParams);
#else
    return nullptr;
#endif

    if (SpawnedActor)
    {
        SpawnedActor->SetActorEnableCollision(false);
        ULidarPointCloudComponent* Comp = SpawnedActor->GetPointCloudComponent();
        if (Comp)
        {
            Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Comp->ColorSource = ELidarPointCloudColorationMode::Data;
            ApplyVisualSettings(Comp);
            // LOD streaming: full depth range, but render only nodes within the view frustum.
            // r.LidarPointBudget (adaptive) then decimates far nodes to hold the target FPS.
            Comp->MinDepth = 0;
            Comp->MaxDepth = -1;
            Comp->bUseFrustumCulling = true;
        }

        SpawnedActor->SetActorHiddenInGame(true);
        SpawnedActor->SetActorTickEnabled(false);
        ApplyTransformToActor(SpawnedActor);
    }

    ActiveActors.Add(SpawnedActor);
    return SpawnedActor;
}

void FJUSYNCPointCloudSpawner::UntrackActorElement(AActor* Actor)
{
    if (!Actor)
    {
        return;
    }

    if (const FString* Key = ActorToElement.Find(Actor))
    {
        ElementToActor.Remove(*Key);
    }
    ActorToElement.Remove(Actor);
}

AActor* FJUSYNCPointCloudSpawner::FindActorByElementKey(const FString& ElementKey) const
{
    const AActor* Found = ElementToActor.FindRef(ElementKey);
    return (Found && Found->IsValidLowLevel()) ? const_cast<AActor*>(Found) : nullptr;
}

FString FJUSYNCPointCloudSpawner::GetElementKeyForActor(AActor* Actor) const
{
    return ActorToElement.FindRef(Actor);
}

void FJUSYNCPointCloudSpawner::DestroyTrackedActor(AActor* Actor)
{
    if (!Actor) return;

    ActiveActors.Remove(Actor);
    AvailablePool.Remove(Actor);
    GradientPendingActors.Remove(Actor);
    GradientPendingData.Remove(Actor);
    LastNormalCalcTime.Remove(Actor);
    UntrackActorElement(Actor);

    if (Actor->IsValidLowLevel())
    {
        Actor->Destroy();
    }
}

void FJUSYNCPointCloudSpawner::ReleaseActor(AActor* Actor)
{
    if (!Actor) return;

    ActiveActors.Remove(Actor);
    UntrackActorElement(Actor);
    Actor->SetActorHiddenInGame(true);
    Actor->SetActorEnableCollision(false);
    AvailablePool.Add(Actor);
}

void FJUSYNCPointCloudSpawner::SetSpawnLocation(const FVector& In)
{
    SpawnLocation = In;
}

void FJUSYNCPointCloudSpawner::ApplyTransformToActor(AActor* Actor)
{
    if (!Actor || !Actor->IsValidLowLevel())
    {
        return;
    }

    Actor->SetActorLocation(SpawnLocation);
    Actor->SetActorScale3D(FVector(SpawnScale));
}

void FJUSYNCPointCloudSpawner::Tick(float DeltaTime)
{
    if (!Owner.IsValid()) return;
    FlushExpiredNoColorClouds();
    DrainReadyQueue();
}

void FJUSYNCPointCloudSpawner::DrainReadyQueue()
{
    FScopeLock Lock(&QueueMutex);
    DrainReadyQueueLocked();
}

void FJUSYNCPointCloudSpawner::DrainReadyQueueLocked()
{
    if (ReadyQueue.Num() == 0) return;

    double StartTime = FPlatformTime::Seconds();
    int32 ItemsProcessed = 0;
    const int32 MaxItemsPerTick = 1;
    double RemainingBudget = (BudgetMs / 1000.0);

    while (ReadyQueue.Num() > 0)
    {
        double Elapsed = FPlatformTime::Seconds() - StartTime;
        if (ItemsProcessed >= MaxItemsPerTick || (Elapsed > RemainingBudget && ItemsProcessed > 0))
        {
            break;
        }

        FPointCloudReadyEntry Entry = ReadyQueue.Pop();

        if (Entry.bSpawned)
        {
            continue;
        }

        if (Entry.Key.IsEmpty())
        {
            Entry.Key = Entry.ElementName.IsEmpty()
                ? FString::Printf(TEXT("jusync_pc_r%d"), Entry.Rank)
                : FString::Printf(TEXT("%s_r%d"), *Entry.ElementName, Entry.Rank);
        }

        if (Entry.Points.Num() == 0)
        {
            if (AActor* ExistingActor = FindActorByElementKey(Entry.Key))
            {
                ReleaseActor(ExistingActor);
            }
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: skipped empty PC entry '%s'"), *Entry.Key);
            continue;
        }

        AActor* Actor = FindActorByElementKey(Entry.Key);
        const bool bInPlaceUpdate = (Actor != nullptr);
        if (!Actor)
        {
            Actor = AllocateActor();
            if (!Actor)
            {
                ReadyQueue.Add(MoveTemp(Entry));
                break;
            }
        }

        // Pooled/in-place actors keep whatever transform they were created with, so re-apply the
        // spawner's current location/scale here; otherwise extent changes never resize them.
        ApplyTransformToActor(Actor);

#ifdef WITH_ANARI_USD_MIDDLEWARE
        // Async path (default): off-thread octree build + latest-wins swap
        // (see StartAsyncCloudBuild).
        if (CVarJusyncAsyncCloudBuild.GetValueOnGameThread() > 0
            && Entry.Points.Num() > 0 && !Entry.bNeedsRecolor)
        {
            if (!bInPlaceUpdate)
            {
                ElementToActor.Add(Entry.Key, Actor);
                ActorToElement.Add(Actor, Entry.Key);
            }
            Actor->SetActorEnableCollision(false);
            StartAsyncCloudBuild(Entry.Key, MoveTemp(Entry), Actor);
            ItemsProcessed++;
            continue;
        }

        bool bPointCloudUpdateFailed = false;
        if (ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor))
        {
            if (ULidarPointCloudComponent* Comp = LidarActor->GetPointCloudComponent())
            {
                ULidarPointCloud* LidarCloud = GetOrCreateCloudForComponent(Comp);
                const double SetDataStart = FPlatformTime::Seconds();
                JUSYNCPadDegenerateBounds(Entry.Points);
                const bool bSetDataAccepted = LidarCloud && LidarCloud->SetData(Entry.Points);
                if (bPerfLogging)
                {
                    UE_LOG(LogTemp, Display, TEXT("JUSYNC PERF PC SetData '%s' %d points %.3f ms"),
                           *Entry.Key, Entry.Points.Num(), (FPlatformTime::Seconds() - SetDataStart) * 1000.0);
                }
                if (!bSetDataAccepted)
                {
                    UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: failed to update LiDAR cloud for '%s'"),
                           *Entry.Key);
                    bPointCloudUpdateFailed = true;
                }
                else
                {
                    MaybeCalculateNormals(LidarCloud, Actor, static_cast<int32>(Entry.Points.Num()));
                }
            }
            else
            {
                bPointCloudUpdateFailed = true;
            }
        }
        else
        {
            bPointCloudUpdateFailed = true;
        }

        if (bPointCloudUpdateFailed)
        {
            if (!bInPlaceUpdate)
            {
                ReleaseActor(Actor);
            }
            continue;
        }

        Actor->SetActorHiddenInGame(false);
        Actor->SetActorEnableCollision(false);

        if (!bInPlaceUpdate)
        {
            ElementToActor.Add(Entry.Key, Actor);
            ActorToElement.Add(Actor, Entry.Key);
        }

        if (Entry.bNeedsRecolor)
        {
            GradientPendingActors.Add(Actor);
            GradientPendingData.Add(Actor, FRecolorData{ MoveTemp(Entry.Positions), MoveTemp(Entry.Widths) });
        }

        const FString EntryName = Entry.Key;
        const int32 EntryPoints = Entry.Points.Num();
        ItemsProcessed++;

        OnPointCloudSpawned.Broadcast(EntryName, Actor);

        {
            UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: %s PC actor '%s' (%d points, budget: %.1fms remaining)"),
                   bInPlaceUpdate ? TEXT("updated") : TEXT("loaded"),
                   *EntryName, EntryPoints, (RemainingBudget - Elapsed) * 1000.0f);
        }
#endif
    }

    // Recolor any actors that spawned white but LUT is now available
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (GradientPendingActors.Num() > 0)
    {
        FScopeLock Lock(&GradientMutex);
        if (GradientLUT.Num() > 0)
            RecolorGradientPendingActors();
    }
#endif
}

void FJUSYNCPointCloudSpawner::ClearAllActors()
{
    for (AActor* Actor : ActiveActors)
    {
        ReleaseActor(Actor);
    }
    ActiveActors.Empty();
}

void FJUSYNCPointCloudSpawner::DestroyAllActors()
{
    {
        FScopeLock Lock(&QueueMutex);
        ReadyQueue.Empty();
        PendingNoColorClouds.Empty();
    }

    for (AActor* Actor : ActiveActors)
    {
        if (Actor && Actor->IsValidLowLevel()) Actor->Destroy();
    }
    ActiveActors.Empty();

    for (AActor* Actor : AvailablePool)
    {
        if (Actor && Actor->IsValidLowLevel()) Actor->Destroy();
    }
    AvailablePool.Empty();

    GradientPendingActors.Empty();
    GradientPendingData.Empty();
    ElementToActor.Empty();
    ActorToElement.Empty();
    LastNormalCalcTime.Empty();

#ifdef WITH_ANARI_USD_MIDDLEWARE
    // Drop stashed replacements (points owned by the entry dtor). In-flight builds
    // self-discard on completion: the actor is untracked, so the key-ownership
    // check fails and the fresh asset is marked garbage.
    for (auto& Pair : InFlightBuilds)
    {
        Pair.Value->PendingReplacement = FPointCloudReadyEntry();
        Pair.Value->bHasPending = false;
    }
#endif
}

void FJUSYNCPointCloudSpawner::RecolorGradientPendingActors()
{
    if (GradientPendingActors.Num() == 0) return;

    TArray<FColor> LocalLUT;
    {
        FScopeLock Lock(&GradientMutex);
        LocalLUT = GradientLUT;
    }
    if (LocalLUT.Num() == 0) return;

    int32 Recolored = 0;
    for (AActor* Actor : GradientPendingActors.Array())
    {
        if (!Actor || !Actor->IsValidLowLevel()) continue;

        ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor);
        if (!LidarActor) continue;

        ULidarPointCloudComponent* Comp = LidarActor->GetPointCloudComponent();
        if (!Comp) continue;

        FRecolorData* RD = GradientPendingData.Find(Actor);
        if (!RD || RD->Positions.Num() == 0) continue;

        int32 N = FMath::Min(RD->Positions.Num(), RD->Widths.Num());
        TArray64<FLidarPointCloudPoint> NewPoints;
        NewPoints.SetNum(N);
        for (int32 i = 0; i < N; ++i)
        {
            FVector3f Pos(RD->Positions[i].X, RD->Positions[i].Y, RD->Positions[i].Z);
            float Attr0 = RD->Widths[i];
            int32 LUTIdx = FMath::Clamp(FMath::RoundToInt(Attr0 * (LocalLUT.Num() - 1)), 0, LocalLUT.Num() - 1);
            FColor Col = LocalLUT[LUTIdx];
            NewPoints[i] = FLidarPointCloudPoint(Pos.X, Pos.Y, Pos.Z, Col.R / 255.f, Col.G / 255.f, Col.B / 255.f, Col.A / 255.f);
        }

        ULidarPointCloud* Cloud = GetOrCreateCloudForComponent(Comp);
        JUSYNCPadDegenerateBounds(NewPoints);
        if (Cloud && Cloud->SetData(NewPoints))
        {
            MaybeCalculateNormals(Cloud, Actor, static_cast<int32>(NewPoints.Num()));
            Recolored++;
        }
    }

    GradientPendingActors.Empty();
    GradientPendingData.Empty();

    if (Recolored > 0)
    {
        UE_LOG(LogTemp, Display, TEXT("[Spawner] Recolored %d point cloud actors with gradient"), Recolored);
    }
}
