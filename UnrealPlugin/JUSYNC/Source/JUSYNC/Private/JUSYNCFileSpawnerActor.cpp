#include "JUSYNCFileSpawnerActor.h"
#include "JUSYNCSubsystem.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/PrimitiveComponent.h"

AJUSYNCFileSpawnerActor::AJUSYNCFileSpawnerActor()
{
    PrimaryActorTick.bCanEverTick = false;
    BrokerEndpoint = TEXT("tcp://localhost:5556");
    RequestTimeoutMs = 5000;
    BandwidthBytesPerSecond = 1000000.0f;
    MinimumFileSizeBytes = 1000;
    bFilterUSDOnly = true;
    bClipsOnly = true;
    SpawnTargetActor = nullptr;
    BaseSpawnLocation = FVector::ZeroVector;
    SpawnSpacing = 200.0f;
    SpawnMaterial = nullptr;
    TextureSampleParameterName = TEXT("");
    SpawnScale = FVector::OneVector;
    bUseUniformScaling = true;
    bAutoStart = true;
    MaxRetries = 2;
    CurrentRetryCount = 0;
    CurrentState = EJUSYNCSpawnerState::Idle;
    FilesDownloaded = 0;
    FilesTotal = 0;
    ActorsSpawned = 0;
    NextSpawnIndex = 0;
    PendingDownloads = 0;
    bIsCancelled = false;
}

void AJUSYNCFileSpawnerActor::BeginPlay()
{
    Super::BeginPlay();
    if (bAutoStart)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: auto-starting pipeline"));
        StartSpawning();
    }
}

void AJUSYNCFileSpawnerActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    CancelSpawning();
    ClearSpawnedActors();
    Super::EndPlay(EndPlayReason);
}

void AJUSYNCFileSpawnerActor::StartSpawning()
{
    if (CurrentState != EJUSYNCSpawnerState::Idle && CurrentState != EJUSYNCSpawnerState::Complete && CurrentState != EJUSYNCSpawnerState::Error)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: already running in state %d"), (int32)CurrentState);
        return;
    }

    ClearSpawnedActors();
    RawFileList.Empty(); RawFileSizes.Empty(); RawFileRanks.Empty();
    FilteredFiles.Empty(); FilteredSizes.Empty(); FilteredRanks.Empty();
    FilesDownloaded = 0; FilesTotal = 0; ActorsSpawned = 0;
    FailedFileIndices.Empty();
    NextSpawnIndex = 0; PendingDownloads = 0; bIsCancelled = false;
    CurrentRetryCount = 0;
    CurrentState = EJUSYNCSpawnerState::Connecting;
    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: starting pipeline"));
    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, FString::Printf(TEXT("[Spawner] Connecting to %s"), *BrokerEndpoint));
    ConnectToBroker();
}

void AJUSYNCFileSpawnerActor::CancelSpawning()
{
    bIsCancelled = true;
    CurrentState = EJUSYNCSpawnerState::Idle;
    PendingDownloads = 0;
}

void AJUSYNCFileSpawnerActor::ClearSpawnedActors()
{
    for (AActor* Actor : SpawnedActors)
        if (Actor) Actor->Destroy();
    SpawnedActors.Empty();
    ActorsSpawned = 0;
}

FVector AJUSYNCFileSpawnerActor::GetNextSpawnLocation() const
{
    FVector Origin = BaseSpawnLocation;
    if (SpawnTargetActor && SpawnTargetActor->IsValidLowLevel() && !SpawnTargetActor->HasAnyFlags(RF_ClassDefaultObject))
        Origin = SpawnTargetActor->GetActorLocation();
    else if (GetWorld() && !HasAnyFlags(RF_ClassDefaultObject))
        Origin = GetActorLocation();

    int32 Col = NextSpawnIndex % 10;
    int32 Row = NextSpawnIndex / 10;
    return Origin + FVector(Col * SpawnSpacing, Row * SpawnSpacing, 0.0f);
}

