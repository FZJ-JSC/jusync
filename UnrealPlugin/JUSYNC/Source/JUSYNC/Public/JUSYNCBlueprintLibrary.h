#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/Texture2D.h"
#include "JUSYNCTypes.h"
#include "JUSYNCSubsystem.h"
#include "JUSYNCBlueprintLibrary.generated.h"

// Forward declarations
class URealtimeMeshComponent;

UCLASS()
class JUSYNC_API UJUSYNCBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

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

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|RealtimeMesh Spawning", CallInEditor)
    static TArray<AActor*> BatchSpawnRealtimeMeshesAtLocations(const TArray<FJUSYNCMeshData>& MeshDataArray,
                                                              const TArray<FVector>& SpawnLocations);

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
	

    // Internal storage for received data (public for subsystem access)
    static TArray<FJUSYNCFileData> ReceivedFiles;
    static TArray<FString> ReceivedMessages;
    static FCriticalSection DataMutex;

private:
    // Internal helper functions
    static bool ValidateBufferSize(const TArray<uint8>& Buffer, const FString& Context);
    static bool ValidateFilePath(const FString& FilePath, const FString& Context);
    static FString ExtractUSDAPreview(const TArray<uint8>& Buffer, int32 MaxLines);
};
