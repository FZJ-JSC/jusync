#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "JUSYNCTypes.h"
#include "JUSYNCBlueprintLibrary.h"
#include "JUSYNCPointCloudSpawner.h"
#include "JUSYNCFileSpawnerActor.generated.h"

class UTexture2D;
class URealtimeMeshComponent;
class UMaterialInterface;

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

    /** Broker endpoint, e.g. "tcp://192.168.1.100:5556" */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Connection")
    FString BrokerEndpoint;

    /** Minimum timeout for any file request (ms). Actual timeout is dynamic based on file size. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Connection")
    int32 RequestTimeoutMs;

    /** Estimated network bandwidth in bytes/sec for dynamic timeout calculation (default: 10GB/s = 10737418240) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Connection")
    float BandwidthBytesPerSecond;

    /** Minimum file size to include in bytes (skip manifests/metadata) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Filters")
    int32 MinimumFileSizeBytes;

    /** Only process USD files */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Filters")
    bool bFilterUSDOnly;

    /** Only spawn files under "clips/" path (excludes shared manifests/materials/camera) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Filters")
    bool bClipsOnly;

    /** Target actor (e.g., Target Point, Empty, etc.) — spawns use its location as origin. Leave blank to use this actor's location. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    AActor* SpawnTargetActor;

    /** Fallback spawn location when SpawnTargetActor is not set */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    FVector BaseSpawnLocation;

    /** Spacing between spawned meshes */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    float SpawnSpacing;

    /** Number of columns in the spawn grid (default: 10) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement", meta = (ClampMin = "1", ClampMax = "100"))
    int32 SpawnGridColumns;

    /** Base material — a dynamic instance is created per mesh. If blank, default material is used. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Material")
    UMaterialInterface* SpawnMaterial;

    /** Texture sample parameter name on the material to assign downloaded textures to. Leave blank to auto-detect. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Material")
    FString TextureSampleParameterName;

    /** Scale factor for spawned meshes */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    FVector SpawnScale;

    /** Use uniform scaling (X = Y = Z) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    bool bUseUniformScaling;

    /** Automatically start spawning when the actor begins play */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Behavior")
    bool bAutoStart;

    /** Enable live update mode: watches for broker notifications and polls for file changes */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|LiveUpdate", meta = (DisplayName = "Enable Live Updates"))
    bool bEnableLiveUpdates;

    /** Poll interval for checking file changes when in live update mode (seconds). Set to 0 to rely solely on broker notifications. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|LiveUpdate", meta = (EditCondition = "bEnableLiveUpdates", EditConditionHides))
    float LiveUpdatePollInterval;

    /** Automatically destroy and re-spawn updated meshes when a file change is detected */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|LiveUpdate", meta = (EditCondition = "bEnableLiveUpdates", EditConditionHides))
    bool bAutoRefreshMeshes;

    /** Pipeline depth: how many files to download ahead while spawning previous ones (1 = sequential, higher = more overlap) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pipeline", meta = (ClampMin = "1", ClampMax = "16"))
    int32 PipelineDepth;

    // Point cloud settings
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    float PointCloudSize;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    bool bSpawnPointClouds;

    /** Gradient PNG filename on the broker (e.g. "shared/gradient.png"). Leave blank to auto-detect first .png */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    FString GradientPngFilename;

    /** Look up point colors from gradient PNG using attribute0 as the index */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (InlineEditConditionToggle))
    bool bUseGradientColors;

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

    /** Manually trigger a full refresh: re-fetch file list, diff changes, and update spawned actors */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|LiveUpdate", meta = (DisplayName = "Manual Refresh"))
    void ManualRefresh();

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Spawner")
    FVector GetNextSpawnLocation() const;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    void ConnectToBroker();
    void RequestFileList();
    void ProcessAndDownloadFiles();

    void OnFileListReceived(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks);
    void OnFileListReceived_Internal(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks, bool bSuccess);
    void OnFileListReceived_Internal(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks, bool bSuccess, const TArray<uint64>& HashLo, const TArray<uint64>& HashHi);
    void OnFileListReceived_Internal_Common(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks, bool bSuccess);
    void OnFileListError(const FString& ErrorMessage);
    void OnFileDownloaded(const FString& Filename, const TArray<uint8>& FileData);
    void OnSingleFileDownloaded(const FString& Filename, TUniquePtr<TArray<uint8>> FileData, bool bSuccess, int32 FileIndex, int32 TargetRank);
    void OnFileDownloadError(const FString& ErrorMessage);
    void DownloadGradientPng(UJUSYNCSubsystem* Subsystem);
    void LoadMeshTextureAsync(UJUSYNCSubsystem* Subsystem);
    // Ingest any gradient PNGs found in Files and kick off the LUT download once (idempotent).
    void MaybeTriggerGradientLoad(const TArray<FString>& Files, const TArray<int32>& Ranks, UJUSYNCSubsystem* Subsystem);
    void ApplySenderTextureToActor(AActor* Actor);
    void ApplySenderTextureToComponent(URealtimeMeshComponent* Comp, UTexture2D* SenderTex);
    void PipelineDownloadNext(UJUSYNCSubsystem* Subsystem);

    int32 CalculateDynamicTimeout(int64 FileSizeBytes) const;
    void SpawnMeshFromData(const FString& Filename, bool bParsed, TArray<FJUSYNCMeshData>&& MeshData, TArray<FJUSYNCPointCloudData>&& PointCloudData, int32 FileIndex, int32 TargetRank);
    /** Bake the point-cloud color-map LUT into per-vertex color (USD primvars:attribute0 -> UV.x,
        the same scalar the point cloud indexes the LUT with) and return the vertex-color material
        to use. Returns false if a real sender texture is present, the LUT is not ready, the mesh
        has no per-vertex scalar, or the vertex-color material is missing — in which case the caller
        should fall back to the spawn material. */
    bool BakeLUTVertexColor(FJUSYNCMeshData& Mesh, UMaterialInterface*& OutVertexMaterial);
    /** Queue a spawned mesh for LUT recolor if it fell back because the color-map LUT was not ready yet. */
    void RegisterMeshForLUTRecolor(AActor* Spawned, const FJUSYNCMeshData& Mesh, const FString& Filename, const FVector& Loc);
    /** Re-spawn any mesh queued via RegisterMeshForLUTRecolor now that the LUT is ready. */
    void RecolorGradientPendingMeshes();
    void ApplyDynamicMaterial(UPrimitiveComponent* Comp, const FString& Filename);
    void CheckAllDownloadsComplete();
    void RetryFailedDownloads();
    void FlushBufferedPointClouds();
    void OnPointCloudSpawnedHandler(const FString& EleName, AActor* Spawned);

    // Live update support
    UFUNCTION()
    void OnBrokerNotification(const FJUSYNCNotification& Notification);
    void HandleFileUpdateNotification(const FString& Filename, int32_t SourceRank);
    void HandleCommitCompleteNotification(bool bIsTimer = false);
    void StartLiveUpdatePolling();
    void StopLiveUpdatePolling();
    void OnLiveUpdateTimer();
    bool RefreshSingleFile(const FString& Filename, int32 TargetRank);
    void DiffAndRefreshFileList(const TArray<FString>& NewFiles, const TArray<int64>& NewSizes, const TArray<int32>& NewRanks, bool bIsManual);

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
    TMap<FString, int32> GradientPngRankMap;
    TMap<int32, TArray<FColor>> RankGradients;
    std::atomic<bool> bGradientReady;
    std::atomic<bool> bGradientDownloadStarted;  // Set once the gradient PNG download has been kicked off
    bool bGradientAttempted;  // Guard: only attempt middleware gradient once
    // Meshes spawned before the color-map LUT was ready (fell back to the spawn/texture material).
    // Recolored (re-spawned with per-vertex color) by RecolorGradientPendingMeshes() once the LUT
    // arrives — mirrors the point-cloud GradientPendingActors / RecolorGradientPendingActors path.
    struct FRecolorMeshEntry
    {
        FJUSYNCMeshData Mesh;
        FString Filename;
        FVector Loc = FVector::ZeroVector;
        FRotator Rot = FRotator::ZeroRotator;
        FVector Scale = FVector::OneVector;
    };
    TMap<AActor*, FRecolorMeshEntry> GradientPendingMeshes;

    // Sender full-texture for meshes (distinct from the point-cloud gradient LUT).
    TMap<FString, TWeakObjectPtr<UTexture2D>> MeshTextureCache;
    TWeakObjectPtr<UTexture2D> ActiveMeshTexture;
    std::atomic<bool> bMeshTextureLoading;
    bool bMeshTextureReady;
    TArray<FJUSYNCPointCloudData> PendingPointClouds;

    int32 NextSpawnIndex;
    int32 PendingDownloads;
    int32 PendingAsyncSpawns;
    int32 PendingAsyncPCS;
    int32 PipelineNextIndex;
    int32 PipelineActive;
    bool bIsCancelled;
    int32 MaxRetries;
    int32 CurrentRetryCount;
    TArray<int32> FailedFileIndices;
    TArray<int32> ParseFailedIndices;

    // Live update state
    FTimerHandle LiveUpdateTimerHandle;
    TMap<FString, AActor*> FileToActorMap;
    // Reverse index: filename → actors spawned from it (for O(1) old actor collection)
    TMap<FString, TArray<AActor*>> FilenameToActors;
    TMap<FString, uint64> FileHashLo;
    TMap<FString, uint64> FileHashHi;
    TMap<FString, int64> FileLastSize;
    double LastCommitCompleteTime;
    double CommitCompleteCooldown;
    bool bCommitDiffInProgress;

    // Live update guards
    bool bInitialSpawnDone;
    TSet<FString> RefreshedFiles;
    TSet<FString> RefreshingFiles;

    // Depth-gated refresh queue (reuses PipelineDepth)
    int32 RefreshActive;
    TArray<TPair<FString, int32>> RefreshRemainingFiles;

    // V2 download-active counter: prevents timer chain refresh from firing while
    // V2 per-file refreshes are downloading+spawning on background threads.
    std::atomic<int32> V2ActiveDownloads;

    // Persistent V2-seen file→rank map: tracks ALL files the broker notifies about,
    // so when a stub file (tiny, filtered) later grows into real geometry, we can spawn it.
    TMap<FString, int32> SeenV2Files;
    FTimerHandle SpawnThrottleTimer;
    int32 MaxSpawnsPerFrame;
    TArray<TPair<TArray<FJUSYNCMeshData>, FString>> DeferredSpawns;

    // Forward-declare helper
    void ChainRefreshNext();
    void RetryRemainingFiles();
    void ProcessDeferredSpawns();
};