int32 AJUSYNCFileSpawnerActor::CalculateDynamicTimeout(int64 FileSizeBytes) const
{
    const int32 BaseTimeout = 5000;
    const int32 MaxTimeout = 120000;
    float TimeForTransfer = (static_cast<float>(FileSizeBytes) / FMath::Max(1.0f, BandwidthBytesPerSecond)) * 1000.0f;
    int32 DynamicTimeout = BaseTimeout + static_cast<int32>(TimeForTransfer);
    return FMath::Clamp(DynamicTimeout, RequestTimeoutMs, MaxTimeout);
}

void AJUSYNCFileSpawnerActor::ConnectToBroker()
{
    if (bIsCancelled) return;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        CurrentState = EJUSYNCSpawnerState::Error;
        OnError.Broadcast(TEXT("Subsystem not available"));
        return;
    }

    bool bInitialized = Subsystem->InitializeMiddleware();
    if (!bInitialized)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: middleware initialization failed"));
        GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Red, TEXT("[Spawner] Middleware init failed."));
        CurrentState = EJUSYNCSpawnerState::Error;
        OnError.Broadcast(TEXT("Middleware initialization failed"));
        return;
    }

    bool bConnected = Subsystem->ConnectToBroker(BrokerEndpoint, 5000);
    if (!bConnected)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: failed to connect to broker at %s"), *BrokerEndpoint);
        GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Red, TEXT("[Spawner] Failed to connect to broker!"));
        CurrentState = EJUSYNCSpawnerState::Error;
        OnError.Broadcast(TEXT("Failed to connect to broker"));
        return;
    }

    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: connected, requesting file list"));
    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, TEXT("[Spawner] Connected, fetching file list..."));
    CurrentState = EJUSYNCSpawnerState::FetchingList;
    RequestFileList();
}

void AJUSYNCFileSpawnerActor::RequestFileList()
{
    if (bIsCancelled) return;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        OnFileListError(TEXT("Subsystem not available"));
        return;
    }

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;

    Async(EAsyncExecution::Thread, [WeakSubsystem, WeakThis]()
        {
            if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

            TArray<FString> Files;
            TArray<int64> Sizes;
            TArray<int32> Ranks;
            bool bSuccess = WeakSubsystem->RequestFileListWithSizesAndRanks(-1, 15000, Files, Sizes, Ranks);

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [WeakThis, Files, Sizes, Ranks, bSuccess]()
                {
                    if (WeakThis.IsValid())
                        WeakThis->OnFileListReceived_Internal(Files, Sizes, Ranks, bSuccess);
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

void AJUSYNCFileSpawnerActor::OnFileListReceived_Internal(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks, bool bSuccess)
{
    if (bIsCancelled) return;

    if (!bSuccess || FileList.Num() == 0)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: failed to retrieve file list from broker"));
        GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Red, TEXT("[Spawner] Failed to get file list from broker!"));
        CurrentState = EJUSYNCSpawnerState::Error;
        OnError.Broadcast(TEXT("Failed to retrieve file list"));
        return;
    }

    RawFileList = FileList;
    RawFileSizes = FileSizes;
    RawFileRanks = FileRanks;

    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: received %d files"), FileList.Num());
    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, FString::Printf(TEXT("[Spawner] Got %d files, filtering..."), FileList.Num()));

    TArray<FString> OutFiles;
    TArray<int64> OutSizes;
    TArray<int32> OutRanks;

    if (bFilterUSDOnly)
    {
        UJUSYNCBlueprintLibrary::FilterFileListByExtensionEnumWithSizesAndRanks(RawFileList, RawFileSizes, RawFileRanks, EJUSYNCExtension::USD, OutFiles, OutSizes, OutRanks);
        RawFileList = MoveTemp(OutFiles); RawFileSizes = MoveTemp(OutSizes); RawFileRanks = MoveTemp(OutRanks);
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: after USD filter: %d files"), RawFileList.Num());
    }

    if (MinimumFileSizeBytes > 0)
    {
        OutFiles.Empty(); OutSizes.Empty(); OutRanks.Empty();
        UJUSYNCBlueprintLibrary::FilterFileListBySizeWithRanks(RawFileList, RawFileSizes, RawFileRanks, MinimumFileSizeBytes, OutFiles, OutSizes, OutRanks);
        RawFileList = MoveTemp(OutFiles); RawFileSizes = MoveTemp(OutSizes); RawFileRanks = MoveTemp(OutRanks);
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: after size filter: %d files"), RawFileList.Num());
    }

    if (bClipsOnly)
    {
        OutFiles.Empty(); OutSizes.Empty(); OutRanks.Empty();
        UJUSYNCBlueprintLibrary::ExtractGeometryClips(RawFileList, RawFileSizes, RawFileRanks, OutFiles, OutSizes, OutRanks);
        RawFileList = MoveTemp(OutFiles); RawFileSizes = MoveTemp(OutSizes); RawFileRanks = MoveTemp(OutRanks);
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: after clips filter: %d files"), RawFileList.Num());
    }

    FilteredFiles = RawFileList;
    FilteredSizes = RawFileSizes;
    FilteredRanks = RawFileRanks;
    FilesTotal = FilteredFiles.Num();

    if (FilteredFiles.Num() == 0)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: no files after filtering"));
        GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Yellow, TEXT("[Spawner] No files after filtering! Check filters."));
        CurrentState = EJUSYNCSpawnerState::Complete;
        OnAllComplete.Broadcast(0, false);
        return;
    }

    CurrentState = EJUSYNCSpawnerState::Downloading;
    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: downloading %d files"), FilesTotal);
    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, FString::Printf(TEXT("[Spawner] Downloading %d geometry clips..."), FilesTotal));
    ProcessAndDownloadFiles();
}

