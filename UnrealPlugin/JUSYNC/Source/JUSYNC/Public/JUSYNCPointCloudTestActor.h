#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "JUSYNCSubsystem.h"
#include "JUSYNCBlueprintLibrary.h"
#include "JUSYNCPointCloudTestActor.generated.h"

UCLASS(Blueprintable, BlueprintType, Category = "JUSYNC|Test")
class JUSYNC_API AJUSYNCPointCloudTestActor : public AActor
{
    GENERATED_BODY()

public:
    AJUSYNCPointCloudTestActor();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Files", meta = (FilePathFilter = "*.usda,*.usd,*.usdc,*.usdz"))
    FString UsdFilePath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Files", meta = (FilePathFilter = "*.usda,*.usd,*.usdc,*.usdz"))
    TArray<FString> UsdFilePaths;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Instances", meta = (ClampMin = "1", UIMin = "1"))
    int32 SpawnCount;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Instances")
    float SpawnGap;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Instances", meta = (ClampMin = "1", UIMin = "1"))
    int32 GridColumns;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Instances")
    float GridRowGap;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Material")
    UMaterialInterface* SpawnMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Material")
    bool bApplyGradientToPCs;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Material", meta = (FilePathFilter = "*.png,*.jpg,*.jpeg,*.bmp,*.tga"))
    FString TexturePngFilePath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Material")
    FString TextureParameterName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config")
    FVector SpawnOffset;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config")
    bool bSpawnMeshes;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config")
    bool bSpawnPointClouds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config", meta = (ClampMin = "0.01", UIMin = "0.01"))
    float SpawnScaleFactor;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config")
    bool bAutoLoadOnBegin;

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Test")
    void LoadAndSpawnFromDisk();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Test")
    void ClearSpawnedActors();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

 private:
    void SpawnFromParsedData(const FString& Filename, bool bParsed, TArray<FJUSYNCMeshData>&& MeshData, TArray<FJUSYNCPointCloudData>&& PointCloudData, FVector BaseSpawnLoc, TArray<uint8>& PngBuffer);
    UTexture2D* LoadPngAsTexture(const TArray<uint8>& PngData);
    void ApplyTextureToMesh(AActor* Spawned, UTexture2D* Tex);
    TArray<FColor> BuildGradientLUTFromPng(const TArray<uint8>& PngData);

    TArray<AActor*> SpawnedActors;
    TArray<UTexture2D*> LoadedTextures;
};
