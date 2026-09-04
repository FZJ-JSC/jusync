#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "JUSYNCTypes.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include <atomic>

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

    // Kept for gradient recoloring when LUT arrives after white fallback
    TArray<FVector> Positions;
    TArray<float> Widths;

    FPointCloudReadyEntry()
        : Rank(0), PointSize(1.0f), bSpawned(false), bNeedsRecolor(false)
    {}
};

class JUSYNC_API FJUSYNCPointCloudSpawner : public FTickableGameObject
{
public:
    FJUSYNCPointCloudSpawner(TWeakObjectPtr<UObject> InOwner);
    virtual ~FJUSYNCPointCloudSpawner();

    void EnqueuePointCloud(const FJUSYNCPointCloudData& PCData, int32 InRank = 0);
    // Rvalue overload: moves the point cloud into the async task (no copy).
    void EnqueuePointCloud(FJUSYNCPointCloudData&& PCData, int32 InRank = 0);
    // Shared overload: avoids an extra copy when the caller owns the payload exclusively.
    void EnqueuePointCloud(FJUSYNCPointCloudRef&& PCRef, int32 InRank = 0);
    virtual void Tick(float DeltaTime) override;
    virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
    virtual bool IsTickable() const override { return Owner.IsValid() && !bShuttingDown.load(); }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FJUSYNCPointCloudSpawner, STATGROUP_Tickables); }

    void SetMaxPoolSize(int32 InMax) { MaxPoolSize = InMax; }
    void SetBudgetMs(float InBudget) { BudgetMs = InBudget; }
    void SetSpawnLocation(const FVector& In);
    void SetSpawnScale(float In) { SpawnScale = In; }

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
    void RequestNormalCalculation(ULidarPointCloud* Cloud, AActor* Actor, int32 PointCount) { MaybeCalculateNormals(Cloud, Actor, PointCount); }
#endif

    void ClearAllActors();
    void WaitForCompletion(float TimeoutSeconds = 5.0f);

    /** Destroy a tracked actor and remove it from all internal bookkeeping. */
    void DestroyTrackedActor(AActor* Actor);

    /** Destroy ALL actors including pooled ones (for full scene cleanup) */
    void DestroyAllActors();

    /** Set a 256-entry gradient color LUT from a decoded PNG row. Attribute0 values map to these colors. */
    void SetGradientLUT(const TArray<FColor>& InLUT);
    TArray<FColor> GetGradientLUT() const { FScopeLock Lock(&GradientMutex); return GradientLUT; }
    uint64 GetLUTVersion() const { FScopeLock Lock(&GradientMutex); return LUTVersion; }

    /** Recolor all actors spawned with white fallback (no gradient at spawn time). Call after SetGradientLUT. */
    void RecolorGradientPendingActors();

    int32 GetGradientPendingCount() const { return GradientPendingData.Num(); }

    int32 GetReadyQueueCount() const { return ReadyQueue.Num(); }
    int32 GetActiveActorCount() const { return ActiveActors.Num(); }

    AActor* FindActorByElementKey(const FString& ElementKey) const;
    FString GetElementKeyForActor(AActor* Actor) const;

    void DrainReadyQueue();
    void DrainReadyQueueLocked();

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
#endif
    std::atomic<bool> bShuttingDown{false};
    std::atomic<int32> InFlightTasks{0};

    bool BeginTask();
    void EndTask();

    TArray<FColor> GradientLUT;
    uint64 LUTVersion = 0;
    TSet<AActor*> GradientPendingActors;
    struct FRecolorData { TArray<FVector> Positions; TArray<float> Widths; };
    TMap<AActor*, FRecolorData> GradientPendingData;

    TMap<FString, AActor*> ElementToActor;
    TMap<AActor*, FString> ActorToElement;

    struct FPendingNoColorCloud
    {
        FJUSYNCPointCloudData Data;
        int32 Rank = 0;
        double QueuedTime = 0.0;
    };
    TArray<FPendingNoColorCloud> PendingNoColorClouds;

    void UntrackActorElement(AActor* Actor);

    AActor* AllocateActor();
    void ReleaseActor(AActor*);
    void FlushExpiredNoColorClouds();
#ifdef WITH_ANARI_USD_MIDDLEWARE
    ULidarPointCloud* GetOrCreateCloudForComponent(ULidarPointCloudComponent* Comp);
    void ApplyVisualSettings(ULidarPointCloudComponent* Comp);
    void ApplyVisualSettingsToActor(AActor* Actor);
#endif
};
