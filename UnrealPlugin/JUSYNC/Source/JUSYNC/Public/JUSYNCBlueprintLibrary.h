#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/Texture2D.h"
#include "JUSYNCTypes.h"
#include "JUSYNCSubsystem.h"
#include "TimerManager.h"
#include "JUSYNCBlueprintLibrary.generated.h"

// Forward declarations
class URealtimeMeshComponent;

UCLASS()
class JUSYNC_API UJUSYNCBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // ========== EVENT DISPATCHERS FOR ASYNC OPERATIONS ==========

    // Worker count async result (use non-multicast for Blueprint function parameters)
    DECLARE_DYNAMIC_DELEGATE_OneParam(FOnWorkerCountReceived, int32, WorkerCount);

    // Total worker count async result (including rank 0)
    DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTotalWorkerCountReceived, int32, TotalCount);

    // Worker status async result
    DECLARE_DYNAMIC_DELEGATE_OneParam(FOnWorkerStatusReceived, const TArray<FJUSYNCWorkerStatus>&, WorkerStatus);

    // File list async result
    DECLARE_DYNAMIC_DELEGATE_OneParam(FOnFileListReceived, const TArray<FString>&, FileList);

    // File list with sizes async result
    DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnFileListWithSizesReceived, const TArray<FString>&, FileList, const TArray<int64>&, FileSizes);

    // File list with sizes and ranks async result
    DECLARE_DYNAMIC_DELEGATE_ThreeParams(FOnFileListWithSizesAndRanksReceived, const TArray<FString>&, FileList, const TArray<int64>&, FileSizes, const TArray<int32>&, FileRanks);

    // File async result
    DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnFileReceived, const FString&, Filename, const TArray<uint8>&, FileData);

    // Parallel file download async result - one callback per file as it completes
    DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnParallelFileReceived, const FString&, Filename, const TArray<uint8>&, FileData);

    // Parallel download completion callback - when ALL files are done
    DECLARE_DYNAMIC_DELEGATE(FOnParallelDownloadComplete);

    // Parallel download error callback - per-file errors
    DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnParallelDownloadError, const FString&, Filename, const FString&, ErrorMessage);

    // Generic error event
    DECLARE_DYNAMIC_DELEGATE_OneParam(FOnBrokerError, FString, ErrorMessage);

