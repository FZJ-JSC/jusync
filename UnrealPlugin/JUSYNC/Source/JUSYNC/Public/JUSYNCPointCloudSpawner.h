#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "JUSYNCTypes.h"

#ifdef WITH_ANARI_USD_MIDDLEWARE
#include "LidarPointCloud.h"
#endif

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPointCloudSpawned, const FString&, AActor*);

struct FPointCloudReadyEntry
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    TArray64<FLidarPointCloudPoint> Points;
#endif
    FString ElementName;
    int32 Rank;
    float PointSize;
    bool bSpawned;

    FPointCloudReadyEntry()
        : Rank(0), PointSize(1.0f), bSpawned(false)
    {}
};

class JUSYNC_API FJUSYNCPointCloudSpawner : public FTickableGameObject
{
public:
    virtual ~FJUSYNCPointCloudSpawner() {}

    void EnqueuePointCloud(const FJUSYNCPointCloudData& PCData, int32 InRank = 0);
    virtual void Tick(float DeltaTime) override;
    virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
    virtual bool IsTickable() const override { return Owner.IsValid(); }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FJUSYNCPointCloudSpawner, STATGROUP_Tickables); }

    void SetMaxPoolSize(int32 InMax) { MaxPoolSize = InMax; }
    void SetBudgetMs(float InBudget) { BudgetMs = InBudget; }
    void SetSpawnLocation(const FVector& In);
    void SetSpawnScale(float In) { SpawnScale = In; }
    void ClearAllActors();

    /** Set a 256-entry gradient color LUT from a decoded PNG row. Attribute0 values map to these colors. */
    void SetGradientLUT(const TArray<FColor>& InLUT) { FScopeLock Lock(&GradientMutex); GradientLUT = InLUT; }
    TArray<FColor> GetGradientLUT() const { FScopeLock Lock(&GradientMutex); return GradientLUT; }

    int32 GetReadyQueueCount() const { return ReadyQueue.Num(); }
    int32 GetActiveActorCount() const { return ActiveActors.Num(); }

    void DrainReadyQueue();

    FOnPointCloudSpawned OnPointCloudSpawned;

    explicit FJUSYNCPointCloudSpawner(TWeakObjectPtr<UObject> InOwner);

private:
    struct FConversionEntry
    {
        TArray<FVector> Positions;
        TArray<FColor> Colors;
        TArray<float> Widths;
        FString ElementName;
        int32 PointCount;
        bool bHasColors;
        int32 Rank;
    };

    TWeakObjectPtr<UObject> Owner;
    TArray<FConversionEntry> ConversionQueue;
    TArray<FPointCloudReadyEntry> ReadyQueue;
    FCriticalSection QueueMutex;
    mutable FCriticalSection GradientMutex;
    TArray<AActor*> AvailablePool;
    TSet<AActor*> ActiveActors;
    int32 MaxPoolSize;
    FVector SpawnLocation;
    float SpawnScale;
    float BudgetMs;
    TArray<FColor> GradientLUT;

    AActor* AllocateActor();
    void ReleaseActor(AActor*);
};
