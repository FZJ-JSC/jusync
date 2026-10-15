#include "JUSYNCPointCloudSpawner.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/Engine.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformTime.h"
#include "JUSYNCElementNames.h"

#ifdef WITH_ANARI_USD_MIDDLEWARE
#include "LidarPointCloud.h"
#include "LidarPointCloudComponent.h"
#include "LidarPointCloudActor.h"
#include "LidarPointCloudSettings.h"

// SetData. 0: legacy in-place SetData on the game thread.

static TAutoConsoleVariable<int32> CVarJusyncCloudMorphSteps(
    TEXT("jusync.Morph.CloudSteps"), 4,
    TEXT("Number of pre-interpolated cloud builds streamed between successive .geo time samples (1..6)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncCloudBlendMs(
    TEXT("jusync.Morph.CloudBlendMs"), 0,
    TEXT("Fixed cloud blend window in ms; 0 = auto (measured sample gap clamped by CloudMaxBlendMs)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncCloudMaxBlendMs(
    TEXT("jusync.Morph.CloudMaxBlendMs"), 2000,
    TEXT("Cap in ms for the auto cloud blend window."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncCloudSmooth(
    TEXT("jusync.Morph.CloudSmooth"), 1,
    TEXT("Per-frame smooth morphing of streamed clouds via in-place point updates (1) vs stepped ramp (0)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncCloudRenderSmoothing(
    TEXT("jusync.Morph.CloudRenderSmoothing"), 0,
    TEXT("1 = keep LidarPointCloud render-data smoothing during morphs (default 0 = disable for upload consistency)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncCloudFixedSize(
    TEXT("jusync.Morph.CloudFixedSize"), 1,
    TEXT("1 (default) = render streamed clouds with FixedScreenSize scaling (octree-independent point size)."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarJusyncCloudScreenSize(
    TEXT("jusync.Morph.CloudScreenSize"), 0.25f,
    TEXT("Point size (screen-fraction units) used while jusync.Morph.CloudFixedSize is on."),
    ECVF_Default);

static TAutoConsoleVariable<FString> CVarJusyncIgnoreElements(
    TEXT("jusync.Spawner.IgnoreElements"), TEXT(""),
    TEXT("Comma-separated substrings (or * patterns); elements with matching names are skipped, e.g. \"_u1\"."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncAsyncCloudBuild(
    TEXT("jusync.AsyncCloudBuild"), 1,
    TEXT("Build streamed point clouds off the game thread (Epic async CreateFromData) and swap on completion."),
    ECVF_Default);
#endif

namespace
{
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
    EnqueuePointCloud(FJUSYNCPointCloudData(PCData), InRank);
}

bool FJUSYNCPointCloudSpawner::IsElementFiltered(const FString& ElementName) const
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    const FString Spec = CVarJusyncIgnoreElements.GetValueOnGameThread();
#else
    const FString Spec;
#endif
    if (Spec.IsEmpty() || ElementName.IsEmpty())
    {
        return false;
    }

    TArray<FString> Parts;
    Spec.ParseIntoArray(Parts, TEXT(","));
    for (FString& Part : Parts)
    {
        Part.TrimStartAndEndInline();
        if (Part.IsEmpty())
        {
            continue;
        }
        const bool bMatched = (Part.Contains(TEXT("*")) || Part.Contains(TEXT("?")))
            ? ElementName.MatchesWildcard(Part, ESearchCase::CaseSensitive)
            : ElementName.Contains(Part);
        if (bMatched)
        {
            return true;
        }
    }
    return false;
}

void FJUSYNCPointCloudSpawner::EnqueuePointCloud(FJUSYNCPointCloudData&& PCData, int32 InRank, bool bAllowNoGradient)
{
    if (!PCData.IsValid() || !Owner.IsValid() || bShuttingDown.load()) return;

    // Rankless data would key its morph chain like rank 0 and blend rank 0's
    // previous positions into this sample, so say so instead of only showing it
    // as a warp.
    if (InRank < 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: cloud '%s' arrived with no owner rank; morph state keyed as rank 0"), *PCData.ElementName);
    }

    if (IsElementFiltered(PCData.ElementName))
    {
        UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: SKIPPED filtered PC '%s' (%d points)"),
               *PCData.ElementName, PCData.PointCount);
        return;
    }

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
            Entry.Key = JUSYNCElementNames::MakeRankedElementKey(ElementName, InRank);
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

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
        [this, PointCount = PCData.PointCount, Positions = MoveTemp(PCData.Positions),
          Colors = MoveTemp(PCData.Colors), bHasColors = bLegacyHasColors,
          Widths = MoveTemp(PCData.Widths), ElementName = MoveTemp(PCData.ElementName),
          InRank]()
    {
#ifdef WITH_ANARI_USD_MIDDLEWARE
        TArray<FColor> LocalLUT;
        {
            FScopeLock Lock(&this->GradientMutex);
            LocalLUT = this->GradientLUT;
        }
        bool bUseGradient = LocalLUT.Num() > 0;

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
                float Attr0 = Widths[i];
                int32 LUTIdx = FMath::Clamp(FMath::RoundToInt(Attr0 * (LocalLUT.Num() - 1)), 0, LocalLUT.Num() - 1);
                Col = LocalLUT[LUTIdx];
            }
            else
            {
                // Neutral gray instead of white: uncolored / recolor-pending clouds used to
                // flash pure-white (a glare "glow") until the gradient LUT recolored them.
                Col = FColor(128, 128, 128);
            }
            Points[i] = FLidarPointCloudPoint(Pos.X, Pos.Y, Pos.Z, Col.R / 255.f, Col.G / 255.f, Col.B / 255.f, Col.A / 255.f);
        });

        FPointCloudReadyEntry Entry;
        Entry.Points = MoveTemp(Points);
        Entry.ElementName = ElementName;
        Entry.Key = JUSYNCElementNames::MakeRankedElementKey(ElementName, InRank);
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

    Cloud->SetOptimizedForDynamicData(false);
    return Cloud;
}

void FJUSYNCPointCloudSpawner::StartAsyncCloudBuild(const FString& Key, FPointCloudReadyEntry&& Entry, AActor* Actor,
                                                    double EarliestApply, bool bChainStep, bool bChainFinal)
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

    F->EarliestApply = EarliestApply;
    F->bDeferred = false;
    F->bChainStep = bChainStep;
    F->bChainFinal = bChainFinal;
    F->bChainAborted = false;

    F->RampPos.SetNum(F->Points->Num());
    for (int64 i = 0; i < F->Points->Num(); ++i)
    {
        F->RampPos[i] = (*F->Points)[i].Location;
    }

    const uint64 Generation = F->Generation;
    const int32 NumPts = static_cast<int32>(F->Points->Num());

    // Completion fires on Epic's build worker thread; swap work hops to the GT.

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

    UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: started async build '%s' (%d points)%s"), *Key, NumPts,
           bChainStep ? (bChainFinal ? TEXT(" [ramp final]") : TEXT(" [ramp step]")) : TEXT(""));
}

void FJUSYNCPointCloudSpawner::OnAsyncCloudBuildFinished(const FString& Key, uint64 Generation, bool bSuccess)
{
    TSharedRef<FInFlightCloudBuild>* Found = InFlightBuilds.Find(Key);
    if (Found && (*Found)->Generation == Generation && ((*Found)->bBuilding || (*Found)->bDeferred))
    {
        FInFlightCloudBuild& F = **Found;
        const double BuildMs = (FPlatformTime::Seconds() - F.BuildStarted) * 1000.0;
        const double NowSeconds = FPlatformTime::Seconds();

        delete F.Points;
        F.Points = nullptr;

        F.bDeferred = false;

        if (F.bChainAborted)
        {
            bSuccess = false;
            F.bChainAborted = false;
            F.bChainStep = false;
            F.bChainFinal = false;
        }

        if (bSuccess && F.bHasPending)
        {
            if (ULidarPointCloud* Stale = F.CloudRef.IsValid()
                ? Cast<ULidarPointCloud>(F.CloudRef->Get()) : nullptr)
            {
                Stale->MarkAsGarbage();
            }
            F.CloudRef.Reset();
            F.bBuilding = false;

            FPointCloudReadyEntry Next = MoveTemp(F.PendingReplacement);
            F.bHasPending = false;

            AActor* NextActor = F.Actor.Get();
            if (!IsValid(NextActor))
            {
                NextActor = AllocateActor();
            }
            if (NextActor)
            {
                ApplyTransformToActor(NextActor);
                double NextApply = 0.0;
                if (const FCloudSmoothMorph* M = SmoothMorphs.Find(Key))
                {
                    NextApply = M->T0 + M->Duration; // land when the glide ends
                }
                StartAsyncCloudBuild(Key, MoveTemp(Next), NextActor, NextApply);
            }
            return;
        }

        if (bSuccess && (NowSeconds < F.EarliestApply || SmoothMorphs.Contains(Key)))
        {
            F.bBuilding = false; // worker is done; only the SWAP is pending now
            F.bDeferred = true;
            return;
        }

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

                    const TArray<int32>* Map = CloudWalkInputIdx.Find(Key);
                    if (Map && IsValid(ReplacedCloud) && F.RampPos.Num() > 0)
                    {
                        const FVector3f NewNegOffset = (FVector3f)(-Cloud->LocationOffset);
                        float ResidMax = 0.f;
                        int32 Slot = 0, Checked = 0, Unmatched = 0;
                        const auto Probe = [&](FLidarPointCloudPoint* P)
                        {
                            const int32 Src = (Slot < Map->Num()) ? (*Map)[Slot] : INDEX_NONE;
                            ++Slot;
                            if (Src == INDEX_NONE || Src >= F.RampPos.Num())
                            {
                                ++Unmatched;
                                return;
                            }
                            ++Checked;
                            const FVector3f NewWalk = F.RampPos[Src] + NewNegOffset;
                            ResidMax = FMath::Max(ResidMax, (P->Location - NewWalk).Length());
                        };
                        ReplacedCloud->ExecuteActionOnAllPoints(Probe, /*bVisibleOnly*/false);
                        UE_LOG(LogTemp, Display,
                               TEXT("JUSYNC CloudMorph: SMOOTH RESID '%s' checked=%d unmatched=%d residMax=%.4fcm oldOff=%.2f newOff=%.2f map=%d ramp=%d walk=%d"),
                               *Key, Checked, Unmatched, ResidMax,
                               ReplacedCloud->LocationOffset.Size(), Cloud->LocationOffset.Size(),
                               Map->Num(), F.RampPos.Num(), (int32)ReplacedCloud->GetNumPoints());
                    }

                    Cloud->SetOptimizedForDynamicData(true);

                    Comp->SetPointCloud(Cloud);
                    ApplyVisualSettings(Comp);
                    Actor->SetActorHiddenInGame(false);
                    F.bEverSwapped = true;
                    bSwapped = true;

                    if (F.RampPos.Num() > 0 && F.bChainStep)
                    {
                        UE_LOG(LogTemp, Log, TEXT("JUSYNC CloudRamp: %s swapped in for '%s' (%d pts displayed)"),
                               F.bChainFinal ? TEXT("final") : TEXT("step"), *Key, F.RampPos.Num());
                    }
                    if (F.RampPos.Num() > 0)
                    {
                        CloudDisplayPos.Add(Key, MoveTemp(F.RampPos));
                    }

                    SmoothMorphs.Remove(Key);
                    CloudWalkInputIdx.Remove(Key);

                    const bool bFinishedChain = F.bChainFinal;
                    F.bChainStep = false;
                    F.bChainFinal = false;
                    if (bFinishedChain)
                    {
                        RampChains.Remove(Key);
                    }

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
            ReplacedCloud->MarkAsGarbage();
            // (gc.TimeBetweenPurgingPendingKillObjects) without user-visible hitches.

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
                double NextApply = 0.0;
                if (const FCloudSmoothMorph* M = SmoothMorphs.Find(Key))
                {
                    NextApply = M->T0 + M->Duration;
                }
                StartAsyncCloudBuild(Key, MoveTemp(Next), Actor, NextApply);
            }
        }
            else if (!bSwapped)
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: async build for '%s' %s (%.1f ms)"),
                   *Key, bSuccess ? TEXT("discarded") : TEXT("FAILED"), BuildMs);
            if (IsValid(Actor) && !F.bEverSwapped)
            {
                ReleaseActor(Actor);
                InFlightBuilds.Remove(Key);
            }
        }
    }
}