public:
    // ========== CONNECTION MANAGEMENT ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Connection", CallInEditor)
    static bool InitializeJUSYNCMiddleware(const FString& Endpoint = TEXT(""));

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Connection", CallInEditor)
    static void ShutdownJUSYNCMiddleware();

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Connection")
    static bool IsJUSYNCConnected();

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Connection")
    static FString GetJUSYNCStatusInfo();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Connection")
    static bool StartJUSYNCReceiving();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Connection")
    static void StopJUSYNCReceiving();

    // ========== DEALER CLIENT FOR HPC BROKER ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker", DisplayName = "Connect to ANARI USD Broker")
    static bool ConnectToANARIUSDBroker(const FString& BrokerEndpoint = TEXT("tcp://localhost:5556"), int32 TimeoutMs = 5000);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker", DisplayName = "Disconnect from ANARI USD Broker")
    static void DisconnectFromANARIUSDBroker();

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Broker", DisplayName = "Is ANARI USD Broker Connected")
    static bool IsANARIUSDBrokerConnected();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker", DisplayName = "Get Last File List From Broker")
    static bool GetLastFileListFromBroker(TArray<FString>& OutFileList);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker", DisplayName = "Get Last File List With Sizes From Broker")
    static bool GetLastFileListWithSizesFromBroker(TArray<FString>& OutFileList, TArray<int64>& OutFileSizes);

    // ========== LEGACY SYNC BROKER FUNCTIONS (Use async versions instead) ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Legacy", DisplayName = "Request File List From Broker (Sync)")
    static bool RequestFileListFromBroker(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Legacy", DisplayName = "Request File List With Sizes From Broker (Sync)")
    static bool RequestFileListWithSizesFromBroker(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles, TArray<int64>& OutSizes);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Legacy", DisplayName = "Request File List With Sizes And Ranks From Broker (Sync)")
    static bool RequestFileListWithSizesAndRanksFromBroker(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles, TArray<int64>& OutSizes, TArray<int32>& OutRanks);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Legacy", DisplayName = "Request File From Broker (Sync)")
    static bool RequestFileFromBroker(const FString& Filename, int32 TargetRank, int32 TimeoutMs, TArray<uint8>& OutData);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Legacy", DisplayName = "Request Frame From Broker (Sync)")
    static bool RequestFrameFromBroker(int32 FrameNumber, int32 TargetRank, int32 TimeoutMs, TArray<FJUSYNCFileData>& OutFiles);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Legacy", DisplayName = "Request Worker Status From Broker (Sync)")
    static bool RequestWorkerStatusFromBroker(int32 TargetRank, int32 TimeoutMs, TArray<FJUSYNCWorkerStatus>& OutWorkerStatus);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Legacy", DisplayName = "Request Worker Count From Broker (Sync)")
    static bool RequestWorkerCountFromBroker(int32 TimeoutMs, int32& OutWorkerCount);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Legacy", DisplayName = "Request Total Worker Count From Broker (Sync)")
    static bool RequestTotalWorkerCountFromBroker(int32 TimeoutMs, int32& OutTotalCount);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Legacy", DisplayName = "Request Worker Count Excluding Rank0 (Sync)")
    static bool RequestWorkerCountExcludingRank0(int32 TimeoutMs, int32& OutWorkerCount);

    // ========== ASYNC WORKER QUERIES (NON-BLOCKING) ==========

    // Async total worker count - fires event when complete, doesn't block
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Async", DisplayName = "Request Total Worker Count Async")
    static void RequestTotalWorkerCountAsync(int32 TimeoutMs, const FOnTotalWorkerCountReceived& OnComplete, const FOnBrokerError& OnError);

    // Async worker count - fires event when complete, doesn't block
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Async", DisplayName = "Request Worker Count Async")
    static void RequestWorkerCountAsync(int32 TimeoutMs, const FOnWorkerCountReceived& OnComplete, const FOnBrokerError& OnError);

    // Async worker status - fires event when complete, doesn't block
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Async", DisplayName = "Request Worker Status Async")
    static void RequestWorkerStatusAsync(int32 TargetRank, int32 TimeoutMs, const FOnWorkerStatusReceived& OnComplete, const FOnBrokerError& OnError);

    // Async file list - fires event when complete, doesn't block
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Async", DisplayName = "Request File List Async")
    static void RequestFileListAsync(int32 TargetRank, int32 TimeoutMs, const FOnFileListReceived& OnComplete, const FOnBrokerError& OnError);

    // Async file list with sizes - fires event when complete, doesn't block
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Async", DisplayName = "Request File List With Sizes Async")
    static void RequestFileListWithSizesAsync(int32 TargetRank, int32 TimeoutMs, const FOnFileListWithSizesReceived& OnComplete, const FOnBrokerError& OnError);

    // Async file list with sizes and ranks - fires event when complete, doesn't block
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Async", DisplayName = "Request File List With Sizes And Ranks Async")
    static void RequestFileListWithSizesAndRanksAsync(int32 TargetRank, int32 TimeoutMs, const FOnFileListWithSizesAndRanksReceived& OnComplete, const FOnBrokerError& OnError);

    // Async file request - fires event when complete, doesn't block
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Async", DisplayName = "Request File Async")
    static void RequestFileAsync(const FString& Filename, int32 TargetRank, int32 TimeoutMs, const FOnFileReceived& OnComplete, const FOnBrokerError& OnError);

    // Async parallel file requests - downloads multiple files simultaneously
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Async", DisplayName = "Request Files Parallel Async")
    static void RequestFilesParallelAsync(
        const TArray<FString>& Filenames,
        const TArray<int32>& TargetRanks,
        int32 TimeoutMs,
        const FOnParallelFileReceived& OnFileReceived,
        const FOnParallelDownloadComplete& OnComplete,
        const FOnParallelDownloadError& OnError);

    // Legacy sync version for backward compatibility
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Legacy", DisplayName = "Request Files Parallel (Sync)")
    static bool RequestFilesParallelSync(
        const TArray<FString>& Filenames,
        const TArray<int32>& TargetRanks,
        int32 TimeoutMs,
        TArray<FJUSYNCFileData>& OutFiles);

    // ========== USD PROCESSING WITH PREVIEW ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|USD", CallInEditor)
    static bool LoadUSDFromBuffer(const TArray<uint8>& Buffer, const FString& Filename,
        TArray<FJUSYNCMeshData>& OutMeshData, FString& OutPreview);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|USD", CallInEditor, DisplayName = "Load USD Full (Mesh + Point Cloud)")
    static bool LoadUSDFullFromBuffer(const TArray<uint8>& Buffer, const FString& Filename,
        TArray<FJUSYNCMeshData>& OutMeshData, TArray<FJUSYNCPointCloudData>& OutPointCloudData, FString& OutPreview);

    /**
     * Zero-copy variant: bypasses std::vector copy at C API boundary.
     * Accepts raw TArray<uint8> data pointer directly to UsdProcessor.
     * Use this for performance-critical paths (large payloads).
     */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|USD", CallInEditor, DisplayName = "Load USD Full No Copy (Mesh + Point Cloud)")
    static bool LoadUSDFullFromBufferNoCopy(const TArray<uint8>& Buffer, const FString& Filename,
        TArray<FJUSYNCMeshData>& OutMeshData, TArray<FJUSYNCPointCloudData>& OutPointCloudData, FString& OutPreview);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|USD", CallInEditor)
    static bool LoadUSDFromDisk(const FString& FilePath,
        TArray<FJUSYNCMeshData>& OutMeshData, FString& OutPreview);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|USD", CallInEditor)
    static FString GetUSDAPreview(const TArray<uint8>& Buffer, int32 MaxLines = 10);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|USD")
    static bool ValidateUSDFormat(const TArray<uint8>& Buffer, const FString& Filename);

    // ========== POINT CLOUD PROCESSING ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|PointCloud", CallInEditor, DisplayName = "Load USD Point Cloud From Buffer")
    static bool LoadUSDPointCloudFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCPointCloudData>& OutPointCloudData);

    // Async point cloud processing delegates
    DECLARE_DYNAMIC_DELEGATE_ThreeParams(FOnAsyncPointCloudLoaded, const TArray<FJUSYNCPointCloudData>&, PCData, bool, bSuccess, const FString&, ErrorMsg);

    // ========== TEXTURE PROCESSING ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Texture", CallInEditor)
    static FJUSYNCTextureData CreateTextureFromBuffer(const TArray<uint8>& Buffer);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Texture", CallInEditor)
    static UTexture2D* CreateUETextureFromJUSYNC(const FJUSYNCTextureData& TextureData);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Texture")
    static bool WriteGradientLineAsPNG(const TArray<uint8>& Buffer, const FString& OutputPath);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Texture")
    static bool GetGradientLineAsPNGBuffer(const TArray<uint8>& Buffer, TArray<uint8>& OutPNGBuffer);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Texture")
    static bool GetPNGDimensions(const TArray<uint8>& Buffer, int32& OutWidth, int32& OutHeight, int32& OutChannels);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Texture")
    static bool GetImageRowAsPNGBuffer(const TArray<uint8>& Buffer, int32 RowIndex, TArray<uint8>& OutPNGBuffer);

    // Broadcast duplicate handling
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broadcast")
    static void ClearBroadcastDuplicates();

    // ========== REALTIMEMESH PROCESSING ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh", CallInEditor)
    static bool CreateRealtimeMeshFromJUSYNC(const FJUSYNCMeshData& MeshData,
        URealtimeMeshComponent* RealtimeMeshComponent);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh|Async", CallInEditor,
        meta = (AdvancedDisplay = "1", ToolTip = "Creates mesh using background threads for better performance"))
    static void CreateRealtimeMeshFromJUSYNC_Async(const FJUSYNCMeshData& MeshData,
        URealtimeMeshComponent* RealtimeMeshComponent);

    // ========== ASYNC MATERIAL CREATION ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Materials|Async", CallInEditor,
        meta = (ToolTip = "Creates dynamic material from texture and applies it to mesh component using background threads"))
    static void CreateMaterialFromTexture_Async(UTexture2D* Texture,
        URealtimeMeshComponent* TargetComponent);

    // Async material creation that returns the material (for batch spawning)
    DECLARE_DYNAMIC_DELEGATE_OneParam(FOnMaterialCreated, UMaterialInstanceDynamic*, CreatedMaterial);

    // Original function for backward compatibility
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh|Async", CallInEditor,
        meta = (ToolTip = "Creates dynamic material from texture and returns it via delegate for batch spawning", AutoCreateRefTerm = "OnMaterialCreated"))
    static void CreateMaterialFromTexture_Async_Return(
        UTexture2D* Texture,
        const FOnMaterialCreated& OnMaterialCreated);

    // Extended version with configurable parameters
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh|Async", CallInEditor,
        meta = (ToolTip = "Creates dynamic material from texture using specified base material and texture parameter, returns via delegate", AutoCreateRefTerm = "OnMaterialCreated", DisplayName = "Create Material From Texture Async Return (Extended)"))
    static void CreateMaterialFromTexture_Async_Return_Extended(
        UTexture2D* Texture,
        UMaterialInterface* BaseMaterial,
        FName TextureParameterName,
        const FOnMaterialCreated& OnMaterialCreated);

    // ========== ASYNC BATCH SPAWNING ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh|Async", CallInEditor,
        meta = (ToolTip = "Batch creates meshes with async processing and frame budget management"))
    static void BatchCreateRealtimeMeshesFromJUSYNC_Async(const TArray<FJUSYNCMeshData>& MeshDataArray,
        const TArray<URealtimeMeshComponent*>& MeshComponents,
        int32 MaxMeshesPerFrame = 10);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh", CallInEditor)
    static bool BatchCreateRealtimeMeshesFromJUSYNC(const TArray<FJUSYNCMeshData>& MeshDataArray,
        const TArray<URealtimeMeshComponent*>& MeshComponents);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh")
    static FJUSYNCRealtimeMeshData ConvertToRealtimeMeshFormat(const FJUSYNCMeshData& StandardMesh);

    // ========== REALTIMEMESH SPAWNING ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning", CallInEditor)
    static AActor* SpawnRealtimeMeshAtLocation(const FJUSYNCMeshData& MeshData,
        const FVector& SpawnLocation,
        const FRotator& SpawnRotation = FRotator::ZeroRotator,
        UMaterialInterface* CustomMaterial = nullptr);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning", CallInEditor)
    static AActor* SpawnRealtimeMeshAtActor(const FJUSYNCMeshData& MeshData,
        AActor* TargetActor,
        UMaterialInterface* CustomMaterial = nullptr);


    DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnBatchSpawnProgress,
        const TArray<AActor*>&, SpawnedActors, float, Progress);

    DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnBatchSpawnComplete,
        const TArray<AActor*>&, SpawnedActors, bool, bSuccess);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning", CallInEditor)
    static TArray<AActor*> BatchSpawnRealtimeMeshesAtLocations(
        const TArray<FJUSYNCMeshData>& MeshDataArray,
        const TArray<FVector>& SpawnLocations,
        const TArray<FRotator>& SpawnRotations,  // Remove default parameter here
        bool bUseAsyncSpawning = false,
        int32 BatchSize = 5,
        float BatchDelay = 0.016f
    );

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning")
    static TArray<FVector> GetSpawnPointLocations(const FString& TagFilter = TEXT("USDSpawnPoint"));

    // ========== POINT CLOUD SPAWNING ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|PointCloud Spawning", CallInEditor, DisplayName = "Spawn Point Cloud At Location")
    static AActor* SpawnPointCloudAtLocation(
        const FJUSYNCPointCloudData& PointCloudData,
        const FVector& SpawnLocation,
        const FRotator& SpawnRotation = FRotator::ZeroRotator,
        const FVector& SpawnScale = FVector(1.0f));

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|PointCloud Spawning", CallInEditor, DisplayName = "Batch Spawn Point Clouds")
    static TArray<AActor*> BatchSpawnPointClouds(
        const TArray<FJUSYNCPointCloudData>& PointCloudDataArray,
        const TArray<FVector>& SpawnLocations);

    // Async point cloud spawn delegate
    DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnPointCloudSpawnedDyn, AActor*, SpawnedActor, bool, bSuccess);
    DECLARE_DYNAMIC_DELEGATE_OneParam(FOnPointCloudBatchSpawnedDyn, const TArray<AActor*>&, SpawnedActors);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|PointCloud Spawning|Async", CallInEditor, DisplayName = "Spawn Point Cloud At Location (Async)")
    static void SpawnPointCloudAtLocation_Async(
        const FJUSYNCPointCloudData& PointCloudData,
        const FVector& SpawnLocation,
        const FRotator& SpawnRotation,
        const FVector& SpawnScale,
        const FOnPointCloudSpawnedDyn& OnSpawned);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|PointCloud Spawning|Async", CallInEditor, DisplayName = "Batch Spawn Point Clouds (Async)")
    static void BatchSpawnPointClouds_Async(
        const TArray<FJUSYNCPointCloudData>& PointCloudDataArray,
        const TArray<FVector>& SpawnLocations,
        const FOnPointCloudBatchSpawnedDyn& OnBatchSpawned);


    // ========== DATA RECEPTION ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Reception")
    static bool CheckForReceivedFiles(TArray<FJUSYNCFileData>& OutReceivedFiles);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Reception")
    static bool CheckForReceivedMessages(TArray<FString>& OutReceivedMessages);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Reception")
    static void ClearReceivedData();

    // ========== VALIDATION & UTILITIES ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Validation")
    static bool ValidateJUSYNCMeshData(const FJUSYNCMeshData& MeshData, FString& ValidationMessage);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Validation")
    static bool ValidateJUSYNCTextureData(const FJUSYNCTextureData& TextureData, FString& ValidationMessage);

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Validation")
    static int32 GetFileSize(const TArray<uint8>& FileBuffer);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Validation")
    static bool FilterFileBySize(const TArray<uint8>& FileBuffer, int32 MinimumSizeBytes);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Validation", DisplayName = "Filter File List By Size")
    static void FilterFileListBySize(const TArray<FString>& FileList, const TArray<int64>& FileSizes, int32 MinimumSizeBytes, TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Validation", DisplayName = "Filter File List By Extension (Enum)")
    static void FilterFileListByExtensionEnum(const TArray<FString>& FileList, EJUSYNCExtension ExtensionFilter, TArray<FString>& OutFilteredFiles);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Validation", DisplayName = "Filter File List By Extension (Enum) With Sizes")
    static void FilterFileListByExtensionEnumWithSizes(const TArray<FString>& FileList, const TArray<int64>& FileSizes, EJUSYNCExtension ExtensionFilter, TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Validation", DisplayName = "Filter File List By Extensions (Array)")
    static void FilterFileListByExtensions(const TArray<FString>& FileList, const TArray<FString>& AllowedExtensions, TArray<FString>& OutFilteredFiles);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Validation", DisplayName = "Filter File List By Extensions (Array) With Sizes")
    static void FilterFileListByExtensionsWithSizes(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<FString>& AllowedExtensions, TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes);

    // ========== RANK-AWARE FILTER FUNCTIONS ==========

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Validation", DisplayName = "Filter File List By Size (With Ranks)")
    static void FilterFileListBySizeWithRanks(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks, int32 MinimumSizeBytes,
        TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes, TArray<int32>& OutFilteredRanks);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Validation", DisplayName = "Filter File List By Extension (Enum) With Sizes And Ranks")
    static void FilterFileListByExtensionEnumWithSizesAndRanks(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks,
        EJUSYNCExtension ExtensionFilter, TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes, TArray<int32>& OutFilteredRanks);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Validation", DisplayName = "Filter File List By Extensions (Array) With Sizes And Ranks")
    static void FilterFileListByExtensionsWithSizesAndRanks(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks,
        const TArray<FString>& AllowedExtensions, TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes, TArray<int32>& OutFilteredRanks);

    // ========== GEOMETRY CLIP FILTER ==========
    // Strips all shared files (manifests, materials, camera, textures) and only returns per-rank geometry clips under "clips/"
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Validation", DisplayName = "Extract Geometry Clips Only")
    static void ExtractGeometryClips(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks,
        TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes, TArray<int32>& OutFilteredRanks);

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Utilities", DisplayName = "Calculate Timeout From File Size")
    static int32 CalculateTimeoutFromFileSize(int64 FileSizeBytes, int32 BaseTimeoutMs = 1000, float BandwidthBytesPerSecond = 1000000.0f);

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Utilities")
    static FString GetJUSYNCMeshStatistics(const FJUSYNCMeshData& MeshData);

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Utilities")
    static FString GetJUSYNCTextureStatistics(const FJUSYNCTextureData& TextureData);

    // ========== FILE OPERATIONS ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|File", CallInEditor)
    static bool LoadFileToBuffer(const FString& FilePath, TArray<uint8>& OutBuffer);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|File", CallInEditor)
    static bool SaveBufferToFile(const TArray<uint8>& Buffer, const FString& FilePath);

    // ========== DEBUG & DISPLAY ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Debug", CallInEditor)
    static void DisplayDebugMessage(const FString& Message, float Duration = 5.0f,
        FLinearColor Color = FLinearColor::Green);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Debug", CallInEditor)
    static void LogJUSYNCMessage(const FString& Message, bool bIsError = false);

    // ========== HELPER FUNCTIONS ==========
    UFUNCTION(BlueprintPure, Category = "JUSYNC|Helpers")
    static UJUSYNCSubsystem* GetJUSYNCSubsystem();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Utilities")
    static FRotator ConvertParaViewToUERotation(const FRotator& ParaViewRotation);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Utilities")
    static TArray<FVector> GenerateDefaultLocations(int32 Count, const FVector& BaseLocation = FVector::ZeroVector, float Spacing = 200.0f);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Utilities")
    static TArray<FRotator> GenerateDefaultRotations(int32 Count, const FRotator& BaseRotation = FRotator::ZeroRotator);

    // ========== Async ===============
    // Add this declaration in the public section
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning|Legacy", DisplayName = "Batch Spawn Realtime Meshes At Locations (Sync)")
    static TArray<AActor*> BatchSpawnRealtimeMeshesAtLocationsSync(
        const TArray<FJUSYNCMeshData>& MeshDataArray,
        const TArray<FVector>& SpawnLocations,
        const TArray<FRotator>& SpawnRotations  // Remove default parameter here
    );

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning", CallInEditor)
    static TArray<AActor*> BatchSpawnRealtimeMeshesWithMaterial(
        const TArray<FJUSYNCMeshData>& MeshDataArray, const TArray<FVector>& SpawnLocations,
        const TArray<FRotator>& SpawnRotations, UMaterialInterface* Material,
        bool bUseUniformScaling = false, FVector OuterBoundingBoxSize = FVector::ZeroVector,
        bool bPreserveAspectRatio = true, bool bUseAsyncSpawning = false, int32 BatchSize = 5,
        float BatchDelay = 0.016f
    );

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning", CallInEditor)
    static AActor* SpawnRealtimeMeshWithMaterial(
        const FJUSYNCMeshData& MeshData, const FVector& SpawnLocation,
        const FRotator& SpawnRotation, UMaterialInterface* Material,
        bool bUseUniformScaling = false, FVector OuterBoundingBoxSize = FVector::ZeroVector,
        bool bPreserveAspectRatio = true, bool bUseAsyncSpawning = false
    );

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning|Benchmarked", CallInEditor)
    static AActor* SpawnRealtimeMeshWithMaterial_Benchmarked(
        const FJUSYNCMeshData& MeshData, const FVector& SpawnLocation,
        const FRotator& SpawnRotation, UMaterialInterface* Material,
        const FJUSYNCBenchmarkConfig& Config,
        bool bUseUniformScaling = false, FVector OuterBoundingBoxSize = FVector::ZeroVector,
        bool bPreserveAspectRatio = true, bool bUseAsyncSpawning = true
    );


    static FBox CalculateMeshBounds(const FJUSYNCMeshData& MeshData, const FVector& Location);

    static FJUSYNCMeshData FixMeshDataForSpawning(const FJUSYNCMeshData& InputMeshData);

    static TArray<FVector> CalculateScaledPositions(
        const TArray<FVector>& OriginalLocations,
        const FVector& BoundingBoxSize,
        bool bPreserveAspectRatio,
        FVector& OutScaleFactor
    );

    // Internal storage for received data (public for subsystem access)
    static TArray<FJUSYNCFileData> ReceivedFiles;
    static TArray<FString> ReceivedMessages;
    static FCriticalSection DataMutex;

    // Last file list retrieved from broker (for async node output)
    static TArray<FString> LastFileList;
    static FCriticalSection LastFileListMutex;

    // Last file list with sizes retrieved from broker (for async node output)
    static TArray<FString> LastFileListWithSizes_Names;
    static TArray<int64> LastFileListWithSizes_Sizes;
    static FCriticalSection LastFileListWithSizesMutex;

    static void ApplyEnhancedDefaultMaterial(URealtimeMeshComponent* MeshComp);
    static FString DetectUSDContentType(const TArray<uint8>& Buffer);

private:
    // Internal helper functions
    static bool ValidateBufferSize(const TArray<uint8>& Buffer, const FString& Context);
    static bool ValidateFilePath(const FString& FilePath, const FString& Context);
    static FString ExtractUSDAPreview(const TArray<uint8>& Buffer, int32 MaxLines);

    // Rank extraction helper
    UFUNCTION(BlueprintPure, Category = "JUSYNC|Utilities")
    static int32 ExtractRankFromFilename(const FString& Filename);

    static void AsyncBatchSpawnInternal(
        const TArray<FJUSYNCMeshData>& MeshDataArray,
        const TArray<FVector>& SpawnLocations,
        const TArray<FRotator>& SpawnRotations,
        TSharedPtr<TArray<AActor*>> SharedResults,
        int32 CurrentBatch,
        int32 BatchSize,
        float BatchDelay
    );

    // ========== BENCHMARKING FUNCTIONS ==========

    /**
     * Start benchmarking for a specific test
     */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Benchmarking")
    static void StartBenchmark(const FString& TestName, const FJUSYNCBenchmarkConfig& Config);

    /**
     * End benchmarking and save results
     */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Benchmarking")
    static void EndBenchmark();

    /**
     * Save all benchmark results to CSV
     */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Benchmarking")
    static void SaveAllBenchmarkResultsToCSV(const FString& OutputDirectory);

    /**
     * Save all benchmark results to JSON (more readable format)
     */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Benchmarking")
    static void SaveAllBenchmarkResultsToJSON(const FString& OutputDirectory);

    /**
     * Clear all benchmark results
     */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Benchmarking")
    static void ClearBenchmarkResults();

    /**
     * Get current benchmark results
     */
    UFUNCTION(BlueprintPure, Category = "JUSYNC|Benchmarking")
    static TArray<FJUSYNCBenchmarkResult> GetBenchmarkResults();

    /**
     * Batch spawn with benchmarking (wraps BatchSpawnRealtimeMeshesWithMaterial)
     */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning|Benchmarked", CallInEditor)
    static TArray<AActor*> BatchSpawnRealtimeMeshesWithMaterial_Benchmarked(
        const TArray<FJUSYNCMeshData>& MeshDataArray,
        const TArray<FVector>& SpawnLocations,
        const TArray<FRotator>& SpawnRotations,
        UMaterialInterface* Material,
        const FJUSYNCBenchmarkConfig& Config,
        bool bUseUniformScaling = false,
        FVector OuterBoundingBoxSize = FVector::ZeroVector,
        bool bPreserveAspectRatio = true,
        bool bUseAsyncSpawning = false,
        int32 BatchSize = 5,
        float BatchDelay = 0.016f
    );

    // ========== DYNAMIC TIMEOUT & RETRY LOGIC ==========

    /**
     * Request file with dynamic timeout and retry logic
     */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker|Async", DisplayName = "Request File Async (Dynamic Timeout)")
    static void RequestFileAsyncDynamic(
        const FString& Filename,
        int32 TargetRank,
        const FOnFileReceived& OnComplete,
        const FOnBrokerError& OnError,
        int32 MaxRetries = 3,
        bool bUseExtractedRank = true);

    /**
     * Calculate dynamic timeout based on file size, rank performance, and retry count
     */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Utilities")
    static int32 CalculateDynamicTimeout(
        const FString& Filename,
        int32 TargetRank,
        bool bIsRetry = false,
        int32 RetryCount = 0);

    /**
     * Estimate file size from filename patterns
     */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Utilities")
    static int64 EstimateFileSizeFromFilename(const FString& Filename);

    /**
     * Get fallback ranks for a given target rank
     */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Utilities")
    static TArray<int32> GetFallbackRanks(int32 TargetRank);

    /**
     * Clear rank performance statistics
     */
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Utilities")
    static void ClearRankPerformanceStats();

    /**
     * Get rank performance statistics as string
     */
    UFUNCTION(BlueprintPure, Category = "JUSYNC|Utilities")
    static FString GetRankPerformanceStats();

private:
    // Rank performance tracking
    class FRankPerformanceTracker
    {
    private:
        struct FRankStats
        {
            FDateTime LastRequestTime;
            float AverageResponseTime = 0.0f;
            int32 RequestCount = 0;
            int32 SuccessCount = 0;
            float SuccessRate = 1.0f;
        };

        TMap<int32, FRankStats> RankStats;
        mutable FCriticalSection StatsMutex;

    public:
        float GetRankPerformanceFactor(int32 Rank);
        bool CanTryRank(int32 Rank);
        void RecordRequestStart(int32 Rank);
        void RecordRequestResult(int32 Rank, bool bSuccess, int32 ResponseTimeMs);
        void ClearStats();
        FString GetStatsAsString() const;
    };

    static FRankPerformanceTracker RankPerformanceTracker;

    // Benchmark data storage
    static TArray<FJUSYNCBenchmarkResult> BenchmarkResults;
    static FString CurrentBenchmarkTest;
    static FJUSYNCBenchmarkConfig CurrentBenchmarkConfig;
    static bool bIsBenchmarking;
    static FDateTime BenchmarkSessionStartTime;

    // Benchmark helper functions
    static void RecordBenchmarkResult(const FJUSYNCBenchmarkResult& Result);
    static FJUSYNCBenchmarkResult CreateBenchmarkResult(const FString& TestName, float TotalTimeMs, int32 TriangleCount, int32 VertexCount, int64 RAMBefore, int64 RAMAfter, int32 ActorCount, int32 ErrorCount, int32 SplitMeshCount);
    static FJUSYNCBenchmarkResult CreateBenchmarkResultExtended(
        const FString& TestName,
        float TotalTimeMs,
        int32 TriangleCount,
        int32 VertexCount,
        int64 RAMBefore,
        int64 RAMAfter,
        int64 RAMPeak,
        int64 RAMDuring,
        int32 ActorCount,
        int32 ErrorCount,
        int32 SplitMeshCount,
        float CPUUsagePercent,
        int64 VRAMBefore,
        int64 VRAMAfter,
        int64 VRAMPeak,
        int32 ActiveThreadCount,
        float GPUUsagePercent,
        int32 HitchCount = 0,
        float AvgHitchDurationMs = 0.0f,
        float MaxHitchDurationMs = 0.0f);
};
