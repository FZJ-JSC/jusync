#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "JUSYNCTypes.h"
#include "JUSYNCBlueprintLibrary.h"
#include "JUSYNCFileSpawnerActor.h"
#include "JUSYNCBenchmarkActor.generated.h"

// Forward declarations
class UMaterialInterface;
class UPrimitiveComponent;
class UJUSYNCSubsystem;

/**
 * End-to-end benchmark actor for the full JUSYNC pipeline.
 *
 * Runs the complete pipeline multiple times (warmup + measured) and reports
 * per-stage timing (Connect -> FileList -> Download -> Parse -> MeshSpawn ->
 * PointCloudSpawn -> Material -> Teardown) plus memory/VRAM/throughput metrics.
 *
 * Pipeline is instrumented, not a wrapper around AJUSYNCFileSpawnerActor, so we
 * get high-resolution timestamps at each stage boundary. Drop into a level and
 * set bAutoStart = true, or call StartBenchmarkRun() from Blueprint / console.
 */
UCLASS(Blueprintable, BlueprintType, Category = "JUSYNC|Benchmark")
class JUSYNC_API AJUSYNCBenchmarkActor : public AActor
{
    GENERATED_BODY()

public:
    AJUSYNCBenchmarkActor();

    // ========== CONNECTION CONFIG (mirrors AJUSYNCFileSpawnerActor) ==========
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Connection")
    FString BrokerEndpoint;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Connection")
    int32 RequestTimeoutMs;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Connection")
    float BandwidthBytesPerSecond;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Filters")
    int32 MinimumFileSizeBytes;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Filters")
    bool bFilterUSDOnly;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Filters")
    bool bClipsOnly;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Pipeline", meta = (ClampMin = "1", ClampMax = "16"))
    int32 PipelineDepth;

    // ========== BENCHMARK CONFIG ==========
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Config")
    int32 WarmupRuns;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Config")
    int32 MeasuredRuns;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Config")
    FString OutputDirectory;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Config")
    int32 OutputFormat; // 0=CSV, 1=JSON, 2=Both

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Config")
    bool bAutoStart;

    // Repeat each full pipeline run this many times per config? (kept simple: N total)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Config")
    bool bGarbageCollectBetweenRuns;

    // ========== SPAWNING CONFIG ==========
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Placement")
    FVector BaseSpawnLocation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Placement")
    float SpawnSpacing;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Placement")
    int32 SpawnGridColumns;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Placement")
    FVector SpawnScale;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Placement")
    bool bUseUniformScaling;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Placement")
    UMaterialInterface* SpawnMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Placement")
    bool bSpawnPointClouds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Placement")
    float PointCloudSize;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Placement")
    bool bUseGradientColors;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Benchmark|Placement")
    int32 MaxSpawnsPerFrame;

    // ========== STATUS ==========
    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark|State")
    int32 CompletedRuns;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark|State")
    bool bIsRunning;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark|State")
    int32 ActorsSpawned;

    // Events
    UPROPERTY(BlueprintAssignable, Category = "JUSYNC|Benchmark|Events")
    FOnSpawnerAllComplete OnBenchmarkComplete;

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Benchmark")
    void StartBenchmarkRun();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Benchmark")
    void CancelBenchmark();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Benchmark")
    void ClearSpawnedActors();

    // C++ only - returns per-run benchmark results
    TArray<FJUSYNCBenchmarkResult> GetResults() const;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    // Stage timing storage for one measured run
    struct FRunTimings
    {
        double ConnectMs = 0.0;
        double FileListMs = 0.0;
        double DownloadMs = 0.0;
        double ParseMs = 0.0;
        double MeshSpawnMs = 0.0;
        double PCSpawnMs = 0.0;
        double MaterialMs = 0.0;
        double TeardownMs = 0.0;
        double TotalMs = 0.0;