namespace JUSyncCloudRamp
{
    static float StepFraction(int32 K, int32 Total)
    {
        return FMath::Clamp(float(K) / float(FMath::Max(Total, 1)), 0.f, 1.f);
    }
}

bool FJUSYNCPointCloudSpawner::ShouldStartCloudRamp(const FString& Key, double NowSeconds) const
{
    const bool bBlendPossible = CVarJusyncCloudSmooth.GetValueOnGameThread() > 0
        || CVarJusyncCloudMorphSteps.GetValueOnGameThread() > 0;
    if (!bBlendPossible || bShuttingDown.load())
    {
        return false;
    }

    const TArray<FVector3f>* Display = CloudDisplayPos.Find(Key);
    if (!Display || Display->Num() == 0)
    {
        return false;
    }

    return (NowSeconds - CloudLastArrival.FindRef(Key)) >= 0.012;
}

void FJUSYNCPointCloudSpawner::KickRampStep(const FString& Key, FCloudRampChain& Chain)
{
    if (Chain.bFinalKicked || Chain.Step >= Chain.TotalSteps)
    {
        return;
    }

    // One build in flight per key (pipeline invariant): wait for the slot.

    if (const TSharedRef<FInFlightCloudBuild>* F = InFlightBuilds.Find(Key))
    {
        if ((*F)->bBuilding || (*F)->bDeferred)
        {
            return;
        }
    }

    ++Chain.Step;
    const bool bFinal = (Chain.Step == Chain.TotalSteps);
    double ApplyAt = Chain.T0 + Chain.Duration * double(Chain.Step) / double(Chain.TotalSteps);

    const double NowSeconds = FPlatformTime::Seconds();
    if (ApplyAt <= NowSeconds + 0.0005)
    {
        ApplyAt = NowSeconds + Chain.Duration / double(Chain.TotalSteps);
        Chain.T0 = ApplyAt - Chain.Duration * double(Chain.Step) / double(Chain.TotalSteps);
    }

    if (bFinal)
    {
        Chain.bFinalKicked = true;
        StartAsyncCloudBuild(Key, MoveTemp(Chain.Entry), Chain.Actor, ApplyAt, /*bChainStep*/true, /*bChainFinal*/true);
        return;
    }

    const int32 PointCount = Chain.Entry.Points.Num();
    TArray64<FLidarPointCloudPoint> Lerped = Chain.Entry.Points; // 20 B/pt copy per step
    const float S = JUSyncCloudRamp::StepFraction(Chain.Step, Chain.TotalSteps);

    for (int32 i = 0; i < PointCount; ++i)
    {
        FLidarPointCloudPoint& P = Lerped[i];
        if (Chain.PrevPos.IsValidIndex(i))
        {
            P.Location = FMath::Lerp(Chain.PrevPos[i], P.Location, S);
        }
    }

    FPointCloudReadyEntry StepEntry;
    StepEntry.Key = Key;
    StepEntry.Points = MoveTemp(Lerped);
    StartAsyncCloudBuild(Key, MoveTemp(StepEntry), Chain.Actor, ApplyAt, /*bChainStep*/true, /*bChainFinal*/false);
}

