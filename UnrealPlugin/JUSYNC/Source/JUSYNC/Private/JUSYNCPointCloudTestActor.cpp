#include "JUSYNCPointCloudTestActor.h"
#include "Engine/Engine.h"
#include "Misc/FileHelper.h"

AJUSYNCPointCloudTestActor::AJUSYNCPointCloudTestActor()
{
    PrimaryActorTick.bCanEverTick = false;

    UsdFilePath = TEXT("");
    SpawnOffset = FVector::ZeroVector;
    bSpawnMeshes = true;
    bSpawnPointClouds = true;
    SpawnScaleFactor = 1.0f;
    bAutoLoadOnBegin = true;
}

void AJUSYNCPointCloudTestActor::BeginPlay()
{
    Super::BeginPlay();

    // Initialize middleware first
    if (!UJUSYNCBlueprintLibrary::InitializeJUSYNCMiddleware(TEXT("")))
    {
        UE_LOG(LogTemp, Error, TEXT("[TestActor] Failed to initialize JUSYNC middleware!"));
        GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("[TestActor] Middleware init failed"));
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("[TestActor] Middleware initialized successfully"));

    if (bAutoLoadOnBegin && !UsdFilePath.IsEmpty())
    {
        LoadAndSpawnFromDisk();
    }
}

void AJUSYNCPointCloudTestActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ClearSpawnedActors();
    Super::EndPlay(EndPlayReason);
}

void AJUSYNCPointCloudTestActor::LoadAndSpawnFromDisk()
{
    if (UsdFilePath.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("[TestActor] UsdFilePath is empty!"));
        GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("[TestActor] No USD file path set"));
        return;
    }

    if (!FPaths::FileExists(UsdFilePath))
    {
        UE_LOG(LogTemp, Error, TEXT("[TestActor] File not found: %s"), *UsdFilePath);
        GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red,
            FString::Printf(TEXT("[TestActor] File not found: %s"), *UsdFilePath));
        return;
    }

    // Read file to buffer
    TArray<uint8> FileBuffer;
    if (!FFileHelper::LoadFileToArray(FileBuffer, *UsdFilePath))
    {
        UE_LOG(LogTemp, Error, TEXT("[TestActor] Failed to read file: %s"), *UsdFilePath);
        GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red,
            FString::Printf(TEXT("[TestActor] Failed to read file")));
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("[TestActor] Loaded file: %s (%d bytes)"), *UsdFilePath, FileBuffer.Num());
    GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Cyan,
        FString::Printf(TEXT("[TestActor] Loaded: %s (%d bytes)"), *FPaths::GetCleanFilename(UsdFilePath), FileBuffer.Num()));

    FString Filename = FPaths::GetCleanFilename(UsdFilePath);
    FVector SpawnLoc = GetActorLocation() + SpawnOffset;

    int32 MeshSpawned = 0;
    int32 PCSpawned = 0;

    // Try meshes
    if (bSpawnMeshes)
    {
        TArray<FJUSYNCMeshData> MeshData;
        FString Preview;
        if (UJUSYNCBlueprintLibrary::LoadUSDFromBuffer(FileBuffer, Filename, MeshData, Preview))
        {
            UE_LOG(LogTemp, Log, TEXT("[TestActor] Extracted %d meshes"), MeshData.Num());
            GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green,
                FString::Printf(TEXT("[TestActor] Meshes found: %d"), MeshData.Num()));

            for (const FJUSYNCMeshData& M : MeshData)
            {
                if (!M.IsValid()) continue;

                if (M.Vertices.Num() == 0 || M.Triangles.Num() == 0) continue;

                AActor* Spawned = UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(M, SpawnLoc);
                if (Spawned)
                {
                    SpawnedActors.Add(Spawned);
                    MeshSpawned++;

                    if (SpawnScaleFactor > 0.0f && SpawnScaleFactor != 1.0f)
                    {
                        Spawned->SetActorScale3D(FVector(SpawnScaleFactor));
                    }

                    UE_LOG(LogTemp, Display, TEXT("[TestActor] Spawned mesh: %s (verts=%d, tris=%d)"),
                        *M.ElementName, M.Vertices.Num(), M.Triangles.Num() / 3);

                    SpawnLoc += FVector(100.0f, 0.0f, 0.0f);
                }
            }
        }
    }

    // Try point clouds
    if (bSpawnPointClouds)
    {
        TArray<FJUSYNCPointCloudData> PCData;
        if (UJUSYNCBlueprintLibrary::LoadUSDPointCloudFromBuffer(FileBuffer, Filename, PCData))
        {
            UE_LOG(LogTemp, Log, TEXT("[TestActor] Extracted %d point clouds"), PCData.Num());
            GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Blue,
                FString::Printf(TEXT("[TestActor] Point clouds found: %d"), PCData.Num()));

            for (const FJUSYNCPointCloudData& PC : PCData)
            {
                if (!PC.IsValid()) continue;

                UE_LOG(LogTemp, Display, TEXT("[TestActor] PointCloud: %s (points=%d, colors=%d, normals=%d)"),
                    *PC.ElementName, PC.PointCount, PC.HasColors() ? 1 : 0, PC.HasNormals() ? 1 : 0);

                if (PC.HasColors())
                {
                    UE_LOG(LogTemp, Log, TEXT("[TestActor]   BoundingBox: (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f)"),
                        PC.BoundingBoxMin.X, PC.BoundingBoxMin.Y, PC.BoundingBoxMin.Z,
                        PC.BoundingBoxMax.X, PC.BoundingBoxMax.Y, PC.BoundingBoxMax.Z);
                }

                AActor* Spawned = UJUSYNCBlueprintLibrary::SpawnPointCloudAtLocation(PC, SpawnLoc, FRotator::ZeroRotator, FVector(1.0f));
                if (Spawned)
                {
                    SpawnedActors.Add(Spawned);
                    PCSpawned++;

                    if (SpawnScaleFactor > 0.0f && SpawnScaleFactor != 1.0f)
                    {
                        Spawned->SetActorScale3D(FVector(SpawnScaleFactor));
                    }

                    UE_LOG(LogTemp, Display, TEXT("[TestActor] Spawned point cloud: %s (%d points, colors=%d)"),
                        *PC.ElementName, PC.PointCount, PC.HasColors() ? 1 : 0);

                    SpawnLoc += FVector(100.0f, 100.0f, 0.0f);
                }
                else
                {
                    UE_LOG(LogTemp, Warning, TEXT("[TestActor] FAILED to spawn point cloud: %s"), *PC.ElementName);
                }
            }
        }
    }

    FString Summary = FString::Printf(TEXT("[TestActor] DONE: meshes=%d, PCs=%d"), MeshSpawned, PCSpawned);
    UE_LOG(LogTemp, Display, TEXT("%s"), *Summary);
    GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor(0, 255, 128), Summary);
}

void AJUSYNCPointCloudTestActor::ClearSpawnedActors()
{
    for (AActor* Actor : SpawnedActors)
    {
        if (Actor)
        {
            Actor->Destroy();
        }
    }
    SpawnedActors.Empty();
}
