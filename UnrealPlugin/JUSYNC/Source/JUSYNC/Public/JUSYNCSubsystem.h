#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "JUSYNCTypes.h"
#include "RealtimeMeshComponent.h"
#include "RealtimeMeshSimple.h"
#include "Mesh/RealtimeMeshBasicShapeTools.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Containers/Map.h"
#include "UObject/SoftObjectPtr.h"
#include <functional>

#ifdef WITH_ANARI_USD_MIDDLEWARE
#include "AnariUsdMiddleware.h"
#endif

// Forward declaration to avoid circular dependency
class UJUSYNCBlueprintLibrary;
class FJUSYNCPointCloudSpawner;

#include "JUSYNCSubsystem.generated.h"

// Forward declarations
class URealtimeMeshComponent;

UCLASS()
class JUSYNC_API UJUSYNCSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    // USubsystem interface
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

    // Async point cloud spawner with actor pooling
    FJUSYNCPointCloudSpawner* GetPointCloudSpawner() const { return PCSpawner.Get(); }

    // Extract cached gradient from middleware and apply to spawner LUT
    void ApplyCachedGradientToSpawner();

    // Core Connection Management
    UFUNCTION(BlueprintCallable, Category = "JUSYNC")
    bool InitializeMiddleware(const FString& Endpoint = TEXT(""));

    UFUNCTION(BlueprintCallable, Category = "JUSYNC")
    void ShutdownMiddleware();

    UFUNCTION(BlueprintPure, Category = "JUSYNC")
    bool IsMiddlewareConnected() const;

    UFUNCTION(BlueprintPure, Category = "JUSYNC")
    FString GetStatusInfo() const;

    // Data Reception
    UFUNCTION(BlueprintCallable, Category = "JUSYNC")
    bool StartReceiving();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC")
    void StopReceiving();

    // Event Dispatchers
    UPROPERTY(BlueprintAssignable, Category = "JUSYNC Events")
    FJUSYNCFileReceived OnFileReceived;

    UPROPERTY(BlueprintAssignable, Category = "JUSYNC Events")
    FJUSYNCMessageReceived OnMessageReceived;

    UPROPERTY(BlueprintAssignable, Category = "JUSYNC Events")
    FJUSYNCProcessingProgress OnProcessingProgress;

    UPROPERTY(BlueprintAssignable, Category = "JUSYNC Events")
    FJUSYNCError OnError;

    UPROPERTY(BlueprintAssignable, Category = "JUSYNC Events")
    FJUSYNCPointCloudReceived OnPointCloudReceived;

    // Live update notification from broker
    UPROPERTY(BlueprintAssignable, Category = "JUSYNC Events")
    FJUSYNCNotificationReceived OnNotificationReceived;

    // USD Processing (Legacy - use JUSYNCBlueprintLibrary versions for preview support)
    UFUNCTION(BlueprintCallable, Category = "JUSYNC USD|Legacy", DisplayName = "Load USD From Buffer (Legacy)")
    bool LoadUSDFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCMeshData>& OutMeshData);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC USD|Legacy", DisplayName = "Load USD From Disk (Legacy)")
    bool LoadUSDFromDisk(const FString& FilePath, TArray<FJUSYNCMeshData>& OutMeshData);

    bool LoadUSDFullFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCMeshData>& OutMeshData, TArray<FJUSYNCPointCloudData>& OutPointCloudData);

    /**
     * Zero-copy variant: bypasses std::vector copy at C API boundary.
     * Uses LoadUSDFullFromPointer_C internally.
     */
    bool LoadUSDFullFromBufferNoCopy(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCMeshData>& OutMeshData, TArray<FJUSYNCPointCloudData>& OutPointCloudData);

    // Texture Processing
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    FJUSYNCTextureData CreateTextureFromBuffer(const TArray<uint8>& Buffer);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    bool WriteGradientLineAsPNG(const TArray<uint8>& Buffer, const FString& OutputPath);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    bool GetGradientLineAsPNGBuffer(const TArray<uint8>& Buffer, TArray<uint8>& OutPNGBuffer);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    bool GetPNGDimensions(const TArray<uint8>& Buffer, int32& OutWidth, int32& OutHeight, int32& OutChannels);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    bool GetImageRowAsPNGBuffer(const TArray<uint8>& Buffer, int32 RowIndex, TArray<uint8>& OutPNGBuffer);

    // Broadcast duplicate handling
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broadcast")
    void ClearProcessedFiles();

    // RealtimeMesh Integration
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh|Legacy", DisplayName = "Create Realtime Mesh From JUSYNC (Legacy)")
    bool CreateRealtimeMeshFromJUSYNC(
        const FJUSYNCMeshData& MeshData,
        URealtimeMeshComponent* RealtimeMeshComponent
    );


    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh|Legacy", DisplayName = "Batch Create Realtime Meshes From JUSYNC (Legacy)")
    bool BatchCreateRealtimeMeshesFromJUSYNC(const TArray<FJUSYNCMeshData>& MeshDataArray, const TArray<URealtimeMeshComponent*>& MeshComponents);

    // Large mesh handling with automatic splitting
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh|Advanced", DisplayName = "Create Realtime Mesh From JUSYNC With Splitting")
    bool CreateRealtimeMeshFromJUSYNCWithSplitting(
        const FJUSYNCMeshData& MeshData,
        URealtimeMeshComponent* RealtimeMeshComponent,
        int32 MaxVerticesPerChunk = 32768
    );

    // Core mesh splitting algorithm for large meshes
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh|Advanced", DisplayName = "Split Large Mesh For RealtimeMesh")
    TArray<FJUSYNCMeshData> SplitLargeMeshForRealtimeMesh(
        const FJUSYNCMeshData& LargeMesh,
        int32 MaxVerticesPerChunk = 32768,
        bool bPreserveConnectivity = true
    );

    // Create multiple RMC components for very large meshes
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh|Advanced", DisplayName = "Create Multiple RMC Components For Large Mesh")
    TArray<URealtimeMeshComponent*> CreateMultipleRMCComponentsForLargeMesh(
        AActor* ParentActor,
        const FJUSYNCMeshData& LargeMesh,
        int32 MaxVerticesPerComponent = 65535
    );

    // Memory limit detection
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh|Advanced", DisplayName = "Check Memory Limits For Mesh")
    bool CheckMemoryLimitsForMesh(
        const FJUSYNCMeshData& MeshData,
        float& OutRequiredRAM_MB,
        float& OutRequiredVRAM_MB,
        float SafetyMarginPercent = 20.0f
    );

    // Conversion utilities for RealtimeMesh
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh")
    FJUSYNCRealtimeMeshData ConvertToRealtimeMeshFormat(const FJUSYNCMeshData& StandardMesh);

    // Point Cloud Processing
    UFUNCTION(BlueprintCallable, Category = "JUSYNC PointCloud", DisplayName = "Load Point Cloud From USD Buffer")
    bool LoadPointCloudFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCPointCloudData>& OutPointCloudData);

    // Async point cloud loading
    DECLARE_DELEGATE_ThreeParams(FOnPointCloudLoaded, const TArray<FJUSYNCPointCloudData>&, bool, const FString&);
    void LoadPointCloudFromBuffer_Async(const TArray<uint8>& Buffer, const FString& Filename, FOnPointCloudLoaded OnLoaded);

    // Spawn point cloud as LiDAR actor
    UFUNCTION(BlueprintCallable, Category = "JUSYNC PointCloud", DisplayName = "Spawn Lidar Point Cloud At Location")
    AActor* SpawnLidarPointCloudAtLocation(
        const FJUSYNCPointCloudData& PointCloudData,
        FVector Location,
        FRotator Rotation = FRotator::ZeroRotator,
        FVector Scale3D = FVector(1.0f)
    );

    // Async point cloud spawning
    DECLARE_DELEGATE_TwoParams(FOnPointCloudSpawned, AActor*, bool);
    void SpawnLidarPointCloudAtLocation_Async(
        const FJUSYNCPointCloudData& PointCloudData,
        FVector Location,
        FRotator Rotation,
        FVector Scale3D,
        FOnPointCloudSpawned OnSpawned
    );

    // Batch spawn point clouds
    UFUNCTION(BlueprintCallable, Category = "JUSYNC PointCloud", DisplayName = "Batch Spawn Point Clouds At Locations")
    TArray<AActor*> BatchSpawnPointCloudsAtLocations(
        const TArray<FJUSYNCPointCloudData>& PointCloudDataArray,
        const TArray<FVector>& Locations
    );

    // Async batch spawn point clouds
    DECLARE_DELEGATE_OneParam(FOnPointCloudBatchSpawned, const TArray<AActor*>&);
    void BatchSpawnPointCloudsAtLocations_Async(
        const TArray<FJUSYNCPointCloudData>& PointCloudDataArray,
        const TArray<FVector>& Locations,
        FOnPointCloudBatchSpawned OnBatchSpawned
    );

    // Texture Integration
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    UTexture2D* CreateUETextureFromJUSYNC(const FJUSYNCTextureData& TextureData);

    // Material Caching
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Materials")
    void PreloadCommonMaterials();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Materials")
    UMaterialInterface* GetCachedMaterial(const FString& MaterialPath);

    // Dynamic Material Creation
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Materials|Async")
    void CreateMaterialFromTexture_Async(UTexture2D* Texture, URealtimeMeshComponent* TargetComponent);

    // Async Material Creation with Return (for batch spawning)
    // Internal implementation - uses standard delegate
    void CreateMaterialFromTexture_Async_Return_Internal(
        UTexture2D* Texture,
        UMaterialInterface* BaseMaterial,
        FName TextureParameterName,
        std::function<void(UMaterialInstanceDynamic*)> OnMaterialCreated);

    // Async Mesh Processing
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh|Async")
    void CreateRealtimeMeshFromJUSYNC_Async(
        const FJUSYNCMeshData& MeshData,
        URealtimeMeshComponent* RealtimeMeshComponent);

    // Callback handlers for Blueprint Library
    UFUNCTION()
    void HandleFileReceivedForLibrary(const FJUSYNCFileData& FileData);

    UFUNCTION()
    void HandleMessageReceivedForLibrary(const FString& Message);

    // DEALER Client Functions for HPC Broker Communication
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Broker")
    bool ConnectToBroker(const FString& BrokerEndpoint = TEXT("tcp://localhost:5556"), int32 TimeoutMs = 5000);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Broker")
    void DisconnectFromBroker();

    UFUNCTION(BlueprintPure, Category = "JUSYNC Broker")
    bool IsBrokerConnected() const;

    // Sync broker functions (used internally by async wrappers)
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Broker", DisplayName = "Request File List (Sync)")
    bool RequestFileList(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Broker", DisplayName = "Request File List With Sizes (Sync)")
    bool RequestFileListWithSizes(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles, TArray<int64>& OutSizes);

    bool RequestFileListWithSizesAndRanks(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles, TArray<int64>& OutSizes, TArray<int32>& OutRanks, TArray<uint64>* OutHashLo = nullptr, TArray<uint64>* OutHashHi = nullptr);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Broker", DisplayName = "Request File (Sync)")
    bool RequestFile(const FString& Filename, int32 TargetRank, int32 TimeoutMs, TArray<uint8>& OutData);
    // In-situ variant: when ExpectedFileSize > 0 (the wire size from the file
    // list) the file is downloaded directly into OutData with no middleware
    // intermediate allocation/copy; otherwise falls back to the legacy path.
    bool RequestFileSized(const FString& Filename, int32 TargetRank, int32 TimeoutMs,
                          TArray<uint8>& OutData, uint64 ExpectedFileSize);

    // Parallel download functions
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Broker", DisplayName = "Request Files Parallel (Sync)")
    bool RequestFilesParallel(
        const TArray<FString>& Filenames,
        const TArray<int32>& TargetRanks,
        int32 TimeoutMs,
        TArray<FJUSYNCFileData>& OutFiles);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Broker", DisplayName = "Request Frame (Sync)")
    bool RequestFrame(int32 FrameNumber, int32 TargetRank, int32 TimeoutMs, TArray<FJUSYNCFileData>& OutFiles);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Broker", DisplayName = "Request Worker Status (Sync)")
    bool RequestWorkerStatus(int32 TargetRank, int32 TimeoutMs, TArray<FJUSYNCWorkerStatus>& OutWorkerStatus);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Broker", DisplayName = "Request Worker Count (Sync)")
    bool RequestWorkerCount(int32 TimeoutMs, int32& OutWorkerCount);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Broker", DisplayName = "Request Total Worker Count (Sync)")
    bool RequestTotalWorkerCount(int32 TimeoutMs, int32& OutTotalCount);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Broker", DisplayName = "Request Worker Count Excluding Rank0 (Sync)")
    bool RequestWorkerCountExcludingRank0(int32 TimeoutMs, int32& OutWorkerCount);

    // ========== PERFORMANCE METRICS FUNCTIONS ==========

    // Metrics configuration
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Metrics")
    void ConfigureMetrics(const FJUSYNCMetricsConfig& NewConfig);

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Metrics")
    FJUSYNCMetricsConfig GetMetricsConfig() const;

    // Metrics collection control
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Metrics")
    void StartMetricsCollection();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Metrics")
    void StopMetricsCollection();

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Metrics")
    bool IsMetricsCollectionActive() const;

    // Metrics data access
    UFUNCTION(BlueprintPure, Category = "JUSYNC|Metrics")
    FJUSYNCMetricsData GetCurrentMetrics() const;

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Metrics")
    TArray<FJUSYNCMetricsData> GetMetricsHistory(int32 MaxSamples = 100) const;

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Metrics")
    void ClearMetricsHistory();

    // Metrics export
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Metrics")
    bool ExportMetricsToCSV(const FString& FilePath);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Metrics")
    bool ExportMetricsToJSON(const FString& FilePath);

    // Metrics events
    UPROPERTY(BlueprintAssignable, Category = "JUSYNC|Metrics")
    FJUSYNCMetricsUpdated OnMetricsUpdated;

    // Manual metric updates (for custom tracking)
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Metrics")
    void RecordMeshSplit(int32 OriginalVertices, int32 OriginalTriangles, int32 ChunksCreated, float SplitTime_ms);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Metrics")
    void RecordMeshCreation(int32 Vertices, int32 Triangles, float CreationTime_ms);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Metrics")
    void RecordError(const FString& ErrorType);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Metrics")
    void RecordMemoryWarning(const FString& WarningType);

    // Real-time display
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Metrics")
    void ShowMetricsDisplay(bool bShow);

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Metrics")
    bool IsMetricsDisplayVisible() const;

    // Hardware monitoring
    float GetSystemRAMUsage_GB() const;
    float GetVRAMUsage_GB() const;
    float GetCPUUsage_Percent() const;
    float GetGPUUsage_Percent() const;

    // Metrics collection functions
    void CollectMetrics();

    // Benchmark-specific metrics (don't reset accumulator)
    int32 GetSplitMeshCount() const;

private:
    // Helper functions for async processing
    struct FProcessedMeshData
    {
        FString ElementName;
        TArray<FVector3f> Positions;
        TArray<FVector3f> Normals;
        TArray<FVector2DHalf> UVs;
        TArray<FColor> Colors;
        TArray<int32> Triangles;
        int32 FinalVertexCount;
        int32 FinalTriCount;
    };

    FProcessedMeshData ProcessMeshDataCPU(const FJUSYNCMeshData& MeshData);
    void ApplyProcessedMeshToComponent(const FProcessedMeshData& ProcessedData, URealtimeMeshComponent* RealtimeMeshComponent);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    TUniquePtr<anari_usd_middleware::AnariUsdMiddleware> Middleware;

    // Legacy callback handlers (wrapped for compatibility)
    void HandleFileReceived(const anari_usd_middleware::FileData& FileData);
    void HandleMessageReceived(const std::string& Message);

    // Legacy conversion helpers (wrapped for compatibility)
    FJUSYNCFileData ConvertFileData(const anari_usd_middleware::FileData& SourceData);
    FJUSYNCMeshData ConvertMeshData(const anari_usd_middleware::MeshData& SourceData);
    FJUSYNCTextureData ConvertTextureData(const anari_usd_middleware::TextureData& SourceData);
#endif

    mutable FCriticalSection MiddlewareMutex;

    // Serializes USD parsing. tinyusdz is NOT thread-safe, so parses must run one-at-a-time.
    // Deliberately SEPARATE from MiddlewareMutex: a single parse (up to ~500MB) runs on a
    // background thread, and holding the general MiddlewareMutex for its whole duration would
    // stall any game-thread state op (init/connect/worker-list) that also takes it.
    mutable FCriticalSection ParseMutex;
    std::atomic<bool> bIsInitialized{ false };

    // Material caching
    TMap<FString, TSoftObjectPtr<UMaterialInterface>> MaterialCache;
    mutable FCriticalSection MaterialCacheMutex;

    // ========== PERFORMANCE METRICS PRIVATE MEMBERS ==========

    // Metrics configuration and state
    FJUSYNCMetricsConfig MetricsConfig;
    FJUSYNCMetricsData CurrentMetrics;
    TArray<FJUSYNCMetricsData> MetricsHistory;
    mutable FCriticalSection MetricsMutex;

    // Metrics collection state
    std::atomic<bool> bMetricsCollectionActive{ false };
    FTimerHandle MetricsCollectionTimerHandle;
    float LastCollectionTime{ 0.0f };

    // Accumulated statistics for averaging
    struct FMetricsAccumulator
    {
        int32 MeshSplitCount{ 0 };
        int32 MeshCreationCount{ 0 };
        int32 ErrorCount{ 0 };
        int32 MemoryWarningCount{ 0 };
        float TotalSplitTime_ms{ 0.0f };
        float TotalCreationTime_ms{ 0.0f };
        int32 TotalSplitVertices{ 0 };
        int32 TotalSplitTriangles{ 0 };
        int32 TotalCreatedVertices{ 0 };
        int32 TotalCreatedTriangles{ 0 };
        int32 TotalChunksCreated{ 0 };

        void Reset()
        {
            MeshSplitCount = 0;
            MeshCreationCount = 0;
            ErrorCount = 0;
            MemoryWarningCount = 0;
            TotalSplitTime_ms = 0.0f;
            TotalCreationTime_ms = 0.0f;
            TotalSplitVertices = 0;
            TotalSplitTriangles = 0;
            TotalCreatedVertices = 0;
            TotalCreatedTriangles = 0;
            TotalChunksCreated = 0;
        }
    };

    FMetricsAccumulator MetricsAccumulator;

    // Metrics collection functions (implementation details)
    void UpdateMetricsData();
    void SaveMetricsToHistory();

    // Component tracking
    int32 CountRMCComponents() const;
    int32 CountActiveRMCComponents() const;
    int32 CountInstancedRMCComponents() const;

    // Display management
    bool bMetricsDisplayVisible{ false };
    TWeakObjectPtr<class UUserWidget> MetricsDisplayWidget;

    // Async point cloud spawner with actor pooling
    TUniquePtr<class FJUSYNCPointCloudSpawner> PCSpawner;

    void CreateMetricsDisplay();
    void DestroyMetricsDisplay();
    void UpdateMetricsDisplay();
};
