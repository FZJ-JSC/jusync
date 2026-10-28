#include "JUSYNCPointCloudSpawner.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformTime.h"
#include "JUSYNCElementNames.h"

#ifdef WITH_ANARI_USD_MIDDLEWARE
#include "LidarPointCloud.h"
#include "LidarPointCloudComponent.h"
#include "LidarPointCloudActor.h"
#include "LidarPointCloudSettings.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"

static TAutoConsoleVariable<int32> CVarJusyncCloudMorphSteps(
    TEXT("jusync.Morph.CloudSteps"), 4,
    TEXT("Number of pre-interpolated cloud builds streamed between successive .geo time samples (1..6)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncCloudBlendMs(
    TEXT("jusync.Morph.CloudBlendMs"), 0,
    TEXT("Fixed cloud blend window in ms; 0 = auto (measured sample gap clamped by CloudMaxBlendMs)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncCloudMaxBlendMs(
    TEXT("jusync.Morph.CloudMaxBlendMs"), 200,
    TEXT("Cap in ms for the auto cloud blend window. Kept short so a swap always\n")
    TEXT("completes between streamed samples instead of being retargeted forever."),
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

static TAutoConsoleVariable<FString> CVarJusyncCloudMaterial(
    TEXT("jusync.Spawner.CloudMaterial"),
    TEXT("/Game/Materials/M_JusyncCloud"),
    TEXT("Material applied to streamed LidarPointCloud components (slot 0). Default M_JusyncCloud is\n")
    TEXT("non-emissive (built by Tools/CreateJusyncCloudMaterial.py); empty = plugin default material."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarJusyncCloudEmissiveFloor(
    TEXT("jusync.Spawner.CloudEmissiveFloor"), 0.004f,
    TEXT("M_JusyncCloud 'EmissiveFloor' param: point color * this goes to emissive. Keep <=1 to stay\n")
    TEXT("below the bloom threshold."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarJusyncCloudScreenSize(
    TEXT("jusync.Morph.CloudScreenSize"), 2.7f,
    TEXT("Point size (screen-fraction units) used while jusync.Morph.CloudFixedSize is on."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncLockSilhouette(
    TEXT("jusync.Morph.LockSilhouette"), 1,
    TEXT("1 (default) = compensate point-cloud actor scale so changing POINT SIZE keeps the\n")
    TEXT("cloud's outer width/height stable instead of inflating the silhouette by the sprite radius."),
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
}  // namespace

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

    if (bPlaybackMode)
    {
        return;
    }

    for (FPendingNoColorCloud& Pending : ToFlush)
    {
        EnqueuePointCloud(MoveTemp(Pending.Data), Pending.Rank);
    }
}

void FJUSYNCPointCloudSpawner::FlushExpiredNoColorClouds()
{
    if (bPlaybackMode)
    {
        FScopeLock Lock(&QueueMutex);
        PendingNoColorClouds.Empty();
        return;
    }

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
        EnqueuePointCloud(MoveTemp(Pending.Data), Pending.Rank,  !bLUTReady);
    }
}

bool FJUSYNCPointCloudSpawner::EnqueuePointCloud(FJUSYNCPointCloudRef&& PCRef, int32 InRank)
{
    if (!PCRef || !PCRef->IsValid())
    {
        return false;
    }

    if (PCRef.IsUnique())
    {
        FJUSYNCPointCloudData Owned = MoveTemp(*PCRef);
        return EnqueuePointCloud(MoveTemp(Owned), InRank);
    }

    FJUSYNCPointCloudData Copy = *PCRef;
    return EnqueuePointCloud(MoveTemp(Copy), InRank);
}

bool FJUSYNCPointCloudSpawner::EnqueuePointCloudForPlayback(FJUSYNCPointCloudRef&& PCRef, int32 InRank)
{
    if (!PCRef || !PCRef->IsValid() || !Owner.IsValid() || bShuttingDown.load())
    {
        return false;
    }

#ifdef WITH_ANARI_USD_MIDDLEWARE
    const bool bMorphPlayback = CVarJusyncCloudBlendMs.GetValueOnGameThread() > 0;

    if (!bMorphPlayback)
    {
        const JUSYNCElementNames::FVariant ReplayVariant =
            JUSYNCElementNames::ParseElement(PCRef->ElementName);
        const FString CanonicalElement = !ReplayVariant.Element.IsEmpty()
            ? JUSYNCElementNames::CanonicalActorIdentity(ReplayVariant.Element)
            : PCRef->ElementName;
        if (TryDisplayCachedPlaybackCloud(PCRef,
            JUSYNCElementNames::MakeRankedElementKey(CanonicalElement, InRank)))
        {
            return true;
        }
    }
#endif

    FJUSYNCPointCloudData PCData = PCRef.IsUnique() ? MoveTemp(*PCRef) : *PCRef;

    const JUSYNCElementNames::FVariant ElementVariant =
        JUSYNCElementNames::ParseElement(PCData.ElementName);
    if (!ElementVariant.Element.IsEmpty())
    {
        PCData.ElementName = JUSYNCElementNames::CanonicalActorIdentity(ElementVariant.Element);
    }

    if (IsElementFiltered(PCData.ElementName))
    {
        return false;
    }

#ifdef WITH_ANARI_USD_MIDDLEWARE
    FPointCloudReadyEntry Entry;
    Entry.ElementName = PCData.ElementName;
    Entry.Key = JUSYNCElementNames::MakeRankedElementKey(PCData.ElementName, InRank);
    Entry.Rank = InRank;
    Entry.PointSize = PointSize;
    Entry.bSpawned = false;
    Entry.bNeedsRecolor = false;
    Entry.bPlaybackMorph = bMorphPlayback;
    Entry.SourcePayload = PCRef;

    if (PCData.HasLidarPoints())
    {
        Entry.Points = MoveTemp(PCData.LidarPoints);
        if (!PCData.HasBakedColors() && PCData.Widths.Num() > 0)
        {
            const TArray<FColor> LUT = GetGradientLUT();
            if (LUT.Num() > 0)
            {
                ParallelFor(static_cast<int32>(Entry.Points.Num()), [&](int32 i)
                {
                    const float Attr0 = (i < PCData.Widths.Num()) ? PCData.Widths[i] : 0.0f;
                    const int32 LUTIdx = FMath::Clamp(
                        FMath::RoundToInt(Attr0 * static_cast<float>(LUT.Num() - 1)),
                        0,
                        LUT.Num() - 1);
                    Entry.Points[i].Color = LUT[LUTIdx];
                });
            }
        }
    }
    else
    {
        const int32 PointCount = PCData.Positions.Num();
        if (PointCount <= 0)
        {
            return false;
        }

        const bool bHasColors = PCData.Colors.Num() == PointCount;
        const TArray<FColor> LUT = GetGradientLUT();
        const bool bUseGradient = LUT.Num() > 0;
        Entry.Points.SetNum(PointCount);
        for (int32 i = 0; i < PointCount; ++i)
        {
            const FVector& P = PCData.Positions[i];
            FColor Col;
            if (bHasColors)
            {
                Col = PCData.Colors[i];
            }
            else if (bUseGradient && PCData.Widths.Num() > i)
            {
                const int32 LUTIdx = FMath::Clamp(
                    FMath::RoundToInt(PCData.Widths[i] * static_cast<float>(LUT.Num() - 1)),
                    0,
                    LUT.Num() - 1);
                Col = LUT[LUTIdx];
            }
            else
            {
                Col = FColor(128, 128, 128);
            }

            Entry.Points[i] = FLidarPointCloudPoint(
                P.X, P.Y, P.Z,
                Col.R / 255.f, Col.G / 255.f, Col.B / 255.f, Col.A / 255.f);
        }
    }

    if (Entry.Points.Num() == 0)
    {
        return false;
    }

    {
        FScopeLock Lock(&QueueMutex);
        ReadyQueue.Add(MoveTemp(Entry));
        DrainReadyQueueLocked();
    }
    return true;
#else
    (void)InRank;
    return false;
#endif
}

bool FJUSYNCPointCloudSpawner::EnqueuePointCloud(const FJUSYNCPointCloudData& PCData, int32 InRank)
{
    return EnqueuePointCloud(FJUSYNCPointCloudData(PCData), InRank);
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

bool FJUSYNCPointCloudSpawner::HasAppliedFileHash(const FString& Filename, uint64 HashLo, uint64 HashHi) const
{
    if (Filename.IsEmpty())
    {
        return false;
    }
    const TPair<uint64, uint64>* Entry = AppliedFileHashes.Find(Filename);
    return Entry && Entry->Key == HashLo && Entry->Value == HashHi;
}

bool FJUSYNCPointCloudSpawner::EnqueuePointCloud(FJUSYNCPointCloudData&& PCData, int32 InRank, bool bAllowNoGradient)
{
    if (!PCData.IsValid() || !Owner.IsValid() || bShuttingDown.load()) return false;

    {
        const JUSYNCElementNames::FVariant ElementVariant =
            JUSYNCElementNames::ParseElement(PCData.ElementName);
        if (!ElementVariant.Element.IsEmpty())
        {
            const FString StableElement = JUSYNCElementNames::CanonicalActorIdentity(ElementVariant.Element);
            UE_LOG(LogTemp, Verbose, TEXT("JUSYNC Spawner: canonical cloud '%s' onto '%s'"),
                *PCData.ElementName, *StableElement);
            PCData.ElementName = StableElement;
        }
    }

    if (InRank < 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: cloud '%s' arrived with no owner rank; morph state keyed as rank 0"), *PCData.ElementName);
    }

    if (IsElementFiltered(PCData.ElementName))
    {
        UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: SKIPPED filtered PC '%s' (%d points)"),
               *PCData.ElementName, PCData.PointCount);
        return false;
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
                return true;
            }
        }

        if (!BeginTask())
        {
            return false;
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
                    if (bPlaybackMode)
                    {
                        return;
                    }
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
        return true;
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
            return true;
        }
    }

    if (!BeginTask())
    {
        return false;
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
                if (bPlaybackMode)
                {
                    return;
                }
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
    return true;
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

    if (!bChainStep && F->bDeferred && !F->bBuilding)
    {
        const uint64 DeferredGeneration = F->Generation;
        OnAsyncCloudBuildFinished(Key, DeferredGeneration, true);

        Existing = InFlightBuilds.Find(Key);
        if (Existing)
        {
            F = *Existing;
        }
        else
        {
            F = MakeShared<FInFlightCloudBuild>();
            InFlightBuilds.Add(Key, F);
        }
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
    F->SourcePayload = MoveTemp(Entry.SourcePayload);
    F->Generation = ++NextCloudBuildGeneration;
    F->bBuilding = true;
    F->BuildStarted = FPlatformTime::Seconds();

    F->EarliestApply = EarliestApply;
    F->bDeferred = false;
    F->bChainStep = bChainStep;
    F->bChainFinal = bChainFinal;
    F->bChainAborted = false;

    F->RampPos.SetNum(F->Points->Num());
    ParallelFor(static_cast<int32>(F->Points->Num()), [&](int32 i)
    {
        F->RampPos[i] = (*F->Points)[i].Location;
    });

    const uint64 Generation = F->Generation;
    const int32 NumPts = static_cast<int32>(F->Points->Num());

    FLidarPointCloudAsyncParameters Params(
         true,
         nullptr,
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
                if (!IsPlaybackCachedCloud(Stale))
                {
                    Stale->MarkAsGarbage();
                }
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
                    NextApply = M->T0 + M->Duration;  // land when the glide ends
                }
                StartAsyncCloudBuild(Key, MoveTemp(Next), NextActor, NextApply);
            }
            return;
        }

        if (bSuccess && (NowSeconds < F.EarliestApply || SmoothMorphs.Contains(Key)))
        {
            F.bBuilding = false;
            F.bDeferred = true;
            return;
        }

        ULidarPointCloud* Cloud = F.CloudRef.IsValid() ? Cast<ULidarPointCloud>(F.CloudRef->Get()) : nullptr;
        F.CloudRef.Reset();
        F.bBuilding = false;

        AActor* Actor = F.Actor.Get();
        bool bSwapped = false;
        ULidarPointCloud* ReplacedCloud = nullptr;

        if (bSuccess && IsValid(Cloud) && IsValid(Actor) && !bShuttingDown.load()
            && FindActorByElementKey(Key) == Actor)
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
                        ReplacedCloud->ExecuteActionOnAllPoints(Probe,  false);  // bVisibleOnly
                        UE_LOG(LogTemp, Display,
                               TEXT("JUSYNC CloudMorph: SMOOTH RESID '%s' checked=%d unmatched=%d residMax=%.4fcm oldOff=%.2f newOff=%.2f map=%d ramp=%d walk=%d"),
                               *Key, Checked, Unmatched, ResidMax,
                               ReplacedCloud->LocationOffset.Size(), Cloud->LocationOffset.Size(),
                               Map->Num(), F.RampPos.Num(), (int32)ReplacedCloud->GetNumPoints());
                    }

                    Cloud->SetOptimizedForDynamicData(true);

                    Comp->SetPointCloud(Cloud);
                    ApplyVisualSettings(Comp);
                    ApplyGradientColorsToCloud(Cloud);
                    ApplyPointHueFilterToCloud(Cloud);
                    Actor->SetActorHiddenInGame(bPointCloudsHidden);
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

                    if (F.SourcePayload.IsValid())
                    {
                        StorePlaybackCloud(Key, MoveTemp(F.SourcePayload), Cloud);
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
            if (!IsPlaybackCachedCloud(ReplacedCloud))
            {
                ReplacedCloud->MarkAsGarbage();
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
            if (IsValid(Actor) && !F.bEverSwapped && FindActorByElementKey(Key) != Actor)
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
        StartAsyncCloudBuild(Key, MoveTemp(Chain.Entry), Chain.Actor, ApplyAt,  true,  true);  // bChainStepbChainFinal
        return;
    }

    const int32 PointCount = Chain.Entry.Points.Num();
    TArray64<FLidarPointCloudPoint> Lerped = Chain.Entry.Points;  // 20 B/pt copy per step
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
    StartAsyncCloudBuild(Key, MoveTemp(StepEntry), Chain.Actor, ApplyAt,  true,  false);  // bChainStepbChainFinal
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

void FJUSYNCPointCloudSpawner::EnterPlaybackMode()
{
    bPlaybackMode = true;

    {
        FScopeLock Lock(&QueueMutex);
        ReadyQueue.Empty();
        PendingNoColorClouds.Empty();
    }

    RampChains.Empty();
    SmoothMorphs.Empty();
    CloudWalkInputIdx.Empty();
    CloudDisplayPos.Empty();
    CloudLastArrival.Empty();

    TArray<FString> Keys;
    InFlightBuilds.GetKeys(Keys);
    for (const FString& Key : Keys)
    {
        PreparePointCloudForPlayback(Key);
    }
}

void FJUSYNCPointCloudSpawner::SeedCloudDisplayPos(const FString& Key, AActor* InActor)
{
    if (CloudDisplayPos.Contains(Key))
    {
        return;
    }

    AActor* Actor = InActor ? InActor : FindActorByElementKey(Key);
    if (ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor))
    {
        if (ULidarPointCloudComponent* Comp = LidarActor->GetPointCloudComponent())
        {
            if (ULidarPointCloud* Cloud = Comp->GetPointCloud())
            {
                const FVector3f Offset = (FVector3f)Cloud->LocationOffset;
                TArray<FVector3f> Displayed;
                Displayed.Reserve(Cloud->GetNumPoints());
                Cloud->ExecuteActionOnAllPoints(
                    [&](FLidarPointCloudPoint* P) { Displayed.Add(P->Location + Offset); },
                     false);  // bVisibleOnly
                if (Displayed.Num() > 0)
                {
                    CloudDisplayPos.Add(Key, MoveTemp(Displayed));
                }
            }
        }
    }
}

void FJUSYNCPointCloudSpawner::PreparePointCloudForPlayback(const FString& Key)
{
    RampChains.Remove(Key);
    SmoothMorphs.Remove(Key);
    CloudLastArrival.Remove(Key);

    if (TSharedRef<FInFlightCloudBuild>* Found = InFlightBuilds.Find(Key))
    {
        FInFlightCloudBuild& F = **Found;
        F.bHasPending = false;
        F.PendingReplacement = FPointCloudReadyEntry();
        F.bChainStep = false;
        F.bChainFinal = false;

        if (F.bBuilding)
        {
            F.bChainAborted = true;
        }
        else if (F.bDeferred)
        {
            F.EarliestApply = 0.0;
            const uint64 Generation = F.Generation;
            OnAsyncCloudBuildFinished(Key, Generation, true);
            InFlightBuilds.Remove(Key);
        }
        else
        {
            InFlightBuilds.Remove(Key);
        }
    }

    SeedCloudDisplayPos(Key);
}

void FJUSYNCPointCloudSpawner::TickCloudRamps(double NowSeconds)
{
    TArray<TPair<FString, uint64>> DeferredLandings;
    for (const TPair<FString, TSharedRef<FInFlightCloudBuild>>& Pair : InFlightBuilds)
    {
        const FInFlightCloudBuild& F = *Pair.Value;
        if (F.bDeferred && NowSeconds >= F.EarliestApply)
        {
            DeferredLandings.Add(TPair<FString, uint64>(Pair.Key, F.Generation));
        }
    }
    for (const TPair<FString, uint64>& Landing : DeferredLandings)
    {
        OnAsyncCloudBuildFinished(Landing.Key, Landing.Value, true);
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

int32 FJUSYNCPointCloudSpawner::FindPlaybackReuseIndex(const FJUSYNCPointCloudRef& Payload) const
{
    if (!Payload.IsValid())
    {
        return INDEX_NONE;
    }
    for (int32 i = 0; i < PlaybackCloudReuse.Num(); ++i)
    {
        if (PlaybackCloudReuse[i].Payload == Payload)
        {
            return i;
        }
    }
    return INDEX_NONE;
}

bool FJUSYNCPointCloudSpawner::TryDisplayCachedPlaybackCloud(
    const FJUSYNCPointCloudRef& Payload, const FString& Key)
{
    const int32 HitIdx = FindPlaybackReuseIndex(Payload);
    if (HitIdx == INDEX_NONE)
    {
        return false;
    }

    ULidarPointCloud* Built = PlaybackCloudReuse[HitIdx].Cloud.IsValid()
        ? Cast<ULidarPointCloud>(PlaybackCloudReuse[HitIdx].Cloud.Get()) : nullptr;
    if (!Built || Built->HasAnyFlags(RF_BeginDestroyed))
    {
        PlaybackCloudReuse.RemoveAt(HitIdx);
        return false;
    }

    AActor* Actor = FindActorByElementKey(Key);
    ALidarPointCloudActor* LidarActor = IsValid(Actor) ? Cast<ALidarPointCloudActor>(Actor) : nullptr;
    ULidarPointCloudComponent* Comp = LidarActor ? LidarActor->GetPointCloudComponent() : nullptr;
    if (!Comp)
    {
        return false;
    }

    PreparePointCloudForPlayback(Key);

    if (Comp->GetPointCloud() != Built)
    {
        Built->SetOptimizedForDynamicData(true);
        Comp->SetPointCloud(Built);
        ApplyVisualSettings(Comp);
        ApplyGradientColorsToCloud(Built);
        ApplyPointHueFilterToCloud(Built);
    }
    LidarActor->SetActorHiddenInGame(bPointCloudsHidden);

    TArray<FVector3f> Displayed;
    Displayed.Reserve(Built->GetNumPoints());
    Built->ExecuteActionOnAllPoints(
        [&](FLidarPointCloudPoint* P) { Displayed.Add(P->Location); },
         false);  // bVisibleOnly
    if (Displayed.Num() > 0)
    {
        CloudDisplayPos.Add(Key, MoveTemp(Displayed));
    }

    PlaybackCloudReuse[HitIdx].LastUsedSeconds = FPlatformTime::Seconds();
    UE_LOG(LogTemp, Log, TEXT("JUSYNC CloudReplay: instant revisit '%s' (%d pts, no rebuild)"),
        *Key, Built->GetNumPoints());
    OnPointCloudSpawned.Broadcast(Key, Actor);
    return true;
}

int32 FJUSYNCPointCloudSpawner::FindEvictableReuseIndex()
{
    int32 EvictIdx = INDEX_NONE;
    double Oldest = TNumericLimits<double>::Max();
    for (int32 i = 0; i < PlaybackCloudReuse.Num(); ++i)
    {
        const FPlaybackCloudReuse& Cand = PlaybackCloudReuse[i];
        if (EvictIdx != INDEX_NONE && Cand.LastUsedSeconds >= Oldest)
        {
            continue;
        }
        AActor* CandActor = FindActorByElementKey(Cand.Key);
        ALidarPointCloudActor* CandLidar = IsValid(CandActor)
            ? Cast<ALidarPointCloudActor>(CandActor) : nullptr;
        ULidarPointCloudComponent* CandComp = CandLidar ? CandLidar->GetPointCloudComponent() : nullptr;
        if (CandComp && Cand.Cloud.IsValid() && CandComp->GetPointCloud() == Cand.Cloud.Get())
        {
            continue;
        }
        Oldest = Cand.LastUsedSeconds;
        EvictIdx = i;
    }
    return EvictIdx;
}

int64 FJUSYNCPointCloudSpawner::EstimatePayloadBytes(const FJUSYNCPointCloudRef& Payload)
{
    if (!Payload.IsValid())
    {
        return 0;
    }
    int64 Bytes = 128;
    if (Payload->HasLidarPoints())
    {
        Bytes += static_cast<int64>(Payload->LidarPoints.Num()) * sizeof(FLidarPointCloudPoint);
    }
    else
    {
        Bytes += static_cast<int64>(Payload->Positions.Num()) * sizeof(FVector);
        Bytes += static_cast<int64>(Payload->Colors.Num()) * sizeof(FColor);
    }
    Bytes += static_cast<int64>(Payload->Widths.Num()) * sizeof(float);
    return Bytes;
}

int32 FJUSYNCPointCloudSpawner::WarmCloudCountForBudget(int64 BudgetBytes, int64 AvgCloudBytes)
{
    if (BudgetBytes <= 0 || AvgCloudBytes <= 0)
    {
        return MaxPlaybackCloudReuse;  // legacy fixed pool until a budget exists
    }
    const int64 ByBudget = BudgetBytes / AvgCloudBytes;
    return static_cast<int32>(FMath::Clamp<int64>(ByBudget, MinWarmClouds, MaxWarmCloudsHardCap));
}

void FJUSYNCPointCloudSpawner::SetWarmCloudBudgetBytes(int64 InBytes)
{
    const int64 NewBudget = FMath::Max<int64>(0, InBytes);
    if (NewBudget == WarmCloudBudgetBytes)
    {
        return;
    }
    WarmCloudBudgetBytes = NewBudget;

    const int32 PoolCap = WarmCloudCountForBudget(WarmCloudBudgetBytes, WarmCloudAvgBytes);
    while (PlaybackCloudReuse.Num() > PoolCap)
    {
        const int32 EvictIdx = FindEvictableReuseIndex();
        if (EvictIdx == INDEX_NONE)
        {
            return;  // everything left is on screen
        }
        PlaybackCloudReuse.RemoveAt(EvictIdx);
    }
}

void FJUSYNCPointCloudSpawner::StorePlaybackCloud(
    const FString& Key, FJUSYNCPointCloudRef&& Payload, UObject* Cloud)
{
    if (!Payload.IsValid() || !IsValid(Cloud))
    {
        return;
    }

    const int32 ExistingIdx = FindPlaybackReuseIndex(Payload);
    if (ExistingIdx != INDEX_NONE)
    {
        FPlaybackCloudReuse& Existing = PlaybackCloudReuse[ExistingIdx];
        Existing.Cloud.Reset(Cloud);
        Existing.Key = Key;
        Existing.LastUsedSeconds = FPlatformTime::Seconds();
        return;
    }

    const int64 NewBytes = FMath::Max<int64>(1, EstimatePayloadBytes(Payload));
    WarmCloudAvgBytes = WarmCloudAvgBytes > 0
        ? (WarmCloudAvgBytes * 3 + NewBytes) / 4
        : NewBytes;
    const int32 PoolCap = WarmCloudCountForBudget(WarmCloudBudgetBytes, WarmCloudAvgBytes);
    while (PlaybackCloudReuse.Num() >= PoolCap)
    {
        const int32 EvictIdx = FindEvictableReuseIndex();
        if (EvictIdx == INDEX_NONE)
        {
            return;
        }
        PlaybackCloudReuse.RemoveAt(EvictIdx);
    }

    FPlaybackCloudReuse& Fresh = PlaybackCloudReuse.AddDefaulted_GetRef();
    Fresh.LastUsedSeconds = FPlatformTime::Seconds();
    Fresh.Key = Key;
    Fresh.Payload = MoveTemp(Payload);
    Fresh.Cloud.Reset(Cloud);
}

bool FJUSYNCPointCloudSpawner::IsPlaybackCachedCloud(const UObject* Cloud) const
{
    if (!Cloud)
    {
        return false;
    }
    for (const FPlaybackCloudReuse& Reuse : PlaybackCloudReuse)
    {
        if (Reuse.Cloud.IsValid() && Reuse.Cloud.Get() == Cloud)
        {
            return true;
        }
    }
    return false;
}

void FJUSYNCPointCloudSpawner::ClearPlaybackCloudReuse()
{
    PlaybackCloudReuse.Reset();
}

void FJUSYNCPointCloudSpawner::ApplyPointCloudsHiddenToActors()
{
    for (AActor* Actor : ActiveActors)
    {
        if (ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor))
        {
            LidarActor->SetActorHiddenInGame(bPointCloudsHidden);
        }
    }
}

void FJUSYNCPointCloudSpawner::SetPointHueFilter(bool bEnabled, float HueDeg, float ToleranceDeg)
{
    bPointHueFilter = bEnabled;
    PointHue = FMath::Clamp(HueDeg, 0.0f, 360.0f);
    PointHueTolerance = FMath::Clamp(ToleranceDeg, 0.0f, 180.0f);

    for (AActor* Actor : ActiveActors)
    {
        ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor);
        ULidarPointCloudComponent* Comp = LidarActor ? LidarActor->GetPointCloudComponent() : nullptr;
        ApplyPointHueFilterToCloud(Comp ? Comp->GetPointCloud() : nullptr);
    }

    if (!bPointHueFilter)
    {
        TArray<TWeakObjectPtr<UObject>> FilteredClouds;
        PointColorOriginals.GetKeys(FilteredClouds);
        for (const TWeakObjectPtr<UObject>& Key : FilteredClouds)
        {
            ApplyPointHueFilterToCloud(Cast<ULidarPointCloud>(Key.Get()));
        }
        PointColorOriginals.Empty();
    }
    PrunePointColorOriginals();
}

void FJUSYNCPointCloudSpawner::ApplyPointHueFilterToCloud(ULidarPointCloud* Cloud)
{
    if (!IsValid(Cloud) || Cloud->HasAnyFlags(RF_BeginDestroyed))
    {
        return;
    }

    const TWeakObjectPtr<UObject> CloudKey(Cloud);
    if (!bPointHueFilter)
    {
        if (TArray<FColor>* Originals = PointColorOriginals.Find(CloudKey))
        {
            const TArray<FColor>& Restore = *Originals;
            int32 Index = 0;
            Cloud->ExecuteActionOnAllPoints([&Restore, &Index](FLidarPointCloudPoint* P)
            {
                if (Restore.IsValidIndex(Index))
                {
                    P->Color = Restore[Index];
                }
                ++Index;
            },  false);  // bVisibleOnly
            PointColorOriginals.Remove(CloudKey);
            Cloud->RefreshRendering();
        }
        return;
    }

    const int32 NumPts = static_cast<int32>(Cloud->GetNumPoints());
    TArray<FColor>& Originals = PointColorOriginals.FindOrAdd(CloudKey);
    if (Originals.Num() != NumPts)
    {
        Originals.Init(FColor::White, NumPts);
        int32 Capture = 0;
        Cloud->ExecuteActionOnAllPoints(
            [&Originals, &Capture](FLidarPointCloudPoint* P)
            {
                if (Originals.IsValidIndex(Capture))
                {
                    Originals[Capture] = P->Color;
                }
                ++Capture;
            },  false);  // bVisibleOnly

        if (PointColorOriginals.Num() > WarmCloudCountForBudget(WarmCloudBudgetBytes, WarmCloudAvgBytes) + 8)
        {
            PrunePointColorOriginals();
        }
    }

    const float SelectedHue = PointHue;
    const float Tolerance = PointHueTolerance;
    int32 Index = 0;
    Cloud->ExecuteActionOnAllPoints(
        [&Originals, &Index, SelectedHue, Tolerance](FLidarPointCloudPoint* P)
        {
            const FColor Base = Originals.IsValidIndex(Index) ? Originals[Index] : P->Color;
            const FLinearColor RGB(Base);
            const float Max = FMath::Max3(RGB.R, RGB.G, RGB.B);
            const float Min = FMath::Min3(RGB.R, RGB.G, RGB.B);
            const float Delta = Max - Min;
            float HueDeg = 0.0f;
            if (Delta > KINDA_SMALL_NUMBER)
            {
                if (Max == RGB.R)
                {
                    HueDeg = 60.0f * FMath::Fmod((RGB.G - RGB.B) / Delta, 6.0f);
                }
                else if (Max == RGB.G)
                {
                    HueDeg = 60.0f * ((RGB.B - RGB.R) / Delta + 2.0f);
                }
                else
                {
                    HueDeg = 60.0f * ((RGB.R - RGB.G) / Delta + 4.0f);
                }
                if (HueDeg < 0.0f)
                {
                    HueDeg += 360.0f;
                }
            }
            const float Dist = FMath::Abs(HueDeg - SelectedHue);
            const bool bMatch = FMath::Min(Dist, 360.0f - Dist) <= Tolerance;

            P->Color = bMatch ? Base : FColor(0, 0, 0, 255);
            ++Index;
        },
         false);  // bVisibleOnly
    Cloud->RefreshRendering();
}

void FJUSYNCPointCloudSpawner::InvalidateColorOriginals(ULidarPointCloud* Cloud)
{
    if (!Cloud)
    {
        return;
    }

    const TWeakObjectPtr<UObject> Key(Cloud);
    PointColorOriginals.Remove(Key);
    GradientColorOriginals.Remove(Key);
}

void FJUSYNCPointCloudSpawner::PrunePointColorOriginals()
{
    for (auto It = PointColorOriginals.CreateIterator(); It; ++It)
    {
        UObject* Cloud = It.Key().Get();
        if (!Cloud)
        {
            It.RemoveCurrent();
            continue;
        }
        if (IsPlaybackCachedCloud(Cloud))
        {
            continue;
        }
        bool bDisplayed = false;
        for (AActor* Actor : ActiveActors)
        {
            ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor);
            ULidarPointCloudComponent* Comp = LidarActor ? LidarActor->GetPointCloudComponent() : nullptr;
            if (Comp && Comp->GetPointCloud() == Cloud)
            {
                bDisplayed = true;
                break;
            }
        }
        if (!bDisplayed)
        {
            It.RemoveCurrent();
        }
    }
}

void FJUSYNCPointCloudSpawner::SetGradientColorsEnabled(bool bEnabled)
{
    bGradientColorsEnabled = bEnabled;

#ifdef WITH_ANARI_USD_MIDDLEWARE
    for (AActor* Actor : ActiveActors)
    {
        ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor);
        ULidarPointCloudComponent* Comp = LidarActor ? LidarActor->GetPointCloudComponent() : nullptr;
        ApplyGradientColorsToCloud(Comp ? Comp->GetPointCloud() : nullptr);
    }

    if (!bGradientColorsEnabled)
    {
        PruneGradientColorOriginals();
    }
#endif
}

#ifdef WITH_ANARI_USD_MIDDLEWARE
void FJUSYNCPointCloudSpawner::ApplyGradientColorsToCloud(ULidarPointCloud* Cloud)
{
    if (!IsValid(Cloud) || Cloud->HasAnyFlags(RF_BeginDestroyed))
    {
        return;
    }

    const TWeakObjectPtr<UObject> CloudKey(Cloud);
    if (bGradientColorsEnabled)
    {
        if (TArray<FColor>* Originals = GradientColorOriginals.Find(CloudKey))
        {
            const TArray<FColor>& Restore = *Originals;
            int32 Index = 0;
            Cloud->ExecuteActionOnAllPoints([&Restore, &Index](FLidarPointCloudPoint* P)
            {
                if (Restore.IsValidIndex(Index))
                {
                    P->Color = Restore[Index];
                }
                ++Index;
            },  false);  // bVisibleOnly
            GradientColorOriginals.Remove(CloudKey);
            Cloud->RefreshRendering();
        }
        return;
    }

    const int32 NumPts = static_cast<int32>(Cloud->GetNumPoints());
    TArray<FColor>& Originals = GradientColorOriginals.FindOrAdd(CloudKey);
    if (Originals.Num() != NumPts)
    {
        Originals.Init(FColor::White, NumPts);
        int32 Capture = 0;
        Cloud->ExecuteActionOnAllPoints([&Originals, &Capture](FLidarPointCloudPoint* P)
        {
            if (Originals.IsValidIndex(Capture))
            {
                Originals[Capture] = P->Color;
            }
            ++Capture;
        },  false);  // bVisibleOnly
    }

    int32 Index = 0;
    Cloud->ExecuteActionOnAllPoints([&Originals, &Index](FLidarPointCloudPoint* P)
    {
        const FColor Base = Originals.IsValidIndex(Index) ? Originals[Index] : P->Color;
        const uint8 Luma = static_cast<uint8>(FMath::Clamp(
            FMath::RoundToInt(0.299f * Base.R + 0.587f * Base.G + 0.114f * Base.B), 0, 255));
        P->Color = FColor(Luma, Luma, Luma, Base.A);
        ++Index;
    },  false);  // bVisibleOnly
    Cloud->RefreshRendering();
}

void FJUSYNCPointCloudSpawner::PruneGradientColorOriginals()
{
    for (auto It = GradientColorOriginals.CreateIterator(); It; ++It)
    {
        UObject* Cloud = It.Key().Get();
        if (!Cloud)
        {
            It.RemoveCurrent();
            continue;
        }
        if (IsPlaybackCachedCloud(Cloud))
        {
            continue;
        }
        bool bDisplayed = false;
        for (AActor* Actor : ActiveActors)
        {
            ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor);
            ULidarPointCloudComponent* Comp = LidarActor ? LidarActor->GetPointCloudComponent() : nullptr;
            if (Comp && Comp->GetPointCloud() == Cloud)
            {
                bDisplayed = true;
                break;
            }
        }
        if (!bDisplayed)
        {
            It.RemoveCurrent();
        }
    }
}
#endif

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

    const TArray64<FLidarPointCloudPoint>& NewPts = Entry.Points;

    TArray<FVector3f> WalkPos;
    TArray<FColor> WalkColor;
    WalkPos.Reserve(ExpectedPts);
    WalkColor.Reserve(ExpectedPts);
    const auto Capture = [&](FLidarPointCloudPoint* P)
    {
        WalkPos.Add(P->Location);
        WalkColor.Add(P->Color);
    };
    Cloud->ExecuteActionOnAllPoints(Capture,  false);  // bVisibleOnly

    const int32 Walked = WalkPos.Num();
    if (Walked == 0)
    {
        return false;
    }

    TArray<int32> WalkToInput;
    WalkToInput.SetNumUninitialized(Walked);
    int32 Held = 0;

    if (bReuseMap)
    {
        ParallelFor(Walked, [&](int32 i)
        {
            WalkToInput[i] = (i < KnownMap->Num()) ? (*KnownMap)[i] : INDEX_NONE;
        });
    }
    else
    {
        const int32 Pow2Exp = FMath::CeilLogTwo(FMath::Max(ExpectedPts * 2, 64));
        const int32 TableSize = 1 << Pow2Exp;
        const uint32 TableMask = static_cast<uint32>(TableSize - 1);
        const auto SlotOf = [&](int32 K) { return static_cast<uint32>((uint64)static_cast<uint32>(K) * 0x9E3779B97F4A7C15ull) >> (64 - Pow2Exp); };

        TArray<int32> Slots;
        Slots.SetNumZeroed(TableSize);
        TArray<int32> KeyOf;  // quantized key per input index
        KeyOf.SetNumUninitialized(ExpectedPts);

        ParallelFor(ExpectedPts, [&](int32 i)
        {
            const int32 K = JUSyncCloudSmooth::QKey((*Displayed)[i] + NegOffset);
            KeyOf[i] = K;
            uint32 h = SlotOf(K);
            for (;;)
            {
                const int32 Prev = FPlatformAtomics::InterlockedCompareExchange(&Slots[h], i + 1, 0);
                if (Prev == 0)
                {
                    break;  // claimed the slot
                }
                if (KeyOf[Prev - 1] == K)
                {
                    break;
                }
                h = (h + 1) & TableMask;
            }
        });

        ParallelFor(Walked, [&](int32 i)
        {
            const int32 K = JUSyncCloudSmooth::QKey(WalkPos[i]);
            uint32 h = SlotOf(K);
            int32 Src = INDEX_NONE;
            for (;;)
            {
                const int32 V = Slots[h];  // read-only after the build barrier
                if (V == 0)
                {
                    break;
                }
                if (KeyOf[V - 1] == K)
                {
                    Src = V - 1;
                    break;
                }
                h = (h + 1) & TableMask;
            }
            WalkToInput[i] = Src;
        });
    }

    Morph.Start = MoveTemp(WalkPos);
    Morph.End.SetNumUninitialized(Walked);
    Morph.NewColors.SetNumUninitialized(Walked);
    FThreadSafeCounter Unmatched(0);
    ParallelFor(Walked, [&](int32 i)
    {
        const int32 Src = WalkToInput[i];
        if (Src != INDEX_NONE && Src < NewCount)
        {
            Morph.End[i] = NewPts[Src].Location + NegOffset;
            Morph.NewColors[i] = NewPts[Src].Color;
        }
        else
        {
            Morph.End[i] = Morph.Start[i];
            Morph.NewColors[i] = WalkColor[i];
            Unmatched.Increment();
        }
    });
    Held = Unmatched.GetValue();
    WalkColor.Reset();

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
        const float S = U * U * (3.f - 2.f * U);

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
        Cloud->ExecuteActionOnAllPoints(Write,  false);  // bVisibleOnly

        if (U >= 1.f)
        {
            It.RemoveCurrent();
        }
    }
}
#endif

