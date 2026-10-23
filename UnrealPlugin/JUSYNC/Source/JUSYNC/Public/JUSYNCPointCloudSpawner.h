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

#ifdef WITH_ANARI_USD_MIDDLEWARE
    struct FInFlightCloudBuild
    {
        // Heap points read by the Epic async build thread; freed on completion.

        TArray64<FLidarPointCloudPoint>* Points = nullptr;
        TUniquePtr<TStrongObjectPtr<UObject>> CloudRef;
        TWeakObjectPtr<AActor> Actor;
        FPointCloudReadyEntry PendingReplacement;
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
# endif

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
#endif
};