void AJUSYNCFileSpawnerActor::OnFileListReceived(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks)
{
    OnFileListReceived_Internal(FileList, FileSizes, FileRanks, true);
}

void AJUSYNCFileSpawnerActor::OnFileListError(const FString& ErrorMessage)
{
    UE_LOG(LogTemp, Error, TEXT("JUSYNC Spawner: file list error: %s"), *ErrorMessage);
    CurrentState = EJUSYNCSpawnerState::Error;
    OnError.Broadcast(ErrorMessage);
}

void AJUSYNCFileSpawnerActor::ProcessAndDownloadFiles()
{
    if (bIsCancelled || FilteredFiles.Num() == 0) return;

    PendingDownloads = FilteredFiles.Num();

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Spawner: subsystem not available"));
        return;
    }

    for (int32 i = 0; i < FilteredFiles.Num(); ++i)
    {
        if (bIsCancelled) break;

        const FString& Filename = FilteredFiles[i];
        int32 TargetRank = FilteredRanks[i];
        int64 FileSize = FilteredSizes.IsValidIndex(i) ? FilteredSizes[i] : int64(1048576);
        int32 DynamicTimeout = CalculateDynamicTimeout(FileSize);

        TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
        TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
        int32 FileIndex = i;

        Async(EAsyncExecution::Thread, [WeakSubsystem, WeakThis, Filename, TargetRank, DynamicTimeout, FileIndex]()
            {
                if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

                TArray<uint8> FileData;
                bool bSuccess = WeakSubsystem->RequestFile(Filename, TargetRank, DynamicTimeout, FileData);

                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [WeakThis, Filename, FileData, bSuccess, FileIndex]()
                    {
                        if (!WeakThis.IsValid()) return;
                        WeakThis->OnSingleFileDownloaded(Filename, FileData, bSuccess, FileIndex);
                    },
                    TStatId(), nullptr, ENamedThreads::GameThread);
            });
    }
}

