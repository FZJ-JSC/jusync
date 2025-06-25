#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "JUSYNCTypes.h"
#include "RealtimeMeshComponent.h"
#include "RealtimeMeshSimple.h"
#include "Mesh/RealtimeMeshBasicShapeTools.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"

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

    // USD Processing
    UFUNCTION(BlueprintCallable, Category = "JUSYNC USD")
    bool LoadUSDFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCMeshData>& OutMeshData);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC USD")
    bool LoadUSDFromDisk(const FString& FilePath, TArray<FJUSYNCMeshData>& OutMeshData);

    // Texture Processing
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    FJUSYNCTextureData CreateTextureFromBuffer(const TArray<uint8>& Buffer);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    bool WriteGradientLineAsPNG(const TArray<uint8>& Buffer, const FString& OutputPath);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Texture")
    bool GetGradientLineAsPNGBuffer(const TArray<uint8>& Buffer, TArray<uint8>& OutPNGBuffer);

    // RealtimeMesh Integration
    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh")
    bool CreateRealtimeMeshFromJUSYNC(const FJUSYNCMeshData& MeshData, URealtimeMeshComponent* RealtimeMeshComponent);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC Mesh")
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

private:
#ifdef WITH_ANARI_USD_MIDDLEWARE
    TUniquePtr<anari_usd_middleware::AnariUsdMiddleware> Middleware;
#endif

    mutable FCriticalSection MiddlewareMutex;
    std::atomic<bool> bIsInitialized{ false };

    // Legacy callback handlers (kept for compatibility)
    void HandleFileReceived(const anari_usd_middleware::AnariUsdMiddleware::FileData& FileData);
    void HandleMessageReceived(const std::string& Message);

    // Legacy conversion helpers (kept for compatibility)
    FJUSYNCFileData ConvertFileData(const anari_usd_middleware::AnariUsdMiddleware::FileData& SourceData);
    FJUSYNCMeshData ConvertMeshData(const anari_usd_middleware::AnariUsdMiddleware::MeshData& SourceData);
    FJUSYNCTextureData ConvertTextureData(const anari_usd_middleware::AnariUsdMiddleware::TextureData& SourceData);
};
