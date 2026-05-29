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

    /** Full path to .usda file to load from disk */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config", meta = (FilePathFilter = "*.usda,*.usd,*.usdc,*.usdz"))
    FString UsdFilePath;

    /** Spawn location for extracted geometry (relative to this actor's location) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config")
    FVector SpawnOffset;

    /** If true, also try to extract and spawn meshes alongside point clouds */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config")
    bool bSpawnMeshes;

    /** If true, also try to extract and spawn point clouds */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config")
    bool bSpawnPointClouds;

    /** Scale for spawned actors */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Test|Config")
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
    TArray<AActor*> SpawnedActors;
};
