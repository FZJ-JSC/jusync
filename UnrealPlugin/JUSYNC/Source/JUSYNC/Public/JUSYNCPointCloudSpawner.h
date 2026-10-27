#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "JUSYNCTypes.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include <atomic>
#include <memory>

#ifdef WITH_ANARI_USD_MIDDLEWARE
#include "LidarPointCloud.h"
#include "LidarPointCloudComponent.h"
#endif

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPointCloudSpawned, const FString&, AActor*);

struct FPointCloudReadyEntry
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    TArray64<FLidarPointCloudPoint> Points;
#endif
    FString ElementName;
    FString Key;
    int32 Rank;
    float PointSize;
    bool bSpawned;
    bool bNeedsRecolor;
    // Cached-playback selection: keep the live smooth-morph path instead of
    // snapping to the selected cloud, so clicking through samples animates.
    bool bPlaybackMorph;
    // Identity of the sample-cache payload this entry was converted from
    // (playback only; null for live). Once the built cloud swaps in, the
    // spawner remembers payload -> cloud so revisiting the sample displays
    // instantly instead of rebuilding it (log: "CloudReplay: instant revisit").
    FJUSYNCPointCloudRef SourcePayload;

    // Broker file this payload was fetched from plus its content hash, set on
    // the live-stream enqueue. The spawner's V2 dedup consults AppliedFileHashes
    // so a notification is only dropped when this exact content already built
    // a cloud (hash-tracked-but-never-applied would otherwise freeze updates).
    FString SourceFile;
    uint64 SourceHashLo = 0;
    uint64 SourceHashHi = 0;

    // Set once ApplyPointCloudBuild finishes spawning/swapping the cloud for
    // this entry; only then is the entry's SourceFile/Hash pair trustworthy.
    bool bCloudApplied = false;

    TArray<FVector> Positions;
    TArray<float> Widths;

    FPointCloudReadyEntry()
        : Rank(0), PointSize(1.0f), bSpawned(false), bNeedsRecolor(false), bPlaybackMorph(false)
    {}
};

class JUSYNC_API FJUSYNCPointCloudSpawner : public FTickableGameObject
{
public:
    FJUSYNCPointCloudSpawner(TWeakObjectPtr<UObject> InOwner);
    virtual ~FJUSYNCPointCloudSpawner();

    bool EnqueuePointCloud(const FJUSYNCPointCloudData& PCData, int32 InRank = 0);
    bool EnqueuePointCloud(FJUSYNCPointCloudData&& PCData, int32 InRank = 0, bool bAllowNoGradient = false);
    // Shared overload: avoids an extra copy when the caller owns the payload exclusively.

    bool EnqueuePointCloud(FJUSYNCPointCloudRef&& PCRef, int32 InRank = 0);
    // Synchronous cached-playback path. Converts and swaps on the game thread so
    // the selected frame is visible immediately instead of waiting for a
    // background conversion/build and a later tick.
    bool EnqueuePointCloudForPlayback(FJUSYNCPointCloudRef&& PCRef, int32 InRank = 0);

    bool IsElementFiltered(const FString& ElementName) const;

    /** True only if this exact (file, hash) pair was actually swapped into a cloud.
     *  Distinct from FileHashLo/Hi on the file spawner, which only record what the
     *  broker told us: a hash can be tracked there without ever being applied. */
    bool HasAppliedFileHash(const FString& Filename, uint64 HashLo, uint64 HashHi) const;

    // Cached-sample scrubbing wants the selected frame on screen now, not a
    // 200 ms morph from the live frame. Playback mode also lets the spawner
    // discard live builds that were already in flight when the user scrubbed.
    void SetPlaybackMode(bool bEnabled) { bPlaybackMode = bEnabled; }
    void EnterPlaybackMode();
    void PreparePointCloudForPlayback(const FString& Key);