void AJUSYNCFileSpawnerActor::OnSingleFileDownloaded(const FString& Filename, const TArray<uint8>& FileData, bool bSuccess, int32 FileIndex)
{
    if (bIsCancelled) return;

    if (!bSuccess || FileData.Num() == 0)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: [DOWNLOAD FAILED] '%s' (rank %d)"), *Filename, FilteredRanks.IsValidIndex(FileIndex) ? FilteredRanks[FileIndex] : -1);
        if (!FailedFileIndices.Contains(FileIndex))
            FailedFileIndices.Add(FileIndex);
        FilesDownloaded++;
        OnFileProgress.Broadcast(FilesDownloaded, FilesTotal);
        return;
    }

    FilesDownloaded++;
    OnFileProgress.Broadcast(FilesDownloaded, FilesTotal);
    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: downloaded %s (%d/%d)"), *Filename, FilesDownloaded, FilesTotal);

    NextSpawnIndex = FileIndex;
    SpawnMeshFromData(Filename, FileData);
}

void AJUSYNCFileSpawnerActor::OnFileDownloaded(const FString& Filename, const TArray<uint8>& FileData)
{
    OnSingleFileDownloaded(Filename, FileData, true, FilesDownloaded);
}

void AJUSYNCFileSpawnerActor::OnFileDownloadError(const FString& ErrorMessage)
{
    UE_LOG(LogTemp, Error, TEXT("JUSYNC Spawner: download error: %s"), *ErrorMessage);
    OnError.Broadcast(ErrorMessage);
}

void AJUSYNCFileSpawnerActor::SpawnMeshFromData(const FString& Filename, const TArray<uint8>& FileData)
{
    if (bIsCancelled) return;

    TArray<FJUSYNCMeshData> MeshData;
    FString Preview;
    bool bParsed = UJUSYNCBlueprintLibrary::LoadUSDFromBuffer(FileData, Filename, MeshData, Preview);

    if (!bParsed)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: [PARSE FAILED] '%s'"), *Filename);
        GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow, FString::Printf(TEXT("[Spawner] PARSE FAILED: %s"), *Filename));
        CheckAllDownloadsComplete();
        return;
    }

    if (MeshData.Num() == 0)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: [NO MESHES] '%s' parsed but returned 0 meshes"), *Filename);
        GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow, FString::Printf(TEXT("[Spawner] NO MESHES: %s"), *Filename));
        CheckAllDownloadsComplete();
        return;
    }

    int32 ValidMeshCount = 0;
    for (const FJUSYNCMeshData& m : MeshData) if (m.IsValid()) ValidMeshCount++;

    if (ValidMeshCount == 0)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: [INVALID MESHES] '%s' has %d meshes but none are valid (0 verts/tris)"), *Filename, MeshData.Num());
        GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow, FString::Printf(TEXT("[Spawner] INVALID MESHES: %s (0 verts/tris)"), *Filename));
        CheckAllDownloadsComplete();
        return;
    }

    for (const FJUSYNCMeshData& Mesh : MeshData)
    {
        if (!Mesh.IsValid()) continue;

        FVector SpawnLoc = GetNextSpawnLocation();
        AActor* Spawned = UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(Mesh, SpawnLoc);

        if (Spawned)
        {
            SpawnedActors.Add(Spawned);
            ActorsSpawned++;

            if (SpawnScale != FVector::ZeroVector)
            {
                FVector FinalScale = bUseUniformScaling ? FVector(SpawnScale.X) : SpawnScale;
                Spawned->SetActorScale3D(FinalScale);
            }

            ApplyDynamicMaterial(Cast<UPrimitiveComponent>(Spawned->GetRootComponent()), Filename);

            OnFileComplete.Broadcast(Filename, Spawned);
            NextSpawnIndex++;

            UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: spawned actor from '%s' (actor #%d)"), *Filename, ActorsSpawned);
        }
        else
        {
            UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: [SPAWN FAILED] mesh '%s' from '%s'"), *Mesh.ElementName, *Filename);
        }
    }

    CheckAllDownloadsComplete();
}

