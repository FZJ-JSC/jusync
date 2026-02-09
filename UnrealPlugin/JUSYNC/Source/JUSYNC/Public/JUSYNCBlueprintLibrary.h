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

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker", DisplayName = "Request File List From Broker")
    static bool RequestFileListFromBroker(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker")
    static bool RequestFileFromBroker(const FString& Filename, int32 TargetRank, int32 TimeoutMs, TArray<uint8>& OutData);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker")
    static bool RequestFrameFromBroker(int32 FrameNumber, int32 TargetRank, int32 TimeoutMs, TArray<FJUSYNCFileData>& OutFiles);

    // ========== WORKER STATUS QUERIES ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker")
    static bool RequestWorkerStatusFromBroker(int32 TargetRank, int32 TimeoutMs, TArray<FJUSYNCWorkerStatus>& OutWorkerStatus);
    
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker")
    static bool RequestWorkerCountFromBroker(int32 TimeoutMs, int32& OutWorkerCount);

    // Total worker count INCLUDING rank 0 (uses legacy string protocol)
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker")
    static bool RequestTotalWorkerCountFromBroker(int32 TimeoutMs, int32& OutTotalCount);

    // Worker count EXCLUDING rank 0 (computational workers only)
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Broker")
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

    // ========== USD PROCESSING WITH PREVIEW ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|USD", CallInEditor)
    static bool LoadUSDFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, 
                                  TArray<FJUSYNCMeshData>& OutMeshData, FString& OutPreview);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|USD", CallInEditor)
    static bool LoadUSDFromDisk(const FString& FilePath, 
                                TArray<FJUSYNCMeshData>& OutMeshData, FString& OutPreview);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|USD", CallInEditor)
    static FString GetUSDAPreview(const TArray<uint8>& Buffer, int32 MaxLines = 10);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|USD")
    static bool ValidateUSDFormat(const TArray<uint8>& Buffer, const FString& Filename);

    // ========== TEXTURE PROCESSING ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Texture", CallInEditor)
    static FJUSYNCTextureData CreateTextureFromBuffer(const TArray<uint8>& Buffer);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Texture", CallInEditor)
    static UTexture2D* CreateUETextureFromJUSYNC(const FJUSYNCTextureData& TextureData);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Texture")
    static bool WriteGradientLineAsPNG(const TArray<uint8>& Buffer, const FString& OutputPath);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Texture")
    static bool GetGradientLineAsPNGBuffer(const TArray<uint8>& Buffer, TArray<uint8>& OutPNGBuffer);

    // ========== REALTIMEMESH PROCESSING ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh", CallInEditor)
    static bool CreateRealtimeMeshFromJUSYNC(const FJUSYNCMeshData& MeshData, 
                                             URealtimeMeshComponent* RealtimeMeshComponent);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh", CallInEditor)
    static bool BatchCreateRealtimeMeshesFromJUSYNC(const TArray<FJUSYNCMeshData>& MeshDataArray, 
                                                    const TArray<URealtimeMeshComponent*>& MeshComponents);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh")
    static FJUSYNCRealtimeMeshData ConvertToRealtimeMeshFormat(const FJUSYNCMeshData& StandardMesh);

    // ========== REALTIMEMESH SPAWNING ==========
    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning", CallInEditor)
    static AActor* SpawnRealtimeMeshAtLocation(const FJUSYNCMeshData& MeshData, 
                                             const FVector& SpawnLocation, 
                                             const FRotator& SpawnRotation = FRotator::ZeroRotator);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning", CallInEditor)
    static AActor* SpawnRealtimeMeshAtActor(const FJUSYNCMeshData& MeshData, 
                                           AActor* TargetActor);


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
	static TArray<FRotator> GenerateDefaultRotations(int32 Count, const FRotator& BaseRotation = FRotator::ZeroRotator);

	// ========== Async ===============
	// Add this declaration in the public section
	UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning")
	static TArray<AActor*> BatchSpawnRealtimeMeshesAtLocationsSync(
		const TArray<FJUSYNCMeshData>& MeshDataArray,
		const TArray<FVector>& SpawnLocations,
		const TArray<FRotator>& SpawnRotations  // Remove default parameter here
	);

UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning", CallInEditor)
        static TArray<AActor*> BatchSpawnRealtimeMeshesWithMaterial(
            const TArray<FJUSYNCMeshData>& MeshDataArray, const TArray<FVector>& SpawnLocations,
            const TArray<FRotator>& SpawnRotations, UMaterialInterface* Material,
            const TArray<uint8>& USDBuffer,  // ADD THIS LINE
            bool bUseUniformScaling = false, FVector OuterBoundingBoxSize = FVector::ZeroVector,
            bool bPreserveAspectRatio = true, bool bUseAsyncSpawning = false, int32 BatchSize = 5,
            float BatchDelay = 0.016f
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

    static void ApplyEnhancedDefaultMaterial(URealtimeMeshComponent* MeshComp);
    static FString DetectUSDContentType(const TArray<uint8>& Buffer);

private:
    // Internal helper functions
    static bool ValidateBufferSize(const TArray<uint8>& Buffer, const FString& Context);
    static bool ValidateFilePath(const FString& FilePath, const FString& Context);
    static FString ExtractUSDAPreview(const TArray<uint8>& Buffer, int32 MaxLines);

	static void AsyncBatchSpawnInternal(
			const TArray<FJUSYNCMeshData>& MeshDataArray,
			const TArray<FVector>& SpawnLocations,
			const TArray<FRotator>& SpawnRotations,
			TSharedPtr<TArray<AActor*>> SharedResults,
			int32 CurrentBatch,
			int32 BatchSize,
			float BatchDelay
		);
};