    virtual void Tick(float DeltaTime) override;
    virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
    virtual bool IsTickable() const override { return Owner.IsValid() && !bShuttingDown.load(); }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FJUSYNCPointCloudSpawner, STATGROUP_Tickables); }

    void SetMaxPoolSize(int32 InMax) { MaxPoolSize = InMax; }
    void SetBudgetMs(float InBudget) { BudgetMs = InBudget; }

    // Byte budget for the warm replay-cloud pool (dynamic cache budget
    // feature; the spawner drives this from a DXGI process video-memory
    // budget poll). 0 = legacy fixed pool of MaxPlaybackCloudReuse clouds.
    void SetWarmCloudBudgetBytes(int64 InBytes);

    // Pool size implied by the byte budget: clamp(budget/avg cloud bytes,
    // MinWarmClouds, MaxWarmCloudsHardCap); legacy constant when either
    // input is 0. Pure so tests can pin it.
    static int32 WarmCloudCountForBudget(int64 BudgetBytes, int64 AvgCloudBytes);
    void SetSpawnLocation(const FVector& In);
    void SetSpawnScale(float In) { SpawnScale = In; }
    const FVector& GetSpawnLocation() const { return SpawnLocation; }
    float GetSpawnScale() const { return SpawnScale; }

    /** Re-applies the current SpawnLocation/SpawnScale to every active point
     *  cloud actor. Used when the FitVolume transform changes after actors have
     *  already been spawned (e.g. once the full-scene bounds are known). */
    void ReapplyTransformToAllActors();

    void SetPointSize(float InSize) { PointSize = InSize; }
    void SetPointSizeBias(float InBias) { PointSizeBias = InBias; }
    void SetGapFillingStrength(float InStrength) { GapFillingStrength = InStrength; }
    void SetPerfLogging(bool InEnabled) { bPerfLogging = InEnabled; }
    void SetNormalCalculation(bool bEnabled, int32 InMaxPoints, int32 InQuality, float InNoiseTolerance, float InCooldownSeconds)
    {
        bCalculateNormals = bEnabled;
        MaxPointsForNormals = FMath::Max(0, InMaxPoints);
        NormalsQuality = FMath::Clamp(InQuality, 1, 100);
        NormalsNoiseTolerance = FMath::Max(0.0f, InNoiseTolerance);
        NormalsCooldownSeconds = FMath::Max(0.0f, InCooldownSeconds);
    }
#ifdef WITH_ANARI_USD_MIDDLEWARE
    void SetPointShape(ELidarPointCloudSpriteShape InShape) { PointShape = InShape; }
    void SetPointOrientation(ELidarPointCloudSpriteOrientation InOrientation) { PointOrientation = InOrientation; }
    void SetPointScaling(ELidarPointCloudScalingMethod InScaling) { PointScaling = InScaling; }
    void ApplyVisualSettingsToComponent(ULidarPointCloudComponent* Comp) { ApplyVisualSettings(Comp); }

    void ApplyPointShapeToActiveActors(ELidarPointCloudSpriteShape InShape);

    void ApplySizingToActiveActors();
    void RequestNormalCalculation(ULidarPointCloud* Cloud, AActor* Actor, int32 PointCount) { MaybeCalculateNormals(Cloud, Actor, PointCount); }
#endif

    void ClearAllActors();
    void WaitForCompletion(float TimeoutSeconds = 5.0f);

    /** Drops every cloud recorded for instant sample revisits. The file
     *  spawner calls this when it clears its sample cache so built clouds
     *  cannot outlive the samples they belong to. */
    void ClearPlaybackCloudReuse();

    /** POINTS panel: hide/show every LiDAR cloud. Clouds swapped in later
     *  (live frames, replays) inherit the current state. */
    void SetPointCloudsHidden(bool bHidden) { bPointCloudsHidden = bHidden; ApplyPointCloudsHiddenToActors(); }
    bool ArePointCloudsHidden() const { return bPointCloudsHidden; }

    /** POINTS panel: hue filter over each point's baked colour. Non-matching
     *  points are dimmed to near-black in place; originals are restored when
     *  the filter is cleared. Re-applied automatically on every cloud swap. */
    void SetPointHueFilter(bool bEnabled, float HueDeg, float ToleranceDeg);

    void DestroyTrackedActor(AActor* Actor);

    void DestroyAllActors();

    void SetGradientLUT(const TArray<FColor>& InLUT);
    TArray<FColor> GetGradientLUT() const { FScopeLock Lock(&GradientMutex); return GradientLUT; }
    uint64 GetLUTVersion() const { FScopeLock Lock(&GradientMutex); return LUTVersion; }

    void RecolorGradientPendingActors();

    int32 GetGradientPendingCount() const { return GradientPendingData.Num(); }

    int32 GetReadyQueueCount() const { return ReadyQueue.Num(); }
    int32 GetActiveActorCount() const { return ActiveActors.Num(); }

    AActor* FindActorByElementKey(const FString& ElementKey) const;
    FString GetElementKeyForActor(AActor* Actor) const;

    void DrainReadyQueue();
    void DrainReadyQueueLocked();

    int32 GetInFlightBuildCount() const { return InFlightBuilds.Num(); }

    FOnPointCloudSpawned OnPointCloudSpawned;