void FJUSYNCPointCloudSpawner::AbortDeferredSwap(const FString& Key)
{
    if (TSharedRef<FInFlightCloudBuild>* F = InFlightBuilds.Find(Key))
    {
        if ((*F)->bChainStep && ((*F)->bBuilding || (*F)->bDeferred))
        {
            (*F)->bChainAborted = true;
        }
    }
}

void FJUSYNCPointCloudSpawner::TickCloudRamps(double NowSeconds)
{
    for (TPair<FString, TSharedRef<FInFlightCloudBuild>>& Pair : InFlightBuilds)
    {
        FInFlightCloudBuild& F = *Pair.Value;
        if (F.bDeferred && NowSeconds >= F.EarliestApply)
        {
            OnAsyncCloudBuildFinished(Pair.Key, F.Generation, true);
        }
    }

    for (auto It = RampChains.CreateIterator(); It; ++It)
    {
        FCloudRampChain& Chain = It.Value();

        if (!IsValid(Chain.Actor) || FindActorByElementKey(It.Key()) != Chain.Actor)
        {
            It.RemoveCurrent();
            continue;
        }

        KickRampStep(It.Key(), Chain);
    }
}


namespace JUSyncCloudSmooth
{
    static inline int32 QKey(const FVector3f& V)
    {
        const FIntVector Q(
            FMath::RoundToInt(V.X * 100.f),
            FMath::RoundToInt(V.Y * 100.f),
            FMath::RoundToInt(V.Z * 100.f));
        return GetTypeHash(Q);
    }