void AJUSYNCFileSpawnerActor::ApplyDynamicMaterial(UPrimitiveComponent* Comp, const FString& Filename)
{
    if (!Comp || !SpawnMaterial) return;

    UMaterialInstanceDynamic* DynamicMat = UMaterialInstanceDynamic::Create(SpawnMaterial, this);
    if (!DynamicMat) return;

    Comp->SetMaterial(0, DynamicMat);
    UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: created dynamic material instance for '%s'"), *Filename);
}

void AJUSYNCFileSpawnerActor::CheckAllDownloadsComplete()
{
    if (FilesDownloaded >= FilesTotal)
    {
        // Retry failed downloads if we have retries left
        if (FailedFileIndices.Num() > 0 && CurrentRetryCount < MaxRetries)
        {
            UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: %d files failed, retrying (%d/%d)..."), FailedFileIndices.Num(), CurrentRetryCount + 1, MaxRetries);
            GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, FString::Printf(TEXT("[Spawner] Retrying %d failed files (attempt %d/%d)..."), FailedFileIndices.Num(), CurrentRetryCount + 1, MaxRetries));
            CurrentRetryCount++;
            FilesDownloaded = 0;
            RetryFailedDownloads();
            return;
        }

        CurrentState = EJUSYNCSpawnerState::Complete;
        bool bSuccess = ActorsSpawned > 0;
        int32 FailedCount = FilesTotal - ActorsSpawned;
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: pipeline complete. Spawned %d actors from %d files (%d not spawned)"), ActorsSpawned, FilesTotal, FailedCount);
        FColor StatusColor = bSuccess ? FColor::Green : FColor::Yellow;
        GEngine->AddOnScreenDebugMessage(-1, 10.0f, StatusColor,
            FString::Printf(TEXT("[Spawner] DONE: spawned %d actors from %d files (%d not spawned)."), ActorsSpawned, FilesTotal, FailedCount));

        // Log failed files
        for (int32 idx : FailedFileIndices)
        {
            if (FilteredFiles.IsValidIndex(idx))
            {
                UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: FAILED FILE: %s (rank %d)"),
                    *FilteredFiles[idx], FilteredRanks.IsValidIndex(idx) ? FilteredRanks[idx] : -1);
            }
        }

        FailedFileIndices.Empty();
        OnAllComplete.Broadcast(ActorsSpawned, bSuccess);
    }
}

void AJUSYNCFileSpawnerActor::RetryFailedDownloads()
{
    if (FailedFileIndices.Num() == 0)
    {
        CheckAllDownloadsComplete();
        return;
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem) return;

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    for (int32 idx : FailedFileIndices)
    {
        if (bIsCancelled || !FilteredFiles.IsValidIndex(idx)) continue;

        const FString& Filename = FilteredFiles[idx];
        int32 TargetRank = FilteredRanks[idx];
        int32 DynamicTimeout = CalculateDynamicTimeout(FilteredSizes.IsValidIndex(idx) ? FilteredSizes[idx] : int64(2097152));
        DynamicTimeout = FMath::Min(DynamicTimeout * 2, 120000); // Double timeout on retry

        TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
        Async(EAsyncExecution::Thread, [WeakSubsystem, WeakThis, Filename, TargetRank, DynamicTimeout, idx]()
            {
                if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

                TArray<uint8> FileData;
                bool bSuccess = WeakSubsystem->RequestFile(Filename, TargetRank, DynamicTimeout, FileData);

                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [WeakThis, Filename, FileData, bSuccess, idx]()
                    {
                        if (!WeakThis.IsValid()) return;
                        WeakThis->OnSingleFileDownloaded(Filename, FileData, bSuccess, idx);
                    },
                    TStatId(), nullptr, ENamedThreads::GameThread);
            });
    }
}
