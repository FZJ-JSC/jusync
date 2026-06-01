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

    // -------- File Selection --------
    /** Single USD file path (legacy, ignored if UsdFilePaths array is populated) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Files", meta = (FilePathFilter = "*.usda,*.usd,*.usdc,*.usdz"))
    FString UsdFilePath;

    /** Multiple USD file paths — overrides UsdFilePath when non-empty */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Files", meta = (FilePathFilter = "*.usda,*.usd,*.usdc,*.usdz"))
    TArray<FString> UsdFilePaths;

    // -------- Instance & Layout --------
    /** Number of times to spawn each parsed file (1 = single spawn) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Instances", meta = (ClampMin = "1", UIMin = "1"))
    int32 SpawnCount;

    /** Gap between spawned copies of the same file (cm) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Instances")
    float SpawnGap;

    /** Number of columns in grid layout before wrapping to next row */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Instances", meta = (ClampMin = "1", UIMin = "1"))
    int32 GridColumns;

    /** Gap between rows (cm) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Instances")
    float GridRowGap;

    // -------- Material & Point Cloud Color --------
    /** Base material to assign to spawned meshes. If left blank, default vertex color material is used. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Material")
    UMaterialInterface* SpawnMaterial;

    /** If true, interpret TexturePngFilePath as a 256×N gradient strip and apply to point clouds via Gradient LUT. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Material")
    bool bApplyGradientToPCs;

    /** PNG file to load and apply as "BaseColor" override on SpawnMaterial. Overrides whatever is in the base material. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Material", meta = (FilePathFilter = "*.png,*.jpg,*.jpeg,*.bmp,*.tga"))
    FString TexturePngFilePath;

    /** Material parameter name for PNG texture on SpawnMaterial. Default: "BaseColor" */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Material")
    FString TextureParameterName;

    // -------- Spawn Options --------
    /** Spawn location offset from this actor */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config")
    FVector SpawnOffset;

    /** If true, extract and spawn meshes alongside point clouds */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config")
    bool bSpawnMeshes;

    /** If true, extract and spawn point clouds */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config")
    bool bSpawnPointClouds;

    /** Scale for spawned actors */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config", meta = (ClampMin = "0.01", UIMin = "0.01"))
    float SpawnScaleFactor;

    /** Automatically load and spawn on BeginPlay */
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