#ifdef WITH_ANARI_USD_MIDDLEWARE
float FJUSYNCPointCloudSpawner::ComputeSilhouetteScale(AActor* Actor) const
{
    if (CVarJusyncLockSilhouette.GetValueOnGameThread() <= 0)
    {
        return 1.0f;
    }

    ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor);
    ULidarPointCloudComponent* Comp = LidarActor ? LidarActor->GetPointCloudComponent() : nullptr;
    const ULidarPointCloud* Cloud = Comp ? Comp->GetPointCloud() : nullptr;
    if (!Cloud || SpawnScale <= KINDA_SMALL_NUMBER)
    {
        return 1.0f;
    }

    const FBox LocalBounds = Cloud->GetBounds();
    if (!LocalBounds.IsValid)
    {
        return 1.0f;
    }

    const FVector Size = LocalBounds.GetSize();
    float DataExtent = TNumericLimits<float>::Max();
    if (Size.X > KINDA_SMALL_NUMBER) DataExtent = FMath::Min(DataExtent, Size.X);
    if (Size.Y > KINDA_SMALL_NUMBER) DataExtent = FMath::Min(DataExtent, Size.Y);
    if (Size.Z > KINDA_SMALL_NUMBER) DataExtent = FMath::Min(DataExtent, Size.Z);
    if (DataExtent <= KINDA_SMALL_NUMBER || DataExtent == TNumericLimits<float>::Max())
    {
        return 1.0f;
    }

    if (SilhouetteReferencePointSize < 0.0f)
    {
        SilhouetteReferencePointSize = FMath::Max(PointSize, 0.001f);
    }

    float ViewDepth = 1000.0f;
    if (UWorld* World = Comp->GetWorld())
    {
        if (APlayerController* PC = World->GetFirstPlayerController())
        {
            if (APlayerCameraManager* Cam = PC->PlayerCameraManager)
            {
                const FVector CloudCenter =
                    Comp->GetComponentTransform().TransformPosition(LocalBounds.GetCenter());
                const FVector CamLoc = Cam->GetCameraLocation();
                const FVector CamFwd = Cam->GetCameraRotation().Vector();
                ViewDepth = FMath::Max(1.0f, FVector::DotProduct(CloudCenter - CamLoc, CamFwd));
            }
        }
    }

    const float DataScreen = SpawnScale * DataExtent / ViewDepth;
    const float SpriteScreen = 0.01f * FMath::Max(0.0f, PointSize);
    const float ReferenceSprite = 0.01f * SilhouetteReferencePointSize;
    const float DesiredData = DataScreen + ReferenceSprite - SpriteScreen;
    return FMath::Clamp(DesiredData / FMath::Max(DataScreen, 1e-4f), 0.05f, 2.0f);
}

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
    }

    float EffectiveScale = SpawnScale;
    if (AActor* OwningActor = Comp->GetOwner())
    {
        const float SilhouetteScale = ComputeSilhouetteScale(OwningActor);
        EffectiveScale = SpawnScale * SilhouetteScale;
        const ULidarPointCloud* Cloud = Comp->GetPointCloud();
        const FVector LocalCenter = Cloud ? Cloud->GetBounds().GetCenter() : FVector::ZeroVector;
        const FVector RotatedLocalCenter = OwningActor->GetActorQuat().RotateVector(LocalCenter);
        OwningActor->SetActorScale3D(FVector(EffectiveScale));
        OwningActor->SetActorLocation(SpawnLocation + RotatedLocalCenter * (SpawnScale - EffectiveScale));
    }
    if (EffectiveScale > KINDA_SMALL_NUMBER)
    {
        Comp->PointSize /= EffectiveScale;
    }

    if (Comp->GetPointShape() != PointShape)
    {
        Comp->SetPointShape(PointShape);
    }

    static FString CachedCloudMatPath;
    static UMaterialInterface* CachedCloudMat = nullptr;
    const FString CloudMatPath = CVarJusyncCloudMaterial.GetValueOnGameThread();
    if (CloudMatPath != CachedCloudMatPath)
    {
        CachedCloudMatPath = CloudMatPath;
        CachedCloudMat = CloudMatPath.IsEmpty()
                             ? nullptr
                             : LoadObject<UMaterialInterface>(nullptr, *CloudMatPath);
        if (!CachedCloudMat && !CloudMatPath.IsEmpty())
        {
            UE_LOG(LogTemp, Warning,
                   TEXT("JUSYNC Spawner: cloud material '%s' not found - run Tools/CreateJusyncCloudMaterial.py;"
                        " keeping the plugin default (emissive) material"),
                   *CloudMatPath);
        }
    }
    if (CachedCloudMat && Comp->GetMaterial(0) != CachedCloudMat)
    {
        Comp->SetMaterial(0, CachedCloudMat);
    }

    if (UMaterialInstanceDynamic* CloudDyn = Cast<UMaterialInstanceDynamic>(Comp->GetMaterial(0)))
    {
        float Current = -1.f;
        const float EmissiveFloor = CVarJusyncCloudEmissiveFloor.GetValueOnGameThread();
        if (!CloudDyn->GetScalarParameterValue(FName("EmissiveFloor"), Current) ||
            !FMath::IsNearlyEqual(Current, EmissiveFloor))
        {
            CloudDyn->SetScalarParameterValue(FName("EmissiveFloor"), EmissiveFloor);
        }

        const float ShapeCircle = (PointShape == ELidarPointCloudSpriteShape::Circle) ? 1.0f : 0.0f;
        float CurrentShapeCircle = -1.0f;
        if (!CloudDyn->GetScalarParameterValue(FName("PointShapeCircle"), CurrentShapeCircle) ||
            !FMath::IsNearlyEqual(CurrentShapeCircle, ShapeCircle))
        {
            CloudDyn->SetScalarParameterValue(FName("PointShapeCircle"), ShapeCircle);
        }
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

void FJUSYNCPointCloudSpawner::EnsureCloudEmissiveFloor()
{
    const float Floor = CVarJusyncCloudEmissiveFloor.GetValueOnGameThread();
    for (AActor* Actor : ActiveActors)
    {
        ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor);
        ULidarPointCloudComponent* Comp = LidarActor ? LidarActor->GetPointCloudComponent() : nullptr;
        if (!Comp)
        {
            continue;
        }
        if (UMaterialInstanceDynamic* Dyn = Cast<UMaterialInstanceDynamic>(Comp->GetMaterial(0)))
        {
            float Current = -1.f;
            if (!Dyn->GetScalarParameterValue(FName("EmissiveFloor"), Current) ||
                !FMath::IsNearlyEqual(Current, Floor))
            {
                Dyn->SetScalarParameterValue(FName("EmissiveFloor"), Floor);
            }
        }
    }
}

