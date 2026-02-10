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

#ifdef WITH_ANARI_USD_MIDDLEWARE
#include "AnariUsdMiddleware.h"
#endif

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

    // USD Processing (Legacy - use JUSYNCBlueprintLibrary versions for preview support)
    UFUNCTION(BlueprintCallable, Category = "JUSYNC USD|Legacy", DisplayName = "Load USD From Buffer (Legacy)")
    bool LoadUSDFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCMeshData>& OutMeshData);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC USD|Legacy", DisplayName = "Load USD From Disk (Legacy)")
    bool LoadUSDFromDisk(const FString& FilePath, TArray<FJUSYNCMeshData>& OutMeshData);

    // Texture Processing
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    FJUSYNCTextureData CreateTextureFromBuffer(const TArray<uint8>& Buffer);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    bool WriteGradientLineAsPNG(const TArray<uint8>& Buffer, const FString& OutputPath);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    bool GetGradientLineAsPNGBuffer(const TArray<uint8>& Buffer, TArray<uint8>& OutPNGBuffer);

    // RealtimeMesh Integration
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh|Legacy", DisplayName = "Create Realtime Mesh From JUSYNC (Legacy)")
    bool CreateRealtimeMeshFromJUSYNC(
        const FJUSYNCMeshData& MeshData, 
        URealtimeMeshComponent* RealtimeMeshComponent
    );
    
    
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh|Legacy", DisplayName = "Batch Create Realtime Meshes From JUSYNC (Legacy)")
    bool BatchCreateRealtimeMeshesFromJUSYNC(const TArray<FJUSYNCMeshData>& MeshDataArray, const TArray<URealtimeMeshComponent*>& MeshComponents);

    // Conversion utilities for RealtimeMesh
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh")
    FJUSYNCRealtimeMeshData ConvertToRealtimeMeshFormat(const FJUSYNCMeshData& StandardMesh);

    // Texture Integration
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    UTexture2D* CreateUETextureFromJUSYNC(const FJUSYNCTextureData& TextureData);

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

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Broker", DisplayName = "Request File (Sync)")
    bool RequestFile(const FString& Filename, int32 TargetRank, int32 TimeoutMs, TArray<uint8>& OutData);

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
      

private:
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
    std::atomic<bool> bIsInitialized{false};
};
