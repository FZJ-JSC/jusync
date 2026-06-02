#include "JUSYNCPointCloudSpawner.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/Engine.h"
#include "Async/ParallelFor.h"

#ifdef WITH_ANARI_USD_MIDDLEWARE
#include "LidarPointCloudComponent.h"
#include "LidarPointCloudActor.h"
#endif

FJUSYNCPointCloudSpawner::FJUSYNCPointCloudSpawner(TWeakObjectPtr<UObject> InOwner)
    : Owner(InOwner), MaxPoolSize(8), SpawnLocation(FVector::ZeroVector),
      SpawnScale(1.0f), BudgetMs(16.0f)
{
}

void FJUSYNCPointCloudSpawner::EnqueuePointCloud(const FJUSYNCPointCloudData& PCData, int32 InRank)
{
    if (!PCData.IsValid() || !Owner.IsValid()) return;

    FConversionEntry Entry;
    Entry.Positions = PCData.Positions;
    Entry.Colors = PCData.Colors;
    Entry.Widths = PCData.Widths;
    Entry.bHasColors = PCData.HasColors();
    Entry.PointCount = PCData.PointCount;
    Entry.ElementName = PCData.ElementName;
    Entry.Rank = InRank;

    {
        FScopeLock Lock(&QueueMutex);
        ConversionQueue.Add(MoveTemp(Entry));
    }

    UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: queued PC '%s' for async conversion (%d points)"),
           *PCData.ElementName, PCData.PointCount);

    // Spawn async task — doesn't block the game thread
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
        [this, PointCount = PCData.PointCount, Positions = PCData.Positions,
          Colors = PCData.Colors, bHasColors = PCData.HasColors(),
          Widths = PCData.Widths, ElementName = PCData.ElementName,
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

        // Build LiDAR points on background thread
        TArray64<FLidarPointCloudPoint> Points;
        Points.SetNum(PointCount);
        for (int32 i = 0; i < PointCount; ++i)
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
            Points[i] = FLidarPointCloudPoint(Pos, Col, true, 0);
        }

        // Marshal to game thread
        FPointCloudReadyEntry Entry;
        Entry.Points = MoveTemp(Points);
        Entry.ElementName = ElementName;
        Entry.Rank = InRank;
        Entry.PointSize = 1.0f;
        Entry.bSpawned = false;

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [this, Entry = MoveTemp(Entry)]() mutable
            {
                {
                    FScopeLock Lock(&QueueMutex);
                    ReadyQueue.Add(MoveTemp(Entry));

                    // Threshold flush: if queue exceeds 8, drain immediately
                    if (ReadyQueue.Num() >= 8)
                    {
                        DrainReadyQueue();
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
    });
}

AActor* FJUSYNCPointCloudSpawner::AllocateActor()
{
    if (AvailablePool.Num() > 0)
    {
        AActor* Actor = AvailablePool.Pop();
        if (Actor)
        {
            Actor->SetActorEnableCollision(false);
            return Actor;
        }
    }

    if (ActiveActors.Num() >= MaxPoolSize)
    {
        if (AActor* Oldest = ActiveActors.Array().Num() > 0 ? ActiveActors.Array()[0] : nullptr)
        {
            ActiveActors.Remove(Oldest);
            Oldest->SetActorHiddenInGame(false);
            return Oldest;
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
            Comp->PointSize = 1.0f;
            Comp->MinDepth = 0;
            Comp->MaxDepth = -1;
            Comp->bUseFrustumCulling = false;
        }

        SpawnedActor->SetActorHiddenInGame(true);
        SpawnedActor->SetActorTickEnabled(false);
        SpawnedActor->SetActorScale3D(FVector(SpawnScale));
    }

    ActiveActors.Add(SpawnedActor);
    return SpawnedActor;
}

void FJUSYNCPointCloudSpawner::ReleaseActor(AActor* Actor)
{
    if (!Actor) return;

    ActiveActors.Remove(Actor);
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (auto* LidarActor = Cast<ALidarPointCloudActor>(Actor))
    {
        if (auto* Comp = LidarActor->GetPointCloudComponent())
        {
            Comp->SetPointCloud(nullptr);
        }
    }
#endif

    Actor->SetActorHiddenInGame(true);
    Actor->SetActorEnableCollision(false);
    AvailablePool.Add(Actor);
}

void FJUSYNCPointCloudSpawner::SetSpawnLocation(const FVector& In)
{
    SpawnLocation = In;
}

void FJUSYNCPointCloudSpawner::Tick(float DeltaTime)
{
    if (!Owner.IsValid()) return;
    DrainReadyQueue();
}

void FJUSYNCPointCloudSpawner::DrainReadyQueue()
{
    if (ReadyQueue.Num() == 0) return;

    double StartTime = FPlatformTime::Seconds();
    int32 ItemsProcessed = 0;
    double RemainingBudget = (BudgetMs / 1000.0);

    while (ReadyQueue.Num() > 0)
    {
        double Elapsed = FPlatformTime::Seconds() - StartTime;
        if (Elapsed > RemainingBudget && ItemsProcessed > 0)
        {
            break;
        }

        FPointCloudReadyEntry& Entry = ReadyQueue.Last();

        if (Entry.bSpawned)
        {
            ReadyQueue.Pop();
            continue;
        }

        AActor* Actor = AllocateActor();
        if (!Actor)
        {
            break;
        }

#ifdef WITH_ANARI_USD_MIDDLEWARE
        ULidarPointCloud* LidarCloud = ULidarPointCloud::CreateFromData(Entry.Points, false);

        if (!LidarCloud)
        {
            FString EntryName = Entry.ElementName;
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: failed to create LiDAR cloud for '%s'"),
                   *EntryName);
            ReleaseActor(Actor);
            ReadyQueue.Pop();
            continue;
        }

        // Refresh bounds to ensure correct frustum intersection in LOD Manager
        LidarCloud->RefreshBounds();

        ULidarPointCloudComponent* Comp = Cast<ALidarPointCloudActor>(Actor)->GetPointCloudComponent();
        if (Comp)
        {
            Comp->SetPointCloud(LidarCloud);
        }

        Actor->SetActorHiddenInGame(false);
        Actor->SetActorEnableCollision(false);

        FString EntryName = Entry.ElementName;
        int32 EntryPoints = Entry.Points.Num();
        Entry.bSpawned = true;
        ReadyQueue.Pop();
        ItemsProcessed++;

        OnPointCloudSpawned.Broadcast(EntryName, Actor);

        UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: loaded PC actor '%s' (%d points, budget: %.1fms remaining)"),
               *EntryName, EntryPoints, (RemainingBudget - Elapsed) * 1000.0f);
#endif
    }
}

void FJUSYNCPointCloudSpawner::ClearAllActors()
{
    for (AActor* Actor : ActiveActors)
    {
        ReleaseActor(Actor);
    }
    ActiveActors.Empty();

    for (AActor* Actor : AvailablePool)
    {
        Actor->Destroy();
    }
    AvailablePool.Empty();
}