void FJUSYNCPointCloudSpawner::ApplyNormalCalculationToActiveActors()
{
    if (!bCalculateNormals)
    {
        return;
    }
    for (AActor* Actor : ActiveActors)
    {
        ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor);
        ULidarPointCloudComponent* Comp = LidarActor ? LidarActor->GetPointCloudComponent() : nullptr;
        ULidarPointCloud* Cloud = Comp ? Comp->GetPointCloud() : nullptr;
        if (!Cloud)
        {
            continue;
        }

        LastNormalCalcTime.Remove(Actor);
        MaybeCalculateNormals(Cloud, Actor, static_cast<int32>(Cloud->GetNumPoints()));
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

void FJUSYNCPointCloudSpawner::ReapplyTransformToAllActors()
{
    for (AActor* Actor : ActiveActors)
    {
        if (!Actor || !Actor->IsValidLowLevel())
        {
            continue;
        }
        ApplyTransformToActor(Actor);
        ApplyVisualSettingsToActor(Actor);
    }
}

void FJUSYNCPointCloudSpawner::ApplyTransformToActor(AActor* Actor)
{
    if (!Actor || !Actor->IsValidLowLevel())
    {
        return;
    }

    const float SilhouetteScale = ComputeSilhouetteScale(Actor);
    const float EffectiveScale = SpawnScale * SilhouetteScale;
    FVector LocalCenter = FVector::ZeroVector;
    if (ALidarPointCloudActor* LidarActor = Cast<ALidarPointCloudActor>(Actor))
    {
        if (ULidarPointCloudComponent* Comp = LidarActor->GetPointCloudComponent())
        {
            if (const ULidarPointCloud* Cloud = Comp->GetPointCloud())
            {
                LocalCenter = Cloud->GetBounds().GetCenter();
            }
        }
    }
    const FVector RotatedLocalCenter = Actor->GetActorQuat().RotateVector(LocalCenter);
    Actor->SetActorScale3D(FVector(EffectiveScale));
    Actor->SetActorLocation(SpawnLocation + RotatedLocalCenter * (SpawnScale - EffectiveScale));
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
    const float CloudFloorNow = CVarJusyncCloudEmissiveFloor.GetValueOnGameThread();
    if (FixedNow != LastFixedSizeState || ScreenNow != LastFixedScreenSize
        || !FMath::IsNearlyEqual(CloudFloorNow, LastCloudEmissiveFloor))
    {
        LastFixedSizeState = FixedNow;
        LastFixedScreenSize = ScreenNow;
        LastCloudEmissiveFloor = CloudFloorNow;
        ApplySizingToActiveActors();
    }

    EnsureCloudEmissiveFloor();
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

    {
        TArray<FPointCloudReadyEntry> Pending = MoveTemp(ReadyQueue);
        ReadyQueue.Reset();

        TMap<FString, int32> IndexByKey;
        TArray<FPointCloudReadyEntry> Keep;
        IndexByKey.Reserve(Pending.Num());
        for (FPointCloudReadyEntry& E : Pending)
        {
            if (E.bSpawned)
            {
                continue;
            }
            if (E.Key.IsEmpty())
            {
                E.Key = JUSYNCElementNames::MakeRankedElementKey(E.ElementName, E.Rank);
            }
            int32* ExistingIdx = IndexByKey.Find(E.Key);
            if (!ExistingIdx)
            {
                IndexByKey.Add(E.Key, Keep.Num());
                Keep.Add(MoveTemp(E));
            }
            else if (E.Points.Num() >= Keep[*ExistingIdx].Points.Num())
            {
                Keep[*ExistingIdx] = MoveTemp(E);
            }
        }
        ReadyQueue = MoveTemp(Keep);
    }

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
            OnPointCloudSpawned.Broadcast(Entry.Key, nullptr);
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
        const bool bPlaybackMorphNow = bPlaybackMode && CVarJusyncCloudBlendMs.GetValueOnGameThread() > 0;
        const bool bAllowCloudMorph = !bPlaybackMode || bPlaybackMorphNow;
        if (CVarJusyncAsyncCloudBuild.GetValueOnGameThread() > 0
            && Entry.Points.Num() > 0 && !Entry.bNeedsRecolor && bAllowCloudMorph)
        {
            if (!bInPlaceUpdate)
            {
                ElementToActor.Add(Entry.Key, Actor);
                ActorToElement.Add(Actor, Entry.Key);
            }
            Actor->SetActorEnableCollision(false);

            const double NowSeconds = FPlatformTime::Seconds();
            const FString Key = Entry.Key;

            SeedCloudDisplayPos(Key, Actor);
            const bool bWantMorph = bPlaybackMorphNow
                ? CloudDisplayPos.Contains(Key)
                : ShouldStartCloudRamp(Key, NowSeconds);
            if (bAllowCloudMorph && bInPlaceUpdate && bWantMorph)
            {
                AbortDeferredSwap(Key);
                RampChains.Remove(Key);

                const int32 ForcedMs = CVarJusyncCloudBlendMs.GetValueOnGameThread();
                const double MaxBlend = FMath::Max(CVarJusyncCloudMaxBlendMs.GetValueOnGameThread(), 8) / 1000.0;
                const bool bPlaybackMorph = bPlaybackMorphNow;
                const double ArrivalGap = NowSeconds - CloudLastArrival.FindRef(Key);
                const double LiveBlend = FMath::Clamp(ArrivalGap * 0.9, 0.12, FMath::Max(0.45, MaxBlend));
                const double BlendSeconds = bPlaybackMorph
                    ? FMath::Max(ForcedMs, 8) / 1000.0
                    : (ForcedMs > 0
                        ? ForcedMs / 1000.0
                        : LiveBlend);
                CloudLastArrival.Add(Key, NowSeconds);

                const bool bSmoothStarted = CVarJusyncCloudSmooth.GetValueOnGameThread() > 0
                    && StartCloudSmoothMorph(Key, Entry, Actor, NowSeconds, BlendSeconds);
                if (bSmoothStarted)
                {
                    if (bPlaybackMorph)
                    {
                        UE_LOG(LogTemp, Display,
                            TEXT("[Playback] morph '%s' %d pts over %.0fms"),
                            *Key, Entry.Points.Num(), BlendSeconds * 1000.0);
                    }
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

                    KickRampStep(Key, Chain);  // overlaps step-1 build with the hold window
                }
            }
            else
            {
                CloudLastArrival.Add(Key, NowSeconds);
                RampChains.Remove(Key);
                if (bPlaybackMorphNow)
                {
                    UE_LOG(LogTemp, Warning,
                        TEXT("[Playback] morph skipped '%s': inPlace=%d allowMorph=%d anchor=%d pts=%d"),
                        *Key, bInPlaceUpdate ? 1 : 0, bAllowCloudMorph ? 1 : 0,
                        CloudDisplayPos.FindRef(Key).Num(), Entry.Points.Num());
                }
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
                    InvalidateColorOriginals(LidarCloud);
                    ApplyGradientColorsToCloud(LidarCloud);
                    ApplyPointHueFilterToCloud(LidarCloud);
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

        Actor->SetActorHiddenInGame(bPointCloudsHidden);
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
    GradientColorOriginals.Empty();

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
            InvalidateColorOriginals(Cloud);
            ApplyGradientColorsToCloud(Cloud);
            ApplyPointHueFilterToCloud(Cloud);
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