private:
    TWeakObjectPtr<UObject> Owner;
    TArray<FPointCloudReadyEntry> ReadyQueue;
    FCriticalSection QueueMutex;
    mutable FCriticalSection GradientMutex;
    TArray<AActor*> AvailablePool;
    TSet<AActor*> ActiveActors;
    int32 MaxPoolSize;
    FVector SpawnLocation;
    float SpawnScale;
    float BudgetMs;
    float PointSize = 1.0f;
    float PointSizeBias = 0.035f;
    float GapFillingStrength = 0.0f;
    bool bPerfLogging = false;
    bool bPlaybackMode = false;
    // POINTS panel state (see SetPointCloudsHidden / SetPointHueFilter).
    bool bPointCloudsHidden = false;
    bool bPointHueFilter = false;
    float PointHue = 0.0f;
    float PointHueTolerance = 30.0f;
    void ApplyPointCloudsHiddenToActors();
#ifdef WITH_ANARI_USD_MIDDLEWARE
    /** In-place colour pass for one cloud: captures originals on first use,
     *  then shows them (filter match) or a dim grey (no match). Also used to
     *  restore by SetPointHueFilter(false,...) and after every swap. */
    void ApplyPointHueFilterToCloud(ULidarPointCloud* Cloud);
    void PrunePointColorOriginals();
    /** cloud -> untouched build-time colours; only populated while filtering. */
    TMap<TWeakObjectPtr<UObject>, TArray<FColor>> PointColorOriginals;
#endif
#ifdef WITH_ANARI_USD_MIDDLEWARE
    ELidarPointCloudSpriteShape PointShape = ELidarPointCloudSpriteShape::Circle;
    ELidarPointCloudSpriteOrientation PointOrientation = ELidarPointCloudSpriteOrientation::PreferFacingCamera;
    ELidarPointCloudScalingMethod PointScaling = ELidarPointCloudScalingMethod::PerNodeAdaptive;
    bool bCalculateNormals = false;
    int32 MaxPointsForNormals = 1000000;
    int32 NormalsQuality = 10;
    float NormalsNoiseTolerance = 0.05f;
    float NormalsCooldownSeconds = 2.0f;
    TMap<AActor*, double> LastNormalCalcTime;
    void MaybeCalculateNormals(ULidarPointCloud* Cloud, AActor* Actor, int32 PointCount);
    int32 LastFixedSizeState = -999999;
    float LastFixedScreenSize = -999999.0f;
    float LastCloudEmissiveFloor = -999999.0f;
#endif
    std::atomic<bool> bShuttingDown{false};
    std::atomic<int32> InFlightTasks{0};
    std::shared_ptr<std::atomic<int32>> AsyncBuildsOutstanding = std::make_shared<std::atomic<int32>>(0);
    std::shared_ptr<std::atomic<bool>> AsyncBuildsAlive = std::make_shared<std::atomic<bool>>(true);

    bool BeginTask();
    void EndTask();

    TArray<FColor> GradientLUT;
    uint64 LUTVersion = 0;
    TSet<AActor*> GradientPendingActors;
    struct FRecolorData { TArray<FVector> Positions; TArray<float> Widths; };
    TMap<AActor*, FRecolorData> GradientPendingData;

    TMap<FString, AActor*> ElementToActor;
    TMap<AActor*, FString> ActorToElement;

    /** (hashLo, hashHi) last actually applied per source broker file. Empty until
     *  the enqueue path starts carrying SourceFile/SourceHashLo/HashHi: queries then
     *  conservatively report "not applied" so a refresh is always queued. */
    TMap<FString, TPair<uint64, uint64>> AppliedFileHashes;