        int64 TotalBytes = 0;
        int32 FilesRequested = 0;
        int32 ActorsSpawned = 0;
        int32 MeshCount = 0;
        int32 PCCount = 0;
        int32 PCPoints = 0;
        int32 Materials = 0;
        int32 Hitches = 0;
        double MaxHitchMs = 0.0;
    };

    FRunTimings CurrentRunTimings;

    // ========== PIPELINE STATE (copied from AJUSYNCFileSpawnerActor, simplified) ==========
    TArray<FString>           FilteredFiles;
    TArray<int64>             FilteredSizes;
    TArray<int32>             FilteredRanks;
    TArray<uint64>            RawHashLo;
    TArray<uint64>            RawHashHi;
    TMap<FString, int32>      GradientPngRankMap;
    TArray<AActor*>           SpawnedActors;
    TArray<int32>             FailedFileIndices;
    TArray<int32>             ParseFailedIndices;
    bool  bGradientReady      = false;
    bool  bGradientAttempted  = false;
    int32 FilesDownloaded     = 0;
    int32 FilesTotal          = 0;
    int32 PipelineNextIndex   = 0;
    int32 PipelineActive      = 0;
    int32 PendingAsyncPCS     = 0;
    int32 NextSpawnIndex      = 0;
    bool  bIsCancelled        = false;

    // ========== TIMER / TIMING HELPERS ==========
    double SessionStartTime    = 0.0;
    double StageStartTime      = 0.0;
    double DownloadStageStart  = 0.0;  // download->spawn window start
    int64  TotalBytesAllRuns   = 0;
    int32  TotalTriangleCount  = 0;
    int32  TotalVertexCount    = 0;
    int32  TotalPCPoints       = 0;

    int32  RunIndex            = 0;
    int32  TotalRuns           = 0;
    bool   bIsWarmup           = false;

    // Baseline memory measurements
    int64  SessionRAMStart     = 0;
    int64  SessionVRAMStart    = 0;

    FTimerHandle CompleteWaitTimerHandle;

    // ========== PIPELINE (copied from AJUSYNCFileSpawnerActor) ==========
    void RunPipelineIteration();
    void ConnectToBroker();
    void RequestFileList();
    void OnFileListReceived_Internal(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks, bool bSuccess);
    void OnFileListError(const FString& ErrorMessage);
    void ProcessAndDownloadFiles();
    void PipelineDownloadNext(UJUSYNCSubsystem* Subsystem);
    void OnSingleFileDownloaded(const FString& Filename, const TArray<uint8>& FileData, bool bSuccess, int32 FileIndex, int32 TargetRank);
    void ChainNextOrComplete();
    void SpawnMeshFromData(const FString& Filename, bool bParsed, TArray<FJUSYNCMeshData>&& MeshData, TArray<FJUSYNCPointCloudData>&& PointCloudData, int32 FileIndex, int32 TargetRank);
    void ApplyDynamicMaterial(UPrimitiveComponent* Comp, const FString& Filename);
    void DownloadGradientPng(UJUSYNCSubsystem* Subsystem);
    void CheckIterationComplete();
    void OnPointCloudSpawnedHandler(const FString& EleName, AActor* Spawned);
    int32 CalculateDynamicTimeout(int64 FileSizeBytes) const;
    FVector GetNextSpawnLocation() const;
    void FinalizeRun();
    void BeginNextRun();

    // Benchmark bookkeeping
    TArray<FJUSYNCBenchmarkResult> BenchmarkResults;
    void StartStage();
    void EndStage(double& OutMs);
    void SnapshotRAMAndVRAM(int64& OutRAM, int64& OutVRAM);
    void ComputeAndRecordResult();
    void SaveResultsToCSV(const FString& InDirectory);
    void SaveResultsToJSON(const FString& InDirectory);
    static void ComputeStageStats(const TArray<float>& InValues, float& OutMin, float& OutMedian, float& OutMax, float& OutStdDev, float& OutP95);
};
