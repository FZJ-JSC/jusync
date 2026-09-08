#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "JUSYNCTypes.h"
#include "JUSYNCBlueprintLibrary.h"
#include "JUSYNCPointCloudSpawner.h"
#include "JUSYNCMeshCache.h"
#include "JUSYNCFileChangeTracker.h"
#include "JUSYNCAnimationController.h"
#include "JUSYNCUSDLoader.h"
#include "JUSYNCFileSpawnerActor.generated.h"

class UTexture2D;
class URealtimeMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
struct FJUSYNCParsedFileResult;

UENUM(BlueprintType)
enum class EJUSYNCSpawnerState : uint8
{
    Idle          UMETA(DisplayName = "Idle"),
    Connecting    UMETA(DisplayName = "Connecting"),
    FetchingList  UMETA(DisplayName = "Fetching File List"),
    Downloading   UMETA(DisplayName = "Downloading Files"),
    Spawning      UMETA(DisplayName = "Spawning Meshes"),
    Complete      UMETA(DisplayName = "Complete"),
    Error         UMETA(DisplayName = "Error")
};

UENUM(BlueprintType)
enum class EJUSYNCPointShape : uint8
{
    Square UMETA(DisplayName = "Square"),
    Circle UMETA(DisplayName = "Circle")
};

UENUM(BlueprintType)
enum class EJUSYNCPointOrientation : uint8
{
    FacingCamera UMETA(DisplayName = "Facing Camera"),
    FacingNormal UMETA(DisplayName = "Facing Normal")
};

UENUM(BlueprintType)
enum class EJUSYNCPointScaling : uint8
{
    PerNode UMETA(DisplayName = "Per Node"),
    PerNodeAdaptive UMETA(DisplayName = "Per Node Adaptive"),
    PerPoint UMETA(DisplayName = "Per Point"),
    FixedScreenSize UMETA(DisplayName = "Fixed Screen Size")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSpawnerFileProgress, int32, CurrentFile, int32, TotalFiles);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSpawnerFileComplete, const FString&, Filename, AActor*, SpawnedActor);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSpawnerAllComplete, int32, TotalSpawned, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSpawnerError, const FString&, ErrorMessage);

UCLASS(Blueprintable, BlueprintType, Category = "JUSYNC")
class JUSYNC_API AJUSYNCFileSpawnerActor : public AActor
{
    GENERATED_BODY()

public:
    AJUSYNCFileSpawnerActor();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Connection")
    FString BrokerEndpoint;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Connection")
    int32 RequestTimeoutMs;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Connection")
    float BandwidthBytesPerSecond;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Filters")
    int32 MinimumFileSizeBytes;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Filters")
    bool bFilterUSDOnly;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Filters")
    bool bClipsOnly;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    AActor* SpawnTargetActor;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    FVector BaseSpawnLocation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    float SpawnSpacing;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement", meta = (ClampMin = "1", ClampMax = "100"))
    int32 SpawnGridColumns;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Material")
    UMaterialInterface* SpawnMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Material")
    FString TextureSampleParameterName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    FVector SpawnScale;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    bool bUseUniformScaling;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Behavior")
    bool bAutoStart;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|LiveUpdate", meta = (DisplayName = "Enable Live Updates"))
    bool bEnableLiveUpdates;