#ifdef WITH_ANARI_USD_MIDDLEWARE
    struct FInFlightCloudBuild
    {
        // Heap points read by the Epic async build thread; freed on completion.

        TArray64<FLidarPointCloudPoint>* Points = nullptr;
        TUniquePtr<TStrongObjectPtr<UObject>> CloudRef;
        TWeakObjectPtr<AActor> Actor;
        FPointCloudReadyEntry PendingReplacement;
        // Playback reuse: the sample-cache payload this build was started
        // for (null for live builds). Stored into the reuse cache on swap.
        FJUSYNCPointCloudRef SourcePayload;
        double BuildStarted = 0.0;
        bool bEverSwapped = false;
        bool bHasPending = false;
        bool bBuilding = false;
        uint64 Generation = 0;

        double EarliestApply = 0.0;
        bool bDeferred = false;
        bool bChainStep = false;
        bool bChainFinal = false;
        bool bChainAborted = false;
        TArray<FVector3f> RampPos;

        ~FInFlightCloudBuild() { delete Points; Points = nullptr; }
    };
    TMap<FString, TSharedRef<FInFlightCloudBuild>> InFlightBuilds;
    uint64 NextCloudBuildGeneration = 0;

    struct FCloudRampChain
    {
        FPointCloudReadyEntry Entry;     // the real sample, held for the final step
        TArray<FVector3f> PrevPos;       // displayed positions at chain start (input order)
        AActor* Actor = nullptr;
        double T0 = 0.0;
        double Duration = 0.0;
        int32 Step = 0;                  // steps KICKED so far (swap timing handled by build deferral)
        int32 TotalSteps = 2;            // interpolated steps + 1 final
        bool bFinalKicked = false;
    };
    TMap<FString, FCloudRampChain> RampChains;

    TMap<FString, TArray<FVector3f>> CloudDisplayPos;
    TMap<FString, double> CloudLastArrival;

    struct FCloudSmoothMorph
    {
        TArray<FVector3f> Start;
        TArray<FVector3f> End;
        // Incoming sample's per-point colors, applied every frame so colors stay realtime
        // while positions glide.
        TArray<FColor> NewColors;
        TWeakObjectPtr<AActor> Actor;
        double T0 = 0.0;
        double Duration = 0.0;
    };
    TMap<FString, FCloudSmoothMorph> SmoothMorphs;

    TMap<FString, TArray<int32>> CloudWalkInputIdx;

    bool StartCloudSmoothMorph(const FString& Key, FPointCloudReadyEntry& Entry, AActor* Actor,
                               double T0, double Duration);
    void AdvanceCloudSmoothMorphs(double NowSeconds);

    void StartAsyncCloudBuild(const FString& Key, FPointCloudReadyEntry&& Entry, AActor* Actor,
                              double EarliestApply = 0.0, bool bChainStep = false, bool bChainFinal = false);
    void OnAsyncCloudBuildFinished(const FString& Key, uint64 Generation, bool bSuccess);

    // Ramp helpers (game-thread only)

    bool ShouldStartCloudRamp(const FString& Key, double NowSeconds) const;
    void KickRampStep(const FString& Key, FCloudRampChain& Chain);
    void AbortDeferredSwap(const FString& Key);
    void TickCloudRamps(double NowSeconds);

    // Instant revisits during cached scrubbing (game-thread only). Keyed by
    // the sample-cache payload's identity: ApplyCachedSample hands the same
    // shared payload back for the same sample, so pointer equality is the
    // sample identity. Bounded LRU of built clouds; a cloud only stays in
    // the cache while GC would otherwise drop it, and the frame currently
    // on screen is never evicted.
    struct FPlaybackCloudReuse
    {
        double LastUsedSeconds = 0.0;
        FString Key;
        FJUSYNCPointCloudRef Payload;
        TStrongObjectPtr<UObject> Cloud;
    };
    TArray<FPlaybackCloudReuse> PlaybackCloudReuse;
    static constexpr int32 MaxPlaybackCloudReuse = 24; // legacy pool size
    int64 WarmCloudBudgetBytes = 0; // 0 = legacy fixed-size pool
    int64 WarmCloudAvgBytes = 0;    // EMA of stored payload sizes
    static constexpr int32 MinWarmClouds = 8;
    static constexpr int32 MaxWarmCloudsHardCap = 128;
    int32 FindEvictableReuseIndex();
    static int64 EstimatePayloadBytes(const FJUSYNCPointCloudRef& Payload);

    int32 FindPlaybackReuseIndex(const FJUSYNCPointCloudRef& Payload) const;
    bool TryDisplayCachedPlaybackCloud(const FJUSYNCPointCloudRef& Payload, const FString& Key);
    void StorePlaybackCloud(const FString& Key, FJUSYNCPointCloudRef&& Payload, UObject* Cloud);
    bool IsPlaybackCachedCloud(const UObject* Cloud) const;
#endif

    struct FPendingNoColorCloud
    {
        FJUSYNCPointCloudData Data;
        int32 Rank = 0;
        double QueuedTime = 0.0;
    };
    TArray<FPendingNoColorCloud> PendingNoColorClouds;

    void UntrackActorElement(AActor* Actor);

    void ApplyTransformToActor(AActor* Actor);

    AActor* AllocateActor();
    void ReleaseActor(AActor*);
    void FlushExpiredNoColorClouds();
#ifdef WITH_ANARI_USD_MIDDLEWARE
    ULidarPointCloud* GetOrCreateCloudForComponent(ULidarPointCloudComponent* Comp);
    void ApplyVisualSettings(ULidarPointCloudComponent* Comp);
    void ApplyVisualSettingsToActor(AActor* Actor);
    /** Re-asserts jusync.Spawner.CloudEmissiveFloor on every live cloud MID.
     *  Scrubbing swaps in clouds whose MID is rebuilt from the material default,
     *  so a one-shot push on CVar change loses the panel value. */
    void EnsureCloudEmissiveFloor();
#endif
};