    static ULidarPointCloud* GetDisplayedCloud(AActor* Actor)
    {
        if (ALidarPointCloudActor* LA = Cast<ALidarPointCloudActor>(Actor))
        {
            if (ULidarPointCloudComponent* Comp = LA->GetPointCloudComponent())
            {
                return Comp->GetPointCloud();
            }
        }
        return nullptr;
    }
}

bool FJUSYNCPointCloudSpawner::StartCloudSmoothMorph(const FString& Key, FPointCloudReadyEntry& Entry, AActor* Actor,
                                                     double T0, double Duration)
{
    const int32 NewCount = Entry.Points.Num();
    ULidarPointCloud* Cloud = JUSyncCloudSmooth::GetDisplayedCloud(Actor);
    const TArray<FVector3f>* Displayed = CloudDisplayPos.Find(Key);
    if (NewCount == 0 || Duration <= 0.0 || !IsValid(Cloud) || !Displayed || Displayed->Num() == 0)
    {
        return false;
    }

    FCloudSmoothMorph Morph;
    Morph.Actor = Actor;
    Morph.T0 = T0;
    Morph.Duration = Duration;

    const int32 ExpectedPts = Displayed->Num();
    Morph.Start.Reserve(ExpectedPts);
    Morph.End.Reserve(ExpectedPts);

    const TArray<int32>* KnownMap = CloudWalkInputIdx.Find(Key);
    const bool bReuseMap = (KnownMap && KnownMap->Num() == ExpectedPts);

    const FVector3f NegOffset = (FVector3f)(-Cloud->LocationOffset);

    TMap<int32, int32> QuantToInput;
    if (!bReuseMap)
    {
        QuantToInput.Reserve(ExpectedPts);
        for (int32 i = 0; i < ExpectedPts; ++i)
        {
            QuantToInput.Add(JUSyncCloudSmooth::QKey((*Displayed)[i] + NegOffset), i);
        }
    }

    const TArray64<FLidarPointCloudPoint>& NewPts = Entry.Points;
    TArray<int32> WalkToInput;
    WalkToInput.Reserve(ExpectedPts);
    int32 Held = 0;

    const auto Capture = [&](FLidarPointCloudPoint* P)
    {
        int32 Src = INDEX_NONE;
        if (bReuseMap)
        {
            Src = (*KnownMap)[Morph.Start.Num()];
        }
        else if (const int32* Found = QuantToInput.Find(JUSyncCloudSmooth::QKey(P->Location)))
        {
            Src = *Found;
        }
        WalkToInput.Add(Src);

        Morph.Start.Add(P->Location);
        Morph.End.Add((Src != INDEX_NONE && Src < NewCount) ? NewPts[Src].Location + NegOffset : P->Location);
        Morph.NewColors.Add((Src != INDEX_NONE && Src < NewCount) ? NewPts[Src].Color : P->Color);
        if (Src == INDEX_NONE || Src >= NewCount)
        {
            ++Held;
        }
    };
    Cloud->ExecuteActionOnAllPoints(Capture, /*bVisibleOnly*/false);

    const int32 Walked = Morph.Start.Num();
    if (Walked == 0)
    {
        return false;
    }

    if (Held > FMath::Max(64, Walked / 4))
    {
        UE_LOG(LogTemp, Log, TEXT("JUSYNC CloudMorph: '%s' correspondence %d/%d unmatched -> ramp fallback"),
               *Key, Held, Walked);
        return false;
    }

    if (!bReuseMap)
    {
        CloudWalkInputIdx.Add(Key, MoveTemp(WalkToInput));
    }

    if (CVarJusyncCloudRenderSmoothing.GetValueOnGameThread() <= 0)
    {
        ULidarPointCloudSettings* LidarSettings = GetMutableDefault<ULidarPointCloudSettings>();
        if (LidarSettings->bUseRenderDataSmoothing)
        {
            LidarSettings->bUseRenderDataSmoothing = false;
            UE_LOG(LogTemp, Log, TEXT("JUSYNC CloudMorph: disabled LidarPointCloud render-data smoothing "
                                      "(per-frame upload consistency; re-enable via Project Settings or "
                                      "jusync.Morph.CloudRenderSmoothing 1)"));
        }
    }

    UE_LOG(LogTemp, Log, TEXT("JUSYNC CloudMorph: SMOOTH START '%s' walk=%d new=%d held=%d blend=%.0fms cloudActors=%d off=%.1fcm%s"),
           *Key, Walked, NewCount, Held, Duration * 1000.0, ElementToActor.Num(),
           Cloud->LocationOffset.Size(), bReuseMap ? TEXT(" (retarget)") : TEXT(""));

    SmoothMorphs.Add(Key, MoveTemp(Morph));
    return true;
}