    /**
     * Slow backstop poll interval. Broker push notifications are the primary
     * update path; this only catches missed notifications. Set to 0 to disable.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|LiveUpdate", meta = (EditCondition = "bEnableLiveUpdates", EditConditionHides))
    float LiveUpdatePollInterval;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|LiveUpdate", meta = (EditCondition = "bEnableLiveUpdates", EditConditionHides))
    bool bAutoRefreshMeshes;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pipeline", meta = (ClampMin = "1", ClampMax = "16"))
    int32 PipelineDepth;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pipeline", meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxSpawnsPerFrame;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|LiveUpdate", meta = (ClampMin = "0.0", ClampMax = "30.0"))
    float CommitCompleteCooldownSeconds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Diagnostics")
    bool bEnablePerfLogging;

    // Point cloud settings
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "0.0"))
    float PointCloudSize;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    EJUSYNCPointShape PointShape;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    EJUSYNCPointOrientation PointOrientation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    EJUSYNCPointScaling PointScaling;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "0.0", ClampMax = "0.15"))
    float PointSizeBias;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "0.0"))
    float GapFillingStrength;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "1", ClampMax = "256"))
    int32 PointCloudPoolSize;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    bool bCalculatePointCloudNormals;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "0", ClampMax = "10000000"))
    int32 PointCloudNormalsMaxPoints;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "1", ClampMax = "100"))
    int32 PointCloudNormalsQuality;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "0.0"))
    float PointCloudNormalsNoiseTolerance;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "0.0", ClampMax = "30.0"))
    float PointCloudNormalsCooldownSeconds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    bool bSpawnPointClouds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    FString GradientPngFilename;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (InlineEditConditionToggle))
    bool bUseGradientColors;

    // Parsed-file cache
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Cache")
    bool bEnableMeshCache;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Cache", meta = (EditCondition = "bEnableMeshCache", EditConditionHides, ClampMin = "1", ClampMax = "4096"))
    int32 MeshCacheMaxEntries;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Cache", meta = (EditCondition = "bEnableMeshCache", EditConditionHides, ClampMin = "16"))
    int32 MeshCacheMaxMB;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Cache", meta = (EditCondition = "bEnableMeshCache", EditConditionHides))
    bool bCachePointClouds;

    // Time-step animation
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Animation")
    bool bEnableTimeStepAnimation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Animation", meta = (EditCondition = "bEnableTimeStepAnimation", EditConditionHides, ClampMin = "1.0", ClampMax = "240.0"))
    float TimeStepPlaybackFPS;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Animation", meta = (EditCondition = "bEnableTimeStepAnimation", EditConditionHides))
    bool bLoopTimeStepAnimation;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Spawner|State")
    EJUSYNCSpawnerState CurrentState;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Spawner|State")
    int32 FilesDownloaded;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Spawner|State")
    int32 FilesTotal;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Spawner|State")
    int32 ActorsSpawned;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Spawner|State")
    TArray<AActor*> SpawnedActors;

    UPROPERTY(BlueprintAssignable, BlueprintCallable, Category = "JUSYNC|Spawner|Events")
    FOnSpawnerFileProgress OnFileProgress;

    UPROPERTY(BlueprintAssignable, BlueprintCallable, Category = "JUSYNC|Spawner|Events")
    FOnSpawnerFileComplete OnFileComplete;

    UPROPERTY(BlueprintAssignable, BlueprintCallable, Category = "JUSYNC|Spawner|Events")
    FOnSpawnerAllComplete OnAllComplete;

    UPROPERTY(BlueprintAssignable, BlueprintCallable, Category = "JUSYNC|Spawner|Events")
    FOnSpawnerError OnError;

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner")
    void StartSpawning();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner")
    void CancelSpawning();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner")
    void ClearSpawnedActors();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|LiveUpdate", meta = (DisplayName = "Manual Refresh"))
    void ManualRefresh();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Animation")
    void PlayTimeStepAnimation();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Animation")
    void StopTimeStepAnimation();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Animation")
    void SetTimeStepIndex(int32 NewIndex);

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Spawner")
    FVector GetNextSpawnLocation() const;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaSeconds) override;

private:
    void ConnectToBroker();
    void RequestFileList();
    void ProcessAndDownloadFiles();

    void OnFileListReceived_Internal(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks, bool bSuccess, const TArray<uint64>& HashLo, const TArray<uint64>& HashHi);
    void OnFileListError(const FString& ErrorMessage);

    void DownloadGradientPng(UJUSYNCSubsystem* Subsystem);
    void LoadMeshTextureAsync(UJUSYNCSubsystem* Subsystem);
    void MaybeTriggerGradientLoad(const TArray<FString>& Files, const TArray<int64>& Sizes, const TArray<int32>& Ranks, UJUSYNCSubsystem* Subsystem);
    void ApplySenderTextureToActor(AActor* Actor);
    void ApplySenderTextureToComponent(URealtimeMeshComponent* Comp, UTexture2D* SenderTex);
    UMaterialInstanceDynamic* GetOrCreateSenderMID(URealtimeMeshComponent* Comp, UTexture2D* SenderTex);
    void PipelineDownloadNext(UJUSYNCSubsystem* Subsystem);
    void ApplyPointCloudSettingsToSpawner(FJUSYNCPointCloudSpawner* Spawner);

    int32 CalculateDynamicTimeout(int64 FileSizeBytes) const;
    void LoadFileThroughPipeline(const FString& Filename, int32 Rank, int64 Size, uint64 HashLo, uint64 HashHi, int32 FileIndex, bool bIsInitial);
    void ApplyParsedFileData(FJUSYNCParsedFileResult&& Result);
    AActor* SpawnOrUpdateMesh(FJUSYNCCompactMeshRef& Mesh, const FString& Filename, bool bIsRefresh, TArray<AActor*>& ReplacementActors, RealtimeMesh::FRealtimeMeshStreamSet* PrebuiltStreams = nullptr, const TArray<AActor*>* OldMeshActors = nullptr, bool bForceGradientSpawn = false);
    void DestroyUnreplacedMeshActors(const FString& Filename, const TArray<AActor*>& OldMeshActors, const TArray<AActor*>& ReplacementActors);
    void DestroyUnreplacedPCActors(const FString& Filename, const TArray<AActor*>& OldPCActors, const TSet<FString>& NewPCKeys);

    bool BakeLUTVertexColorCompact(FJUSYNCCompactMeshRef& Mesh, UMaterialInterface*& OutVertexMaterial);
    void RegisterMeshForLUTRecolor(AActor* Spawned, FJUSYNCCompactMeshRef& Mesh, const FString& Filename, const FVector& Loc);
    void RecolorGradientPendingMeshes();
    void ProcessGradientDeferredMeshes(bool bForce = false);
    void ProcessGradientDeferredMeshesTimer();
    void RebuildGradientMaterial();
    void FlushHiddenMeshUpdates();

    void CheckAllDownloadsComplete();
    void RetryFailedDownloads();
    void OnPointCloudSpawnedHandler(const FString& EleName, AActor* Spawned);

    UFUNCTION()
    void OnBrokerNotification(const FJUSYNCNotification& Notification);
    void HandleFileUpdateNotification(const FString& Filename, int32_t SourceRank, int64 FileSize, uint64 HashLo, uint64 HashHi);
    void HandleCommitCompleteNotification(bool bIsTimer = false);
    void StartLiveUpdatePolling();
    void StopLiveUpdatePolling();
    void OnLiveUpdateTimer();
    void DiffAndRefreshFileList(const TArray<FString>& NewFiles, const TArray<int64>& NewSizes, const TArray<int32>& NewRanks, bool bIsManual);

    void FilterFileList(const TArray<FString>& InFiles, const TArray<int64>& InSizes, const TArray<int32>& InRanks,
        const TArray<uint64>& InHashLo, const TArray<uint64>& InHashHi,
        TArray<FString>& OutFiles, TArray<int64>& OutSizes, TArray<int32>& OutRanks,
        TArray<uint64>& OutHashLo, TArray<uint64>& OutHashHi,
        TArray<FString>& OutPngFiles, TArray<int64>& OutPngSizes, TArray<int32>& OutPngRanks) const;
    bool QueueRefreshFile(const FString& Filename, int32 Rank, int64 Size, uint64 HashLo, uint64 HashHi);
    void UpdateTrackedFileMetadata(const FString& Filename, int32 Rank, int64 Size, uint64 HashLo, uint64 HashHi);
    int32 DestroyFileActors(const FString& Filename);

    void ChainRefreshNext();
    void RetryRemainingFiles();
    void ProcessDeferredSpawns();

    TArray<FString> RawFileList;
    TArray<int64> RawFileSizes;
    TArray<int32> RawFileRanks;
    TArray<uint64> RawHashLo;
    TArray<uint64> RawHashHi;

    TArray<FString> FilteredFiles;
    TArray<int64> FilteredSizes;
    TArray<int32> FilteredRanks;
    TArray<uint64> FilteredHashLo;
    TArray<uint64> FilteredHashHi;
    struct FJUSYNCFileCandidateMeta
    {
        int32 Rank = 0;
        int64 Size = 0;
    };
    TMap<FString, FJUSYNCFileCandidateMeta> GradientPngRankMap;
    TMap<int32, TArray<FColor>> RankGradients;
    std::atomic<bool> bGradientReady;
    std::atomic<bool> bGradientDownloadStarted;
    bool bGradientAttempted;
    TWeakObjectPtr<UTexture2D> GradientLUTTexture;
    TWeakObjectPtr<UMaterialInterface> GradientMaterial;
    FName GradientTextureParameterName;

    struct FRecolorMeshEntry
    {
        FJUSYNCCompactMeshRef Mesh;
        FString Filename;
        FVector Loc = FVector::ZeroVector;
        FRotator Rot = FRotator::ZeroRotator;
        FVector Scale = FVector::OneVector;
    };
    TMap<AActor*, FRecolorMeshEntry> GradientPendingMeshes;
    TMap<AActor*, FRecolorMeshEntry> HiddenPendingMeshUpdates;

    TMap<FString, TWeakObjectPtr<UTexture2D>> MeshTextureCache;
    TWeakObjectPtr<UTexture2D> ActiveMeshTexture;
    std::atomic<bool> bMeshTextureLoading;
    bool bMeshTextureReady;
    TMap<URealtimeMeshComponent*, TWeakObjectPtr<UMaterialInstanceDynamic>> SenderMIDCache;

    int32 NextSpawnIndex;
    int32 PendingDownloads;
    int32 PendingAsyncSpawns;
    int32 PendingAsyncPCS;
    int32 PendingParseTasks;
    int32 PipelineNextIndex;
    int32 PipelineActive;
    bool bIsCancelled;
    int32 MaxRetries;
    int32 CurrentRetryCount;
    TArray<int32> FailedFileIndices;
    TArray<int32> ParseFailedIndices;

    // Live update state
    FTimerHandle LiveUpdateTimerHandle;
    float LiveUpdatePollAccumulator = 0.0f;
    TMap<FString, AActor*> FileToActorMap;
    TMap<FString, TArray<AActor*>> FilenameToActors;
    TMap<FString, TArray<AActor*>> FilenameToPCActors;
    TMap<FString, FString> PCElementToFilename;

    TMap<FString, uint64> FileHashLo;
    TMap<FString, uint64> FileHashHi;
    TMap<FString, int64> FileLastSize;
    double LastCommitCompleteTime;
    bool bSceneDiffInFlight;
    bool bInitialSpawnDone;

    // Persistent V2-seen file→rank map.
    TMap<FString, int32> SeenV2Files;
    FTimerHandle SpawnThrottleTimer;

    TUniquePtr<FJUSYNCMeshCache> MeshCache;
    TUniquePtr<FJUSYNCFileChangeTracker> ChangeTracker;
    TUniquePtr<FJUSYNCAnimationController> AnimationController;

    struct FDeferredSpawnEntry
    {
        TArray<FJUSYNCCompactMeshRef> Meshes;
        FString Filename;
        TArray<AActor*> OldActors;
        TArray<AActor*> ReplacementActors;
        int32 ExpectedNewMeshes = 0;
        int32 SpawnedNewMeshes = 0;
        uint64 Generation = 0;
        TArray<TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet>> PrebuiltStreams;
    };
    TArray<FDeferredSpawnEntry> DeferredSpawns;

    struct FJUSYNCGradientDeferredMeshEntry
    {
        FJUSYNCCompactMeshRef Mesh;
        FString Filename;
        bool bIsRefresh = false;
        TArray<AActor*> OldActors;
        int32 ExpectedNewMeshes = 1;
        int32 SpawnedNewMeshes = 0;
        uint64 Generation = 0;
        double DeferredTime = 0.0;
        TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet> PrebuiltStream;
    };
    TArray<FJUSYNCGradientDeferredMeshEntry> GradientDeferredMeshes;
    TMap<FString, TArray<AActor*>> GradientDeferredReplacementActors;
    FTimerHandle GradientDeferredTimerHandle;
    float GradientDeferredTimeoutSeconds = 3.0f;
};
