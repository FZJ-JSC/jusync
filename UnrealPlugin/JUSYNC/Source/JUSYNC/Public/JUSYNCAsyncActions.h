#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "JUSYNCTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"           
#include "Engine/GameInstance.h"
#include "TimerManager.h"  
#include "JUSYNCAsyncActions.generated.h"

// Delegate declarations for async outputs
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJUSYNCAsyncUSDLoadComplete, const TArray<FJUSYNCMeshData>&, MeshData, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJUSYNCAsyncFileReceiveComplete, const FJUSYNCFileData&, FileData, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJUSYNCAsyncProcessingProgress, float, Progress);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJUSYNCAsyncTextureComplete, const FJUSYNCTextureData&, TextureData, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJUSYNCAsyncMeshComplete, URealtimeMeshComponent*, MeshComponent, bool, bSuccess);

// ========== ASYNC USD LOADING NODE ==========
UCLASS()
class JUSYNC_API UJUSYNCAsyncLoadUSD : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    // Output pins for Blueprint
    UPROPERTY(BlueprintAssignable)
    FJUSYNCAsyncUSDLoadComplete OnSuccess;
    
    UPROPERTY(BlueprintAssignable)
    FJUSYNCAsyncUSDLoadComplete OnFailure;
    
    UPROPERTY(BlueprintAssignable)
    FJUSYNCAsyncProcessingProgress OnProgress;

    // Factory function - this creates the Blueprint node
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Async", 
              meta = (BlueprintInternalUseOnly = "true", CallInEditor = "true"))
    static UJUSYNCAsyncLoadUSD* AsyncLoadUSDFromBuffer(
        const TArray<uint8>& Buffer, 
        const FString& Filename);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Async", 
              meta = (BlueprintInternalUseOnly = "true", CallInEditor = "true"))
    static UJUSYNCAsyncLoadUSD* AsyncLoadUSDFromDisk(const FString& FilePath);

    // Activation function
    virtual void Activate() override;

private:
    TArray<uint8> BufferData;
    FString FilenameData;
    FString FilePathData;
    bool bIsFromDisk;
    
    void OnLoadComplete(bool bSuccess, const TArray<FJUSYNCMeshData>& MeshData);
    void BroadcastProgress(float Progress);
};

// ========== ASYNC FILE RECEPTION NODE ==========
UCLASS()
class JUSYNC_API UJUSYNCAsyncReceiveFiles : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable)
    FJUSYNCAsyncFileReceiveComplete OnFileReceived;

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Async", 
              meta = (BlueprintInternalUseOnly = "true"))
    static UJUSYNCAsyncReceiveFiles* AsyncStartReceiving();

    virtual void Activate() override;

private:
    FTimerHandle CheckTimer;
    void CheckForFiles();
    UWorld* GetWorld() const;
};

// ========== ASYNC TEXTURE PROCESSING NODE ==========
UCLASS()
class JUSYNC_API UJUSYNCAsyncCreateTexture : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable)
    FJUSYNCAsyncTextureComplete OnSuccess;
    
    UPROPERTY(BlueprintAssignable)
    FJUSYNCAsyncTextureComplete OnFailure;

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Async", 
              meta = (BlueprintInternalUseOnly = "true"))
    static UJUSYNCAsyncCreateTexture* AsyncCreateTextureFromBuffer(const TArray<uint8>& Buffer);

    virtual void Activate() override;

private:
    TArray<uint8> BufferData;
    void OnTextureComplete(bool bSuccess, const FJUSYNCTextureData& TextureData);
};

// ========== ASYNC REALTIMEMESH CREATION NODE ==========
UCLASS()
class JUSYNC_API UJUSYNCAsyncCreateMesh : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable)
    FJUSYNCAsyncMeshComplete OnSuccess;
    
    UPROPERTY(BlueprintAssignable)
    FJUSYNCAsyncMeshComplete OnFailure;

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Async", 
              meta = (BlueprintInternalUseOnly = "true"))
    static UJUSYNCAsyncCreateMesh* AsyncCreateRealtimeMeshFromJUSYNC(
        const FJUSYNCMeshData& MeshData, 
        URealtimeMeshComponent* RealtimeMeshComponent);

    virtual void Activate() override;

private:
    FJUSYNCMeshData MeshDataCopy;
    TWeakObjectPtr<URealtimeMeshComponent> MeshComponentPtr;
    void OnMeshComplete(bool bSuccess, URealtimeMeshComponent* Component);
};