void FJUSYNCPointCloudSpawner::AdvanceCloudSmoothMorphs(double NowSeconds)
{
    for (auto It = SmoothMorphs.CreateIterator(); It; ++It)
    {
        FCloudSmoothMorph& M = It.Value();

        ULidarPointCloud* Cloud = JUSyncCloudSmooth::GetDisplayedCloud(M.Actor.Get());
        if (!IsValid(Cloud) || FindActorByElementKey(It.Key()) != M.Actor.Get())
        {
            CloudWalkInputIdx.Remove(It.Key());
            It.RemoveCurrent();
            continue;
        }

        const float U = FMath::Clamp(float((NowSeconds - M.T0) / M.Duration), 0.f, 1.f);
        const float S = U * U * (3.f - 2.f * U); // continuous motion -> smoothstep easing is correct

        int32 Slot = 0;
        const int32 Num = M.Start.Num();
        const auto Write = [&](FLidarPointCloudPoint* P)
        {
            if (Slot < Num)
            {
                P->Location = FMath::Lerp(M.Start[Slot], M.End[Slot], S);
                if (Slot < M.NewColors.Num())
                {
                    P->Color = M.NewColors[Slot];
                }
                ++Slot;
            }
        };
        Cloud->ExecuteActionOnAllPoints(Write, /*bVisibleOnly*/false);

        if (U >= 1.f)
        {
            It.RemoveCurrent();
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

    if (CVarJusyncCloudFixedSize.GetValueOnGameThread() > 0)
    {
        Comp->ScalingMethod = ELidarPointCloudScalingMethod::FixedScreenSize;
        Comp->PointSize = FMath::Max(0.0f, CVarJusyncCloudScreenSize.GetValueOnGameThread());
    }

    if (Comp->GetPointShape() != PointShape)
    {
        Comp->SetPointShape(PointShape);
    }
}

void FJUSYNCPointCloudSpawner::ApplySizingToActiveActors()
{
    for (AActor* Actor : ActiveActors)
    {
        if (ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor))
        {
            if (ULidarPointCloudComponent* Comp = LidarActor->GetPointCloudComponent())
            {
                ApplyVisualSettings(Comp);
                Comp->MarkRenderStateDirty();
            }
        }
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
        // Reuse the cloud that has been idle longest, and only take one that is
        // mid-morph when nothing else is left. Every rank owns a piece of the same
        // element, so the pool is tight under multi-rank streaming; the previous
        // "first entry" pick was TSet order and could take the actor a rank is
        // actively ramping on, leaving that element's key pointing at an actor that
        // now shows another rank's points.
        AActor* Victim = nullptr;
        FString VictimKey;
        bool bVictimMorphing = false;
        double VictimStamp = TNumericLimits<double>::Lowest();

        for (AActor* Candidate : ActiveActors)
        {
            if (!Candidate || !Candidate->IsValidLowLevel())
            {
                Victim = Candidate;
                VictimKey.Reset();
                bVictimMorphing = false;
                break;
            }

            const FString* Key = ActorToElement.Find(Candidate);
            const double Stamp = (Key && CloudLastArrival.Contains(*Key))
                ? CloudLastArrival[*Key]
                : TNumericLimits<double>::Lowest();
            const bool bMorphing = Key && (RampChains.Contains(*Key) || SmoothMorphs.Contains(*Key));

            bool bBetter;
            if (!Victim)
            {
                bBetter = true;
            }
            else if (bMorphing != bVictimMorphing)
            {
                bBetter = !bMorphing;
            }
            else
            {
                bBetter = Stamp < VictimStamp;
            }

            if (bBetter)
            {
                Victim = Candidate;
                VictimKey = Key ? *Key : FString();
                bVictimMorphing = bMorphing;
                VictimStamp = Stamp;
            }
        }

        if (Victim && Victim->IsValidLowLevel())
        {
            if (bVictimMorphing)
            {
                UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: cloud pool full (%d), reusing the actor of mid-morph element '%s' - raise PointCloudPoolSize for multi-rank streams"),
                    MaxPoolSize, *VictimKey);
            }

            ActiveActors.Remove(Victim);
            UntrackActorElement(Victim);
            Victim->SetActorHiddenInGame(false);
#ifdef WITH_ANARI_USD_MIDDLEWARE
            ApplyVisualSettingsToActor(Victim);
#endif
            ApplyTransformToActor(Victim);
            ActiveActors.Add(Victim);
            return Victim;
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
#ifdef WITH_ANARI_USD_MIDDLEWARE
    const double NowSeconds = FPlatformTime::Seconds();
    TickCloudRamps(NowSeconds);
    AdvanceCloudSmoothMorphs(NowSeconds);

    const int32 FixedNow = CVarJusyncCloudFixedSize.GetValueOnGameThread();
    const float ScreenNow = CVarJusyncCloudScreenSize.GetValueOnGameThread();
    if (FixedNow != LastFixedSizeState || ScreenNow != LastFixedScreenSize)
    {
        LastFixedSizeState = FixedNow;
        LastFixedScreenSize = ScreenNow;
        ApplySizingToActiveActors();
    }
#endif
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
            Entry.Key = JUSYNCElementNames::MakeRankedElementKey(Entry.ElementName, Entry.Rank);
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

        ApplyTransformToActor(Actor);

#ifdef WITH_ANARI_USD_MIDDLEWARE
        if (CVarJusyncAsyncCloudBuild.GetValueOnGameThread() > 0
            && Entry.Points.Num() > 0 && !Entry.bNeedsRecolor)
        {
            if (!bInPlaceUpdate)
            {
                ElementToActor.Add(Entry.Key, Actor);
                ActorToElement.Add(Actor, Entry.Key);
            }
            Actor->SetActorEnableCollision(false);

            const double NowSeconds = FPlatformTime::Seconds();
            const FString Key = Entry.Key;

            if (bInPlaceUpdate && ShouldStartCloudRamp(Key, NowSeconds))
            {
                AbortDeferredSwap(Key);
                RampChains.Remove(Key);

                const int32 ForcedMs = CVarJusyncCloudBlendMs.GetValueOnGameThread();
                const double MaxBlend = FMath::Max(CVarJusyncCloudMaxBlendMs.GetValueOnGameThread(), 8) / 1000.0;
                const double BlendSeconds = (ForcedMs > 0)
                    ? ForcedMs / 1000.0
                    : FMath::Clamp(NowSeconds - CloudLastArrival.FindRef(Key), 0.008, MaxBlend);
                CloudLastArrival.Add(Key, NowSeconds);

                if (CVarJusyncCloudSmooth.GetValueOnGameThread() > 0
                    && StartCloudSmoothMorph(Key, Entry, Actor, NowSeconds, BlendSeconds))
                {
                    StartAsyncCloudBuild(Key, MoveTemp(Entry), Actor, NowSeconds + BlendSeconds);
                }
                else
                {
                    FCloudRampChain& Chain = RampChains.FindOrAdd(Key);
                    Chain.PrevPos = CloudDisplayPos.FindRef(Key);
                    Chain.Entry = MoveTemp(Entry);
                    Chain.Actor = Actor;
                    Chain.T0 = NowSeconds;
                    Chain.Duration = BlendSeconds;
                    Chain.TotalSteps = FMath::Clamp(CVarJusyncCloudMorphSteps.GetValueOnGameThread(), 1, 6) + 1;

                    UE_LOG(LogTemp, Log, TEXT("JUSYNC CloudRamp: chain START '%s' prev=%d -> new=%d pts, %d interp step(s) over %.0f ms"),
                           *Key, Chain.PrevPos.Num(), Chain.Entry.Points.Num(), Chain.TotalSteps - 1, Chain.Duration * 1000.0);

                    KickRampStep(Key, Chain); // overlaps step-1 build with the hold window
                }
            }
            else
            {
                CloudLastArrival.Add(Key, NowSeconds);
                RampChains.Remove(Key);
                StartAsyncCloudBuild(Key, MoveTemp(Entry), Actor);
            }
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

#ifdef WITH_ANARI_USD_MIDDLEWARE
    RampChains.Reset();
    CloudDisplayPos.Reset();
    CloudLastArrival.Reset();
    SmoothMorphs.Reset();
    CloudWalkInputIdx.Reset();
#endif

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
