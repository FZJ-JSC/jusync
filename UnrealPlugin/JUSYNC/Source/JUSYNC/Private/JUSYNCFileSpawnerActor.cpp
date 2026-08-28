#include "JUSYNCFileSpawnerActor.h"
#include <atomic>
#include <mutex>
#include "JUSYNCSubsystem.h"
#include "JUSYNCPointCloudSpawner.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Kismet/GameplayStatics.h"
#include "Templates/UniquePtr.h"

AJUSYNCFileSpawnerActor::AJUSYNCFileSpawnerActor()
{
    PrimaryActorTick.bCanEverTick = false;
    BrokerEndpoint = TEXT("tcp://localhost:5556");
    RequestTimeoutMs = 5000;
    BandwidthBytesPerSecond = 10737418240.0f; // 10GB/s - max bandwidth, no artificial throttling
    MinimumFileSizeBytes = 1000;
    bFilterUSDOnly = true;
    bClipsOnly = true;
    SpawnTargetActor = nullptr;
    BaseSpawnLocation = FVector::ZeroVector;
    SpawnSpacing = 0.0f;
    SpawnGridColumns = 10;
    SpawnMaterial = nullptr;
    TextureSampleParameterName = TEXT("");
    SpawnScale = FVector::OneVector;
    bUseUniformScaling = true;
    bAutoStart = true;
    bEnableLiveUpdates = true;
    LiveUpdatePollInterval = 3.0f;
    bAutoRefreshMeshes = true;
    LastCommitCompleteTime = 0.0;
    CommitCompleteCooldown = 8.0;
    bCommitDiffInProgress = false;
    bInitialSpawnDone = false;
    bSpawnPointClouds = true;
    bUseGradientColors = true;
    GradientPngFilename = TEXT("");
    PointCloudSize = 1.0f;
    bGradientReady = false;
    bGradientAttempted = false;
    PipelineDepth = 10;
    MaxRetries = 2;
    CurrentRetryCount = 0;
    CurrentState = EJUSYNCSpawnerState::Idle;
    FilesDownloaded = 0;
    FilesTotal = 0;
    ActorsSpawned = 0;
    NextSpawnIndex = 0;
    PendingDownloads = 0;
    PendingAsyncSpawns = 0;
    PendingAsyncPCS = 0;
    PipelineNextIndex = 0;
    PipelineActive = 0;
    bIsCancelled = false;
    RefreshActive = 0;
    V2ActiveDownloads = 0;
    MaxSpawnsPerFrame = 3;
}

void AJUSYNCFileSpawnerActor::BeginPlay()
{
    Super::BeginPlay();

    // Bind to broker notification events for live updates
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->OnNotificationReceived.AddDynamic(this, &AJUSYNCFileSpawnerActor::OnBrokerNotification);
    }

    if (bAutoStart)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: auto-starting pipeline"));
        StartSpawning();
    }
}

void AJUSYNCFileSpawnerActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopLiveUpdatePolling();

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->OnNotificationReceived.RemoveAll(this);
    }

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
    GradientPngRankMap.Empty();
    PendingPointClouds.Empty();
    ParseFailedIndices.Empty();
    bGradientReady = false;
    FilesDownloaded = 0; FilesTotal = 0; ActorsSpawned = 0;
    FailedFileIndices.Empty();
    NextSpawnIndex = 0; PendingDownloads = 0; PendingAsyncSpawns = 0; bIsCancelled = false;
    CurrentRetryCount = 0;
    CurrentState = EJUSYNCSpawnerState::Connecting;
    bInitialSpawnDone = false;
    RefreshedFiles.Empty();
    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: starting pipeline"));
    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, FString::Printf(TEXT("[Spawner] Connecting to %s"), *BrokerEndpoint));

    // Bind PC spawn handler once (before downloads start)
    {
        UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
        if (Subsystem && Subsystem->GetPointCloudSpawner())
        {
            FJUSYNCPointCloudSpawner* Spawner = Subsystem->GetPointCloudSpawner();
            Spawner->OnPointCloudSpawned.Clear();
            Spawner->OnPointCloudSpawned.AddUObject(this, &AJUSYNCFileSpawnerActor::OnPointCloudSpawnedHandler);
        }
    }

    ConnectToBroker();
}

void AJUSYNCFileSpawnerActor::CancelSpawning()
{
    bIsCancelled = true;
    PendingAsyncSpawns = 0;
    CurrentState = EJUSYNCSpawnerState::Idle;
    PendingDownloads = 0;
}

void AJUSYNCFileSpawnerActor::ClearSpawnedActors()
{
    // Clear processed files tracking to avoid silently dropping files on next cycle
    {
        UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
        if (Subsystem) Subsystem->ClearProcessedFiles();
    }

    // Destroy pooled point cloud actors (not just hide them)
    {
        UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
        if (Subsystem && Subsystem->GetPointCloudSpawner())
        {
            Subsystem->GetPointCloudSpawner()->DestroyAllActors();
        }
    }

    for (AActor* Actor : SpawnedActors)
        if (Actor && Actor->IsValidLowLevel()) Actor->Destroy();
    SpawnedActors.Empty();
    ActorsSpawned = 0;
    FileToActorMap.Empty();
    FilenameToActors.Empty();
    FileLastSize.Empty();
}

FVector AJUSYNCFileSpawnerActor::GetNextSpawnLocation() const
{
    FVector Origin = BaseSpawnLocation;
    if (SpawnTargetActor && SpawnTargetActor->IsValidLowLevel() && !SpawnTargetActor->HasAnyFlags(RF_ClassDefaultObject))
        Origin = SpawnTargetActor->GetActorLocation();
    else if (GetWorld() && !HasAnyFlags(RF_ClassDefaultObject))
        Origin = GetActorLocation();

    int32 Col = NextSpawnIndex % SpawnGridColumns;
    int32 Row = NextSpawnIndex / SpawnGridColumns;
    return Origin + FVector(Col * SpawnSpacing, Row * SpawnSpacing, 0.0f);
}

int32 AJUSYNCFileSpawnerActor::CalculateDynamicTimeout(int64 FileSizeBytes) const
{
    const int32 BaseTimeout = 5000;
    const int32 MaxTimeout = 600000; // 10 minutes max - allow large transfers
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

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis]()
        {
            if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

            TArray<FString> Files;
            TArray<int64> Sizes;
            TArray<int32> Ranks;
            TArray<uint64> HashLo;
            TArray<uint64> HashHi;
            bool bSuccess = WeakSubsystem->RequestFileListWithSizesAndRanks(-1, 15000, Files, Sizes, Ranks, &HashLo, &HashHi);

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [WeakThis, Files, Sizes, Ranks, HashLo, HashHi, bSuccess]()
                {
                    if (WeakThis.IsValid())
                    {
                        WeakThis->RawHashLo = HashLo;
                        WeakThis->RawHashHi = HashHi;
                        WeakThis->OnFileListReceived_Internal(Files, Sizes, Ranks, bSuccess);
                    }
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

    // Single-pass categorization: bucket files by type, apply all filters in one pass
    TArray<FString> PngFiles;
    TArray<int32> PngRanks;
    TArray<FString> LocalFiles;
    TArray<int64> LocalSizes;
    TArray<int32> LocalRanks;
    TArray<uint64> LocalHashLo;
    TArray<uint64> LocalHashHi;
    LocalFiles.Reserve(FileList.Num());
    LocalSizes.Reserve(FileList.Num());
    LocalRanks.Reserve(FileList.Num());
    LocalHashLo.Reserve(FileList.Num());
    LocalHashHi.Reserve(FileList.Num());

    for (int32 i = 0; i < FileList.Num(); ++i)
    {
        const FString& Fname = FileList[i];
        int64 Fsize = FileSizes.IsValidIndex(i) ? FileSizes[i] : 0;
        int32 Frank = FileRanks.IsValidIndex(i) ? FileRanks[i] : 0;
        uint64 FhLo = RawHashLo.IsValidIndex(i) ? RawHashLo[i] : 0;
        uint64 FhHi = RawHashHi.IsValidIndex(i) ? RawHashHi[i] : 0;

        if (Fname.EndsWith(TEXT(".png")) || Fname.EndsWith(TEXT(".PNG")))
        {
            PngFiles.Add(Fname);
            PngRanks.Add(Frank);
            continue;
        }

        if (bFilterUSDOnly)
        {
            FString Ext = FPaths::GetExtension(Fname).ToLower();
            if (Ext != TEXT("usd") && Ext != TEXT("usda") && Ext != TEXT("usdc") && Ext != TEXT("usdz"))
                continue;
        }

        if (MinimumFileSizeBytes > 0 && Fsize < MinimumFileSizeBytes)
            continue;

        if (bClipsOnly && !Fname.StartsWith(TEXT("clips/")))
            continue;

        LocalFiles.Add(Fname);
        LocalSizes.Add(Fsize);
        LocalRanks.Add(Frank);
        LocalHashLo.Add(FhLo);
        LocalHashHi.Add(FhHi);
    }

    for (int32 i = 0; i < PngFiles.Num(); ++i)
    {
        GradientPngRankMap.Add(PngFiles[i], PngRanks[i]);
    }

    RawFileList = LocalFiles;
    RawFileSizes = LocalSizes;
    RawFileRanks = LocalRanks;
    RawHashLo = LocalHashLo;
    RawHashHi = LocalHashHi;
    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: single-pass filter: %d geometry files, %d PNGs"), RawFileList.Num(), PngFiles.Num());

    FilteredFiles = RawFileList;
    FilteredSizes = RawFileSizes;
    FilteredRanks = RawFileRanks;
    FilteredHashLo = RawHashLo;
    FilteredHashHi = RawHashHi;
    FilesTotal = FilteredFiles.Num();

    if (FilteredFiles.Num() == 0)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: no files after filtering"));
        GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, TEXT("[Spawner] No files after filtering! Check filters."));
        bInitialSpawnDone = true;
        CurrentState = EJUSYNCSpawnerState::Complete;
        OnAllComplete.Broadcast(0, false);
        return;
    }

    // Gradient LUT will be built from middleware cache right after first PC parse
    // Also try to download PNG gradient from broker first
    if (GradientPngRankMap.Num() > 0)
    {
        UJUSYNCSubsystem* GrdSubsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
        if (GrdSubsystem)
        {
            DownloadGradientPng(GrdSubsystem);
        }
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

    // Pipelined download: start with PipelineDepth files, then chain next download after each completes
    PipelineNextIndex = 0;
    PipelineActive = 0;

    // Start initial batch of downloads (up to PipelineDepth)
    int32 InitialBatch = FMath::Min(PipelineDepth, FilteredFiles.Num());
    for (int32 i = 0; i < InitialBatch; ++i)
    {
        PipelineDownloadNext(Subsystem);
    }
}

void AJUSYNCFileSpawnerActor::PipelineDownloadNext(UJUSYNCSubsystem* Subsystem)
{
    if (!Subsystem || bIsCancelled) return;
    if (PipelineNextIndex >= FilteredFiles.Num()) return;

    int32 i = PipelineNextIndex++;
    const FString& Filename = FilteredFiles[i];
    int32 TargetRank = FilteredRanks[i];
    int64 FileSize = FilteredSizes.IsValidIndex(i) ? FilteredSizes[i] : int64(1048576);
    int32 DynamicTimeout = CalculateDynamicTimeout(FileSize);

    PipelineActive++;
    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    int32 FileIndex = i;
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis, Filename, TargetRank, DynamicTimeout, FileIndex, FileSize]()
        {
            if (!WeakSubsystem.IsValid() || !WeakThis.IsValid())
            {
                return;
            }

            TUniquePtr<TArray<uint8>> FileData = MakeUnique<TArray<uint8>>();
            // In-situ: download straight into FileData (wire size is known).
            bool bSuccess = WeakSubsystem->RequestFileSized(Filename, TargetRank, DynamicTimeout, *FileData, static_cast<uint64>(FileSize));

            TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThisCopy = WeakThis;
            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [WeakThisCopy, Filename, FileData = MoveTemp(FileData), bSuccess, FileIndex, TargetRank]() mutable
                {
                    if (!WeakThisCopy.IsValid()) return;
                    WeakThisCopy->OnSingleFileDownloaded(Filename, MoveTemp(FileData), bSuccess, FileIndex, TargetRank);
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

void AJUSYNCFileSpawnerActor::OnSingleFileDownloaded(const FString& Filename, TUniquePtr<TArray<uint8>> FileData, bool bSuccess, int32 FileIndex, int32 TargetRank)
{
    if (bIsCancelled) return;

    PipelineActive = FMath::Max(0, PipelineActive - 1);

    if (!bSuccess || !FileData || FileData->Num() == 0)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: [DOWNLOAD FAILED] '%s' (rank %d)"), *Filename, FilteredRanks.IsValidIndex(FileIndex) ? FilteredRanks[FileIndex] : -1);
        if (!FailedFileIndices.Contains(FileIndex))
            FailedFileIndices.Add(FileIndex);
        if (!bInitialSpawnDone)
        {
            FilesDownloaded++;
            OnFileProgress.Broadcast(FilesDownloaded, FilesTotal);
        }
        if (RefreshRemainingFiles.Num() > 0) ChainRefreshNext();
        else if (PipelineNextIndex < FilteredFiles.Num()) {
            UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
            if (S)
                PipelineDownloadNext(S);
        }
        if (!bInitialSpawnDone) CheckAllDownloadsComplete();
        return;
    }

    //     Quick bookkeeping on game thread, then dispatch heavy parse to background
    // Only track download progress during initial spawn — during live refresh this counter pollutes
    // CheckAllDownloadsComplete which resets ActorsSpawned=0, FilesTotal=0 and re-triggers completion.
    if (!bInitialSpawnDone)
    {
        FilesDownloaded++;
        OnFileProgress.Broadcast(FilesDownloaded, FilesTotal);
    }
    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: downloaded %s (%s %d/%d)"),
           *Filename, bInitialSpawnDone ? TEXT("refresh") : TEXT("initial"),
           FilesDownloaded, FilesTotal);
    NextSpawnIndex = FileIndex;

    // Move USD parse (heavy) to background thread — game thread stays responsive
    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis, Filename, FileData = MoveTemp(FileData), FileIndex, TargetRank]() mutable
        {
            if (!WeakThis.IsValid()) return;

            TArray<FJUSYNCMeshData> MeshData;
            TArray<FJUSYNCPointCloudData> PointCloudData;
            FString Preview;
            bool bParsed = UJUSYNCBlueprintLibrary::LoadUSDFullFromBufferNoCopy(*FileData, Filename, MeshData, PointCloudData, Preview);

            //                     // During live refresh, skip initial-spawn bookkeeping (FilesDownloaded, FilesTotal reset,
                    // OnAllComplete, StartLiveUpdatePolling) — those would corrupt the actor count and restart the pipeline in a loop.
                    bool bIsLiveRefresh = WeakThis->bInitialSpawnDone;
                    // Dispatch lightweight spawn to game thread
            TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakCopy = WeakThis;
            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [WeakCopy, Filename, bParsed, FileIndex, TargetRank, MeshData = MoveTemp(MeshData), PointCloudData = MoveTemp(PointCloudData)]() mutable
                {
                    if (!WeakCopy.IsValid()) return;
                    WeakCopy->SpawnMeshFromData(Filename, bParsed, MoveTemp(MeshData), MoveTemp(PointCloudData), FileIndex, TargetRank);

                    // Chain next download in pipeline (overlap download with spawn)
                    // During live refresh, RefreshRemainingFiles drives ChainRefreshNext; PipelineNextIndex stays stale.
                    if (WeakCopy->RefreshRemainingFiles.Num() > 0) WeakCopy->ChainRefreshNext();
                    else if (!WeakCopy->bInitialSpawnDone && WeakCopy->PipelineNextIndex < WeakCopy->FilteredFiles.Num()) {
                        UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
                        if (S)
                            WeakCopy->PipelineDownloadNext(S);
                    }
                    // Don't call CheckAllDownloadsComplete during live refresh — it resets
                    // FilesDownloaded=0, ActorsSpawned=0, triggering infinite spawn/despawn loops.
                    if (!WeakCopy->bInitialSpawnDone) WeakCopy->CheckAllDownloadsComplete();
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

void AJUSYNCFileSpawnerActor::OnFileDownloaded(const FString& Filename, const TArray<uint8>& FileData)
{
    // Broadcast/live-refresh path: the buffer arrives as an external const ref, so hand the
    // parse an owned heap copy (the single copy this path always paid).
    OnSingleFileDownloaded(Filename, MakeUnique<TArray<uint8>>(FileData), true, FilesDownloaded, 0);
}

void AJUSYNCFileSpawnerActor::OnFileDownloadError(const FString& ErrorMessage)
{
    UE_LOG(LogTemp, Error, TEXT("JUSYNC Spawner: download error: %s"), *ErrorMessage);
    OnError.Broadcast(ErrorMessage);
}

void AJUSYNCFileSpawnerActor::DownloadGradientPng(UJUSYNCSubsystem* Subsystem)
{
    if (!Subsystem) return;

    FString PngPath = GradientPngFilename;
    int32 PngRank = 0;

    if (PngPath.IsEmpty())
    {
        // Auto-detect: use first .png from broker file list
        if (GradientPngRankMap.Num() > 0)
        {
            for (auto It = GradientPngRankMap.CreateConstIterator(); It; ++It)
            {
                PngPath = It.Key();
                PngRank = It.Value();
                break;
            }
        }
    }
    else
    {
        // Manual filename — try to find rank from broker list, default to first USD rank
        if (GradientPngRankMap.Contains(PngPath))
        {
            PngRank = GradientPngRankMap[PngPath];
        }
        else if (FilteredRanks.Num() > 0)
        {
            PngRank = FilteredRanks[0];
        }
    }

    if (PngPath.IsEmpty())
    {
        UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: no gradient PNG found in broker"));
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: downloading gradient PNG '%s' from rank %d"), *PngPath, PngRank);

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSub = Subsystem;
    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    FString PngCopy = PngPath;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSub, WeakThis, PngCopy, PngRank]()
    {
        if (!WeakSub.IsValid() || !WeakThis.IsValid()) return;

        TArray<uint8> PngData;
        bool bOk = false;

        // Retry PNG download up to 3 times with exponential backoff
        for (int32 Retry = 0; Retry < 3; ++Retry)
        {
            if (Retry > 0)
            {
                UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: retrying gradient PNG download (attempt %d/3)"), Retry + 1);
                FPlatformProcess::Sleep(1.0f * Retry);
            }
            bOk = WeakSub->RequestFile(PngCopy, PngRank, 15000, PngData);
            if (bOk && PngData.Num() > 0) break;
            PngData.Empty();
        }

        // Decode PNG and extract color LUT (first row pixels → 256-entry gradient)
        TArray<FColor> LUT;
        if (bOk && PngData.Num() > 0)
        {
            IImageWrapperModule& ImgMod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
            TSharedPtr<IImageWrapper> Img = ImgMod.CreateImageWrapper(EImageFormat::PNG);
            if (Img.IsValid() && Img->SetCompressed(PngData.GetData(), PngData.Num()))
            {
                TArray64<uint8> RawData;
                if (Img->GetRaw(RawData))
                {
                    int64 W = Img->GetWidth();
                    LUT.Reserve(FMath::Min(W, 256));
                    for (int64 x = 0; x < W && x < 256; ++x)
                    {
                        const uint8* Pixel = RawData.GetData() + x * 4;
                        // BGRA → RGB
                        LUT.Add(FColor(Pixel[2], Pixel[1], Pixel[0], 255));
                    }
                }
            }
        }

        // Fallback: try middleware cached gradient if PNG decode failed
        if (LUT.Num() == 0)
        {
            UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: PNG decode failed, trying middleware gradient cache"));
            UJUSYNCSubsystem* S = WeakSub.Get();
            if (S)
            {
                S->ApplyCachedGradientToSpawner();
                FJUSYNCPointCloudSpawner* Sp = S->GetPointCloudSpawner();
                if (Sp && Sp->GetGradientLUT().Num() > 0)
                {
                    LUT = Sp->GetGradientLUT();
                    UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: fallback gradient cache: %d colors"), LUT.Num());
                }
            }
        }

        if (LUT.Num() == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: failed to obtain gradient after PNG + cache fallback"));
        }

        if (LUT.Num() > 0)
        {
            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [WeakThis, LUT = MoveTemp(LUT)]()
                {
                    if (!WeakThis.IsValid()) return;

                    UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
                    if (!S) return;

                    FJUSYNCPointCloudSpawner* Sp = S->GetPointCloudSpawner();
                    if (Sp)
                    {
                        Sp->SetGradientLUT(LUT);
                        WeakThis->bGradientReady = true;
                        UE_LOG(LogTemp, Display, TEXT("[Spawner] Gradient LUT loaded: %d colors"), LUT.Num());

                        // Recolor any white actors spawned before gradient arrived
                        Sp->RecolorGradientPendingActors();
                    }
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: could not decode gradient PNG '%s'"), *PngCopy);
        }
    });
}

void AJUSYNCFileSpawnerActor::SpawnMeshFromData(const FString& Filename, bool bParsed, TArray<FJUSYNCMeshData>&& MeshData, TArray<FJUSYNCPointCloudData>&& PointCloudData, int32 FileIndex, int32 TargetRank)
{
    if (bIsCancelled) return;

    // After first PC parse, middleware has cached the gradient texture — extract it now
    // (gradient caching is populated during the first UsdProcessor::BakeColorsFromGradient call)
    // Guard: only attempt middleware gradient once per spawn cycle
    if (bParsed && bSpawnPointClouds && PointCloudData.Num() > 0 && !bGradientReady && !bGradientAttempted)
    {
        bGradientAttempted = true;
        UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
        if (S && S->GetPointCloudSpawner())
        {
            S->ApplyCachedGradientToSpawner();
            bGradientReady = S->GetPointCloudSpawner()->GetGradientLUT().Num() > 0;
            if (bGradientReady)
            {
                UE_LOG(LogTemp, Display, TEXT("[Spawner] Gradient LUT ready after first PC parse"));
                // Recolor any white actors spawned before gradient arrived
                S->GetPointCloudSpawner()->RecolorGradientPendingActors();
            }
        }
    }

    bool bHadMeshes = false;
    bool bHadPointClouds = false;

    //     Spawn meshes directly (already on game thread)
    if (bParsed && MeshData.Num() > 0)
    {
        int32 ValidMeshCount = 0;
        for (const FJUSYNCMeshData& m : MeshData) if (m.IsValid()) ValidMeshCount++;

        if (ValidMeshCount > 0)
        {
            bHadMeshes = true;

            FString MeshKeySuffix = TEXT("|") + Filename;

            // O(1) old actor lookup via FilenameToActors index
            bool bIsRefresh = bInitialSpawnDone;
            TArray<AActor*> OldMeshActors;
            TArray<AActor*>* pOldActors = FilenameToActors.Find(Filename);
            if (bIsRefresh && pOldActors)
            {
                OldMeshActors.Append(*pOldActors);
            }

            // Throttle: only spawn N meshes per frame, defer rest
            int32 SpawnThisFrame = FMath::Min(MeshData.Num(), MaxSpawnsPerFrame);
            int32 SpawnCount = 0;
            TArray<FJUSYNCMeshData> RemainingMeshes;
            for (int32 i = 0; i < MeshData.Num(); ++i)
            {
                if (!MeshData[i].IsValid()) continue;
                if (SpawnCount >= SpawnThisFrame)
                {
                    // Move (not copy): the deferred batch owns this mesh from
                    // now on; MeshData[i] is not read again after this point.
                    RemainingMeshes.Add(MoveTemp(MeshData[i]));
                    continue;
                }

                FVector SpawnLoc = GetNextSpawnLocation();
                AActor* Spawned = UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(MeshData[i], SpawnLoc);

                if (Spawned)
                {
                    SpawnedActors.Add(Spawned);
                    ActorsSpawned++;

                    // Track in both maps
                    FileToActorMap.Add(MeshData[i].ElementName + MeshKeySuffix, Spawned);
                    FilenameToActors.FindOrAdd(Filename).Add(Spawned);

                    Spawned->SetActorEnableCollision(false);

                    if (SpawnScale != FVector::ZeroVector)
                    {
                        FVector FinalScale = bUseUniformScaling ? FVector(SpawnScale.X) : SpawnScale;
                        Spawned->SetActorScale3D(FinalScale);
                    }

                    ApplyDynamicMaterial(Cast<UPrimitiveComponent>(Spawned->GetRootComponent()), Filename);
                    OnFileComplete.Broadcast(Filename, Spawned);
                    NextSpawnIndex++;
                    SpawnCount++;
                }
            }

            // Defer remaining mesh spawns to next frame
            if (RemainingMeshes.Num() > 0)
            {
                DeferredSpawns.Add(TPair<TArray<FJUSYNCMeshData>, FString>(MoveTemp(RemainingMeshes), Filename));
                // Schedule next batch in 1 frame (~16ms at 60fps)
                FTimerHandle DummyHandle;
                FTimerManager& TimerMgr = GetWorldTimerManager();
                TimerMgr.SetTimer(DummyHandle, FTimerDelegate::CreateUObject(this, &AJUSYNCFileSpawnerActor::ProcessDeferredSpawns), 0.02f, false);
            }

            // Destroy old mesh actors after new ones are live (atomic swap for refresh)
            if (bIsRefresh && OldMeshActors.Num() > 0)
            {
                for (AActor* OldActor : OldMeshActors)
                {
                    if (OldActor && OldActor->IsValidLowLevel())
                    {
                        SpawnedActors.Remove(OldActor);
                        if (ActorsSpawned > 0) ActorsSpawned--;
                        OldActor->Destroy();
                    }
                }
                // Remove destroyed old actors from reverse index — do NOT Empty() the
                // whole array since new mesh actors were already added at line ~754.
                if (pOldActors)
                {
                    pOldActors->RemoveAll([OldMeshActors](AActor* A)
                    {
                        return OldMeshActors.Contains(A);
                    });
                }
                for (AActor* OldActor : OldMeshActors)
                {
                    // Remove from FileToActorMap (we don't know exact key, iterate once per old actor)
                    for (auto It = FileToActorMap.CreateIterator(); It; ++It)
                    {
                        if (It.Value() == OldActor)
                        {
                            It.RemoveCurrent();
                            break;
                        }
                    }
                }
                UE_LOG(LogTemp, Log, TEXT("[Refresh] Destroyed %d old mesh actors for '%s'"), OldMeshActors.Num(), *Filename);
            }

            // Only check completion during initial spawn — during refresh this resets ActorsSpawned=0.
            if (!bInitialSpawnDone) CheckAllDownloadsComplete();

            FString ResultMsg = FString::Printf(TEXT("[Spawner] %s: spawned %d/%d meshes (%d deferred)"), *Filename, SpawnCount, MeshData.Num(), RemainingMeshes.Num());
            GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green, ResultMsg);
        }
    }

    // Dispatch point clouds immediately — gradient will recolor in-place when it arrives
    if (bSpawnPointClouds && PointCloudData.Num() > 0)
    {
        int32 ValidPCCount = 0;
        for (const FJUSYNCPointCloudData& pc : PointCloudData) if (pc.IsValid()) ValidPCCount++;

        if (ValidPCCount > 0)
        {
            bHadPointClouds = true;

            UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
            if (Subsystem)
            {
                FJUSYNCPointCloudSpawner* Spawner = Subsystem->GetPointCloudSpawner();
                if (Spawner)
                {
                    Spawner->SetSpawnLocation(GetNextSpawnLocation());
                    Spawner->SetSpawnScale(bUseUniformScaling ? SpawnScale.X : 1.0f);

                    bool bHasGradient = Spawner->GetGradientLUT().Num() > 0;
                    // Move each cloud into the async task (this is the last
                    // use of PointCloudData in this function).
                    for (FJUSYNCPointCloudData& PC : PointCloudData)
                    {
                        if (PC.IsValid())
                        {
                            PendingAsyncPCS++;
                            Spawner->EnqueuePointCloud(MoveTemp(PC), TargetRank);
                        }
                    }

                    UE_LOG(LogTemp, Log, TEXT("[Spawner] Dispatched %d PCs from '%s' (%s)"),
                           ValidPCCount, *Filename, bHasGradient ? TEXT("with gradient") : TEXT("white, will recolor"));
                }
            }
        }
    }

    // Report if nothing to spawn
    if (!bHadMeshes && !bHadPointClouds)
    {
        if (!bParsed)
        {
            UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: [PARSE FAILED] '%s'"), *Filename);
            GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow,
                FString::Printf(TEXT("[Spawner] PARSE FAILED: %s"), *Filename));
            if (!ParseFailedIndices.Contains(FileIndex))
                ParseFailedIndices.Add(FileIndex);
        }
        else
        {
            UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: [NO GEOMETRY] '%s' parsed but returned no meshes or point clouds"),
                   *Filename);
            GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow,
                FString::Printf(TEXT("[Spawner] NO GEOMETRY: %s"), *Filename));
            if (!ParseFailedIndices.Contains(FileIndex))
                ParseFailedIndices.Add(FileIndex);
        }
    }

    // Only check completion during initial spawn — during refresh this resets ActorsSpawned=0.
    if (!bInitialSpawnDone) CheckAllDownloadsComplete();
}

void AJUSYNCFileSpawnerActor::ApplyDynamicMaterial(UPrimitiveComponent* Comp, const FString& Filename)
{
    if (!Comp || !SpawnMaterial) return;

    UMaterialInstanceDynamic* DynamicMat = UMaterialInstanceDynamic::Create(SpawnMaterial, this);
    if (!DynamicMat) return;

    Comp->SetMaterial(0, DynamicMat);
    UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: created dynamic material instance for '%s'"), *Filename);
}

/** Dispatch buffered point clouds if gradient failed to load */
void AJUSYNCFileSpawnerActor::FlushBufferedPointClouds()
{
    if (PendingPointClouds.Num() == 0) return;

    UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!S) return;

    FJUSYNCPointCloudSpawner* Spawner = S->GetPointCloudSpawner();
    if (!Spawner) return;

    UE_LOG(LogTemp, Warning, TEXT("[Spawner] Flushing %d buffered PCs (gradient LUT unavailable, using white)"), PendingPointClouds.Num());
    // Move each cloud into the async task; the buffer is emptied right after.
    for (FJUSYNCPointCloudData& PC : PendingPointClouds)
    {
        PendingAsyncPCS++;
        Spawner->EnqueuePointCloud(MoveTemp(PC));
    }
    PendingPointClouds.Empty();
}

 void AJUSYNCFileSpawnerActor::OnPointCloudSpawnedHandler(const FString& EleName, AActor* Spawned)
    {
        // FIX3: Discard stale PC spawns that arrived after a chain refresh already handled the file
        if (bInitialSpawnDone && RefreshActive > 0)
        {
            // Chain refresh is active — check if a newer version of this element is already tracked
            AActor* Existing = FileToActorMap.FindRef(EleName);
            if (Existing && Existing != Spawned)
            {
                UE_LOG(LogTemp, Warning, TEXT("[PCSpawn] Stale async PC '%s' — chain refresh already active, discarding"), *EleName);
                Spawned->Destroy();
                PendingAsyncPCS--;
                if (PendingAsyncPCS < 0) PendingAsyncPCS = 0;
                return;
            }
        }

        if (Spawned)
        {
            SpawnedActors.Add(Spawned);
            ActorsSpawned++;
            Spawned->SetActorEnableCollision(false);
#if WITH_EDITORONLY_DATA
            Spawned->SetActorLabel(EleName);
#endif

            // Track for live updates (use element name as key, now includes rank)
            if (!EleName.IsEmpty())
            {
                AActor* OldActor = FileToActorMap.FindRef(EleName);
                if (OldActor && OldActor != Spawned && OldActor->IsValidLowLevel())
                {
                    // FIX3: Extra stale check — if old actor was already marked for removal by chain refresh
                    if (bInitialSpawnDone && RefreshingFiles.Contains(EleName))
                    {
                        UE_LOG(LogTemp, Log, TEXT("[PCSpawn] Old PC '%s' still refreshing, skipping swap"), *EleName);
                    }
                    else
                    {
                        if (bInitialSpawnDone)
                        {
                            // Refresh mode: destroy old actor, keep new one
                            SpawnedActors.Remove(OldActor);
                            if (ActorsSpawned > 0) ActorsSpawned--;
                            OldActor->Destroy();
                            UE_LOG(LogTemp, Log, TEXT("[Refresh] Destroyed old PC actor '%s', replacing with new"), *EleName);
                        }
                        else
                        {
                            // Stale async spawn during initial load — new actor already exists, destroy this one
                            SpawnedActors.Remove(Spawned);
                            ActorsSpawned--;
                            Spawned->Destroy();
                            UE_LOG(LogTemp, Warning, TEXT("[Spawner] Destroying stale async spawn '%s' (actor #+%d)"), *EleName, ActorsSpawned);
                            PendingAsyncPCS--;
                            if (PendingAsyncPCS < 0) PendingAsyncPCS = 0;
                            if (!bInitialSpawnDone) CheckAllDownloadsComplete();
                            return;
                        }
                    }
                }
                FileToActorMap.Add(EleName, Spawned);
            }

        OnFileComplete.Broadcast(EleName, Spawned);
        NextSpawnIndex++;

        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: loaded point cloud '%s' (actor #%d)"),
               *EleName, ActorsSpawned);
    }
    else
    {
        // Spawned is null — don't count it, just decrement pending
        UE_LOG(LogTemp, Log, TEXT("[PCSpawn] Null spawn for '%s', skipping"), *EleName);
    }

    PendingAsyncPCS--;
    if (PendingAsyncPCS < 0) PendingAsyncPCS = 0;

    // During initial spawn, always check completion. During live refresh, the chain tracks itself via RefreshActive.
    if (!bInitialSpawnDone)
        CheckAllDownloadsComplete();
}

void AJUSYNCFileSpawnerActor::ProcessDeferredSpawns()
{
    if (DeferredSpawns.Num() == 0) return;

    // Process up to MaxSpawnsPerFrame deferred mesh entries
    int32 Processed = 0;
    TArray<TPair<TArray<FJUSYNCMeshData>, FString>> Remaining;
    for (auto& Entry : DeferredSpawns)
    {
        int32 InThisEntry = Entry.Key.Num();
        int32 CanSpawn = FMath::Max(0, MaxSpawnsPerFrame - Processed);
        if (CanSpawn <= 0) { Remaining.Add(Entry); continue; }

        FString MeshKeySuffix = TEXT("|") + Entry.Value;
        int32 SpawnCount = 0;
        TArray<FJUSYNCMeshData> Leftovers;
        for (const FJUSYNCMeshData& M : Entry.Key)
        {
            if (!M.IsValid()) continue;
            if (SpawnCount >= CanSpawn) { Leftovers.Add(M); continue; }

            FVector Loc = GetNextSpawnLocation();
            AActor* Spawned = UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(M, Loc);
            if (Spawned)
            {
                SpawnedActors.Add(Spawned);
                ActorsSpawned++;
                FileToActorMap.Add(M.ElementName + MeshKeySuffix, Spawned);
                FilenameToActors.FindOrAdd(Entry.Value).Add(Spawned);
                Spawned->SetActorEnableCollision(false);
                if (SpawnScale != FVector::ZeroVector)
                    Spawned->SetActorScale3D(bUseUniformScaling ? FVector(SpawnScale.X) : SpawnScale);
                ApplyDynamicMaterial(Cast<UPrimitiveComponent>(Spawned->GetRootComponent()), Entry.Value);
                OnFileComplete.Broadcast(Entry.Value, Spawned);
                NextSpawnIndex++;
                SpawnCount++;
            }
        }
        Processed += SpawnCount;
        if (Leftovers.Num() > 0) Remaining.Add(TPair<TArray<FJUSYNCMeshData>, FString>(Leftovers, Entry.Value));
    }

    // Re-add remaining deferred spawns
    DeferredSpawns = MoveTemp(Remaining);
    if (DeferredSpawns.Num() > 0)
    {
        FTimerHandle DummyHandle;
        GetWorldTimerManager().SetTimer(DummyHandle, FTimerDelegate::CreateUObject(this, &AJUSYNCFileSpawnerActor::ProcessDeferredSpawns), 0.02f, false);
        UE_LOG(LogTemp, Verbose, TEXT("[Spawner] Deferred spawns remaining: %d entries"), DeferredSpawns.Num());
    }
    else
    {
        UE_LOG(LogTemp, Verbose, TEXT("[Spawner] All deferred spawns complete"));
    }
}

void AJUSYNCFileSpawnerActor::CheckAllDownloadsComplete()
{
    UE_LOG(LogTemp, Display, TEXT("[COMPLETION] ENTRY: FilesDownloaded=%d, FilesTotal=%d, PipelineActive=%d, PendingAsyncSpawns=%d, PendingAsyncPCS=%d, bInitialSpawnDone=%d"),
        FilesDownloaded, FilesTotal, PipelineActive, PendingAsyncSpawns, PendingAsyncPCS, bInitialSpawnDone ? 1 : 0);
    if (FilesDownloaded < FilesTotal)
    {
        UE_LOG(LogTemp, Warning, TEXT("[COMPLETION] EARLY EXIT: FilesDownloaded(%d) < FilesTotal(%d)"), FilesDownloaded, FilesTotal);
    }
    else
    {
        UE_LOG(LogTemp, Display, TEXT("[COMPLETION] FilesDownloaded >= FilesTotal, checking remaining guards..."));
    }
    if (FilesDownloaded >= FilesTotal)
    {
        // Wait for in-flight downloads to finish before deciding
        if (PipelineActive > 0) {
            UE_LOG(LogTemp, Display, TEXT("[COMPLETION] BLOCKED: PipelineActive=%d"), PipelineActive);
            return;
        }

        // Retry failed downloads AND parse failures if we have retries left
        int32 TotalFailed = FailedFileIndices.Num() + ParseFailedIndices.Num();
        if (TotalFailed > 0 && CurrentRetryCount < MaxRetries)
        {
            UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: %d files failed (dl=%d, parse=%d), retrying (%d/%d)..."),
                TotalFailed, FailedFileIndices.Num(), ParseFailedIndices.Num(), CurrentRetryCount + 1, MaxRetries);
            GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow,
                FString::Printf(TEXT("[Spawner] Retrying %d failed files (attempt %d/%d)..."),
                TotalFailed, CurrentRetryCount + 1, MaxRetries));
            CurrentRetryCount++;
            FilesDownloaded = FilesTotal - TotalFailed;
            PipelineActive = 0;
            PipelineNextIndex = FilesTotal;
            RetryFailedDownloads();
            return;
        }

        // Drain any ready point clouds from the spawner before reporting complete
        UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
        if (Subsystem && Subsystem->GetPointCloudSpawner())
        {
            Subsystem->GetPointCloudSpawner()->DrainReadyQueue();
        }

        // Wait for pending async spawns (meshes + point clouds) to finish
        UE_LOG(LogTemp, Display, TEXT("[COMPLETION] DrainReadyQueue done. PendingAsyncSpawns=%d, PendingAsyncPCS=%d"), PendingAsyncSpawns, PendingAsyncPCS);
        if (PendingAsyncSpawns > 0 || PendingAsyncPCS > 0)
        {
            UE_LOG(LogTemp, Display, TEXT("[COMPLETION] BLOCKED: Still waiting for async spawns (meshes=%d, PCs=%d)"), PendingAsyncSpawns, PendingAsyncPCS);
            UE_LOG(LogTemp, Log, TEXT("[Spawner] Waiting for async spawns to complete (meshes: %d, PCs: %d)"), PendingAsyncSpawns, PendingAsyncPCS);
            TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
            FTimerHandle WaitSpawnHandle;
            FTimerDelegate WaitSpawnDelay;
            WaitSpawnDelay.BindLambda([WeakThis]()
            {
                TWeakObjectPtr<AJUSYNCFileSpawnerActor> Wt = WeakThis;
                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [Wt]() { if (Wt.IsValid()) Wt->CheckAllDownloadsComplete(); },
                    TStatId(), nullptr, ENamedThreads::GameThread);
            });
            if (GWorld)
                GWorld->GetTimerManager().SetTimer(WaitSpawnHandle, WaitSpawnDelay, 0.2f, false);
            return;
        }

        UE_LOG(LogTemp, Display, TEXT("[COMPLETION] SUCCESS — all checks passed, setting Complete"));
        CurrentState = EJUSYNCSpawnerState::Complete;
        bool bSuccess = ActorsSpawned > 0;
        int32 DownloadFailed = FailedFileIndices.Num();
        int32 ParseFailed = ParseFailedIndices.Num();
        int32 FailedCount = FilesTotal - ActorsSpawned - DownloadFailed - ParseFailed;
        int32 ActualNotSpawned = FMath::Max(0, FailedCount);
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: pipeline complete. Spawned %d actors from %d files (%d not spawned, %d download failed, %d parse failed)"),
            ActorsSpawned, FilesTotal, ActualNotSpawned, DownloadFailed, ParseFailed);
        FColor StatusColor = bSuccess ? FColor::Green : FColor::Yellow;
        GEngine->AddOnScreenDebugMessage(-1, 10.0f, StatusColor,
            FString::Printf(TEXT("[Spawner] DONE: spawned %d actors from %d files (%d not spawned, %d failed)."), ActorsSpawned, FilesTotal, ActualNotSpawned, DownloadFailed + ParseFailed));

        // Log failed files
        for (int32 idx : FailedFileIndices)
        {
            if (FilteredFiles.IsValidIndex(idx))
            {
                UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: FAILED FILE: %s (rank %d)"),
                    *FilteredFiles[idx], FilteredRanks.IsValidIndex(idx) ? FilteredRanks[idx] : -1);
            }
        }

        for (int32 idx : ParseFailedIndices)
        {
            if (FilteredFiles.IsValidIndex(idx))
            {
                UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: PARSE FAILED: %s (rank %d)"),
                    *FilteredFiles[idx], FilteredRanks.IsValidIndex(idx) ? FilteredRanks[idx] : -1);
            }
        }

        FailedFileIndices.Empty();
        ParseFailedIndices.Empty();
        OnAllComplete.Broadcast(ActorsSpawned, bSuccess);

        // Mark initial spawn as done so live updates can proceed
        bInitialSpawnDone = true;
        RefreshedFiles.Empty();

        // Start live update polling if enabled
        if (bEnableLiveUpdates)
        {
            StartLiveUpdatePolling();
        }
    }
}

void AJUSYNCFileSpawnerActor::RetryFailedDownloads()
{
    int32 TotalFailed = FailedFileIndices.Num() + ParseFailedIndices.Num();
    if (TotalFailed == 0)
    {
        CheckAllDownloadsComplete();
        return;
    }

    // Merge parse failures into download retry (re-download the file)
    for (int32 idx : ParseFailedIndices)
    {
        if (!FailedFileIndices.Contains(idx))
            FailedFileIndices.Add(idx);
    }
    ParseFailedIndices.Empty();

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem) return;

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystemCopy = Subsystem;
    int32 AvailableSpots = FMath::Max(0, PipelineDepth - PipelineActive);
    int32 Spawned = 0;

    for (int32 i = 0; i < FailedFileIndices.Num() && Spawned < AvailableSpots; ++i)
    {
        int32 idx = FailedFileIndices[i];
        if (bIsCancelled || !FilteredFiles.IsValidIndex(idx)) continue;

        const FString& Filename = FilteredFiles[idx];
        int32 TargetRank = FilteredRanks[idx];
        int64 FileSize = FilteredSizes.IsValidIndex(idx) ? FilteredSizes[idx] : int64(2097152);
        int32 DynamicTimeout = FMath::Min(CalculateDynamicTimeout(FileSize) * 2, 120000);

        FailedFileIndices.RemoveAt(i);
        --i;
        PipelineActive++;
        Spawned++;

        TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
        TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = WeakSubsystemCopy;
        AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis, Filename, TargetRank, DynamicTimeout, idx, FileSize]()
            {
                if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

                TUniquePtr<TArray<uint8>> FileData = MakeUnique<TArray<uint8>>();
                // In-situ: download straight into FileData (wire size is known).
                bool bSuccess = WeakSubsystem->RequestFileSized(Filename, TargetRank, DynamicTimeout, *FileData, static_cast<uint64>(FileSize));

                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [WeakThis, Filename, FileData = MoveTemp(FileData), bSuccess, idx, TargetRank]() mutable
                    {
                        if (!WeakThis.IsValid()) return;
                        WeakThis->OnSingleFileDownloaded(Filename, MoveTemp(FileData), bSuccess, idx, TargetRank);
                    },
                    TStatId(), nullptr, ENamedThreads::GameThread);
            });
    }

    // If we didn't spawn all, chain the rest from OnSingleFileDownloaded
    if (FailedFileIndices.Num() > 0)
    {
        // Will be picked up by CheckAllDownloadsComplete or next OnSingleFileDownloaded
    }
}

// ============================================================================
// DEPTH-GATED CHAIN HELPERS
// ============================================================================

void AJUSYNCFileSpawnerActor::RetryRemainingFiles()
{
    if (FailedFileIndices.Num() == 0) return;
    if (CurrentRetryCount >= MaxRetries) return;

    RetryFailedDownloads();
}

void AJUSYNCFileSpawnerActor::ChainRefreshNext()
{
    if (RefreshRemainingFiles.Num() == 0) return;

    int32 AvailableSpots = FMath::Max(0, PipelineDepth - PipelineActive);
    int32 Spawned = 0;

    for (int32 i = 0; i < RefreshRemainingFiles.Num() && Spawned < AvailableSpots; ++i)
    {
        const auto& Entry = RefreshRemainingFiles[i];
        RefreshRemainingFiles.RemoveAt(i);
        --i;

        PipelineActive++;
        RefreshActive++;

        UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
        if (!Subsystem) return;

        TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
        TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
        FString Fname = Entry.Key;
        int32 Rank = Entry.Value;

        AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis, Fname, Rank]()
            {
                if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

                TUniquePtr<TArray<uint8>> FileData = MakeUnique<TArray<uint8>>();
                bool bSuccess = WeakSubsystem->RequestFile(Fname, Rank, 30000, *FileData);

                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [WeakThis, Fname, FileData = MoveTemp(FileData), bSuccess, Rank]() mutable
                    {
                        if (!WeakThis.IsValid()) return;
                        WeakThis->OnSingleFileDownloaded(Fname, MoveTemp(FileData), bSuccess, -1, Rank);
                        WeakThis->RefreshActive--;
                        if (WeakThis->RefreshActive <= 0 && WeakThis->RefreshRemainingFiles.Num() == 0)
                        {
                            WeakThis->bCommitDiffInProgress = false;
                            WeakThis->RefreshActive = 0;
                            WeakThis->RefreshingFiles.Empty();
                            WeakThis->RefreshedFiles.Empty();
                            // FIX4: Drain V2ActiveDownloads — any V2-enqueued files were handled by chain
                            // V2's own async handlers decrement their own; this catches V2→chain-enqueued ones
                            int32 RemainingV2 = WeakThis->V2ActiveDownloads.load();
                            if (RemainingV2 > 0)
                            {
                                WeakThis->V2ActiveDownloads.store(0);
                                UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Chain complete — drained %d stuck V2ActiveDownloads"), RemainingV2);
                            }
                            UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] All refreshes complete — dedup sets cleared"));
                        }
                    },
                    TStatId(), nullptr, ENamedThreads::GameThread);
            });
        Spawned++;
    }
}

// ============================================================================
// LIVE UPDATE SUPPORT
// ============================================================================

void AJUSYNCFileSpawnerActor::OnBrokerNotification(const FJUSYNCNotification& Notification)
{
    // UNCONDITIONAL diagnostic — fires before ANY guard so we can debug
    const char* TypeStr = (Notification.Type == EJUSYNCNotificationType::FileUpdateV2) ? "FileUpdateV2" :
                           (Notification.Type == EJUSYNCNotificationType::CommitComplete) ? "CommitComplete" : "FileUpdate";
    UE_LOG(LogTemp, Display, TEXT("[V2HANDLER] Received %s for '%s' | state=%d enabled=%d initialDone=%d"),
           ANSI_TO_TCHAR(TypeStr), *Notification.Filename, (int32)CurrentState, bEnableLiveUpdates ? 1 : 0, bInitialSpawnDone ? 1 : 0);
    GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Magenta,
        FString::Printf(TEXT("[V2HANDLER] state=%d enabled=%d"), (int32)CurrentState, bEnableLiveUpdates ? 1 : 0));
    GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Cyan,
        FString::Printf(TEXT("[SIGNAL] %s state=%d enabled=%d"), ANSI_TO_TCHAR(TypeStr), (int32)CurrentState, bEnableLiveUpdates ? 1 : 0));

    if (!bEnableLiveUpdates)
    {
        UE_LOG(LogTemp, Warning, TEXT("[SIGNAL] DROPPED — 'Enable Live Updates' is FALSE in spawner Details panel"));
        return;
    }
    if (CurrentState != EJUSYNCSpawnerState::Complete)
    {
        UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] BLOCKED: CurrentState=%d (need Complete=%d)"), (int32)CurrentState, (int32)EJUSYNCSpawnerState::Complete);
        GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red,
            TEXT("[LiveUpdate] Spawner not ready — wait for initial spawn to complete"));
        return;
    }

    if (Notification.Type == EJUSYNCNotificationType::FileUpdate)
        {
            UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] File update notification: '%s' (rank %d, size=%lld)"),
                   *Notification.Filename, Notification.SourceRank, (long long)Notification.FileSize);
            if (Notification.FileSize >= 2048)
            {
                HandleFileUpdateNotification(Notification.Filename, Notification.SourceRank);
            }
        }
    else if (Notification.Type == EJUSYNCNotificationType::FileUpdateV2)
    {
        bool bHashChanged = (Notification.HashLo != Notification.HashPrevLo ||
                             Notification.HashHi != Notification.HashPrevHi);
        UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] FileUpdateV2: '%s' (rank %d, hash_changed=%d)"),
               *Notification.Filename, Notification.SourceRank, bHashChanged ? 1 : 0);
        if (bHashChanged)
        {
            // Skip tiny stub files that consistently parse empty (659-byte .usda stubs)
            if (Notification.FileSize < 2048)
            {
                UE_LOG(LogTemp, Verbose, TEXT("[LiveUpdate] Skipping tiny file in V2 notification: '%s' (%lld bytes)"),
                       *Notification.Filename, (long long)Notification.FileSize);
            }
            else
            {
                HandleFileUpdateNotification(Notification.Filename, Notification.SourceRank);
            }
        }
        else
        {
            UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] FileUpdateV2: hash unchanged, skipping '%s'"),
                   *Notification.Filename);
        }
    }
    else if (Notification.Type == EJUSYNCNotificationType::CommitComplete)
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Commit complete notification"));
        HandleCommitCompleteNotification();
    }
}

void AJUSYNCFileSpawnerActor::HandleFileUpdateNotification(const FString& Filename, int32_t SourceRank)
{
    if (!bAutoRefreshMeshes || Filename.IsEmpty())
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping file update: auto-refresh disabled or empty filename"));
        return;
    }

    // Block live updates until initial spawn pipeline finishes
    if (!bInitialSpawnDone)
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Blocking: initial spawn not done yet (file='%s')"), *Filename);
        return;
    }

    // FIX4: When chain refresh is active, check if this file is already queued.
    // If already queued, safe to skip (chain will handle it). If NOT queued, add it
    // so chain picks it up — this prevents V2-notified files from being dropped
    // when CommitComplete fires mid-V2-burst.
    if (RefreshActive > 0)
    {
        bool bAlreadyQueued = false;
        for (const auto& QueuedFile : RefreshRemainingFiles)
        {
            if (QueuedFile.Key == Filename) { bAlreadyQueued = true; break; }
        }
        if (bAlreadyQueued)
        {
            UE_LOG(LogTemp, Verbose, TEXT("[LiveUpdate] V2 for '%s' already in chain queue, skipping"), *Filename);
        }
        else
        {
            // Enqueue V2 file into chain — chain will download+spawn it
            RefreshedFiles.Add(Filename);
            RefreshRemainingFiles.Add(TPair<FString, int32>(Filename, SourceRank));
            RefreshActive++;
            UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] V2 for '%s' NOT in chain — enqueuing for chain refresh"), *Filename);
        }
        return;
    }

    // Skip non-geometry files EARLY — before dedup to avoid polluting RefreshedFiles
    if (!Filename.EndsWith(TEXT(".usda")))
    {
        // Silent drop for known non-geometry (images/, .png, etc.) — already filtered at notification level
        return;
    }
    if (Filename.StartsWith(TEXT("Session_")) || Filename == TEXT("scene.usda") ||
        Filename.Contains(TEXT("manifest")) || Filename.Contains(TEXT("images/")) ||
        Filename.Contains(TEXT("shared/")) ||
        Filename.Contains(TEXT("primstages/")) ||
        Filename.Contains(TEXT("_Light.usda")) ||
        Filename.Contains(TEXT("_Material.usda")) ||
        Filename.Contains(TEXT("_Camera.usda")) ||
        Filename.Contains(TEXT("_Sampler.usda")))
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping non-clip USD file: '%s'"), *Filename);
        return;
    }

    // Skip if file is already being refreshed (in-flight dedup)
    if (RefreshingFiles.Contains(Filename))
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Already refreshing in-flight: '%s'"), *Filename);
        return;
    }

    // Deduplicate: skip if we already refreshed this file this cycle
    if (RefreshedFiles.Contains(Filename))
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Dedup: already refreshed '%s' this cycle"), *Filename);
        return;
    }

    UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] Refreshing file: '%s' from rank %d"), *Filename, SourceRank);
    GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Blue,
        FString::Printf(TEXT("[LiveUpdate] Refreshing: %s"), *Filename));

    // Track the file+rank from V2 for future use (even if tiny/stub now)
    SeenV2Files.Add(Filename, SourceRank);

    // Try to find the rank from our stored file lists
    int32 TargetRank = SourceRank;
    if (TargetRank < 0)
    {
        // Search in filtered files
        for (int32 i = 0; i < FilteredFiles.Num(); ++i)
        {
            if (FilteredFiles[i] == Filename)
            {
                TargetRank = FilteredRanks.IsValidIndex(i) ? FilteredRanks[i] : 0;
                break;
            }
        }
        // Also search in raw files
        if (TargetRank < 0)
        {
            for (int32 i = 0; i < RawFileRanks.Num(); ++i)
            {
                if (RawFileList.IsValidIndex(i) && RawFileList[i] == Filename)
                {
                    TargetRank = RawFileRanks[i];
                    break;
                }
            }
        }
        // Fallback: use V2-seen rank (file appeared in V2 but not in initial file list)
        if (TargetRank < 0)
        {
            int32* pRank = SeenV2Files.Find(Filename);
            if (pRank)
            {
                UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Using SeenV2Files rank %d for new file '%s'"), *pRank, *Filename);
                TargetRank = *pRank;
            }
        }
    }

    RefreshSingleFile(Filename, TargetRank);
}

 void AJUSYNCFileSpawnerActor::HandleCommitCompleteNotification(bool bIsTimer)
{
    if (!bAutoRefreshMeshes)
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping commit complete: auto-refresh disabled"));
        return;
    }

    double Now = FPlatformTime::Seconds();
    if (bCommitDiffInProgress)
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping commit complete: diff already in progress"));
        return;
    }

    // Only enforce cooldown for real CommitComplete notifications.
    // Timer fallback should always be able to run (it's the safety net when CommitComplete never arrives)
    // BUT: if V2 per-file refreshes are actively downloading/spawning, skip the chain refresh —
    // the chain's sync destroy would kill actors V2 just spawned, then V2's async handler
    // would kill the chain's fresh actors. Better to wait for V2 to settle.
    if (V2ActiveDownloads.load() > 0)
    {
        UE_LOG(LogTemp, Verbose, TEXT("[LiveUpdate] Skipping commit complete: V2 downloads in-flight (%d)"), V2ActiveDownloads.load());
        return;
    }
    if (!bIsTimer && Now - LastCommitCompleteTime < CommitCompleteCooldown)
    {
        double Remaining = CommitCompleteCooldown - (Now - LastCommitCompleteTime);
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping commit complete: cooldown active (%.1fs remaining)"), Remaining);
        return;
    }

    if (!bIsTimer) LastCommitCompleteTime = Now;
    bCommitDiffInProgress = true;
    // Don't clear RefreshedFiles here — V2 notifications may still be in-flight using it for dedup.
    // It gets cleared when the refresh chain fully completes.
    UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] Commit complete - re-fetching file list"));

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem || !Subsystem->IsBrokerConnected())
    {
        UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Cannot refresh: broker not connected"));
        bCommitDiffInProgress = false;
        return;
    }

    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis]()
    {
        if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

        TArray<FString> NewFiles;
        TArray<int64> NewSizes;
        TArray<int32> NewRanks;
        TArray<uint64> NewHashLo;
        TArray<uint64> NewHashHi;
        bool bSuccess = WeakSubsystem->RequestFileListWithSizesAndRanks(-1, 15000, NewFiles, NewSizes, NewRanks, &NewHashLo, &NewHashHi);

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakThis, NewFiles, NewSizes, NewRanks, NewHashLo, NewHashHi, bSuccess]()
            {
                if (!WeakThis.IsValid() || !bSuccess || NewFiles.Num() == 0)
                {
                    if (WeakThis.IsValid()) WeakThis->bCommitDiffInProgress = false;
                    return;
                }

                WeakThis->RawHashLo = NewHashLo;
                WeakThis->RawHashHi = NewHashHi;
                WeakThis->DiffAndRefreshFileList(NewFiles, NewSizes, NewRanks, false);
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
}

void AJUSYNCFileSpawnerActor::DiffAndRefreshFileList(const TArray<FString>& NewFiles, const TArray<int64>& NewSizes, const TArray<int32>& NewRanks, bool bIsManual)
{
    TMap<FString, int64> OldSizes;
    TMap<FString, uint64> OldHashLo;
    TMap<FString, uint64> OldHashHi;
    for (int32 i = 0; i < FilteredFiles.Num(); ++i)
    {
        OldSizes.Add(FilteredFiles[i], FilteredSizes.IsValidIndex(i) ? FilteredSizes[i] : 0);
        if (FilteredHashLo.IsValidIndex(i) && FilteredHashHi.IsValidIndex(i))
        {
            OldHashLo.Add(FilteredFiles[i], FilteredHashLo[i]);
            OldHashHi.Add(FilteredFiles[i], FilteredHashHi[i]);
        }
    }

    int32 ChangedCount = 0;
    int32 NewCount = 0;
    int32 SkippedCount = 0;
    TArray<FString> ChangedFiles;

    for (int32 i = 0; i < NewFiles.Num(); ++i)
    {
        const FString& Fname = NewFiles[i];
        int64 NewSize = NewSizes.IsValidIndex(i) ? NewSizes[i] : 0;
        int32 Rank = NewRanks.IsValidIndex(i) ? NewRanks[i] : 0;
        uint64 NewHLo = RawHashLo.IsValidIndex(i) ? RawHashLo[i] : 0;
        uint64 NewHHi = RawHashHi.IsValidIndex(i) ? RawHashHi[i] : 0;

        if (!Fname.EndsWith(TEXT(".usda"))) { SkippedCount++; continue; }
        if (bIsManual == false && (Fname.StartsWith(TEXT("Session_")) || Fname == TEXT("scene.usda") ||
            Fname.Contains(TEXT("manifest")) || Fname.Contains(TEXT("images/")) || Fname.Contains(TEXT("shared/"))))
        { SkippedCount++; continue; }

        int64 OldSize = 0;
        int64* pOldSize = OldSizes.Find(Fname);
        bool bFound = pOldSize != nullptr;
        if (bFound) OldSize = *pOldSize;

        uint64 OldHLo = 0, OldHHi = 0;
        uint64* pHLo = OldHashLo.Find(Fname);
        uint64* pHHi = OldHashHi.Find(Fname);
        bool bHasOldHash = (pHLo != nullptr && pHHi != nullptr);
        if (bHasOldHash) { OldHLo = *pHLo; OldHHi = *pHHi; }

        bool bHashChanged = (NewHLo != OldHLo || NewHHi != OldHHi);
        bool bSizeChanged = (NewSize != OldSize);

        // Skip tiny/empty stub files that parse empty
        if (NewSize < 2048)
        {
            UE_LOG(LogTemp, Verbose, TEXT("[LiveUpdate] Skipping tiny file '%s' (%lld bytes)"), *Fname, (long long)NewSize);
            SkippedCount++;
            continue;
        }

        // FIX2: Broaden hash detection — refresh even when old hash is unknown (untracked file)
        // Previously: (bHasOldHash && NewHLo && bHashChanged) — skipped files with no old hash
        // Now: detect change by hash OR size, regardless of whether old hash was known
        bool bShouldRefresh = !bFound || bSizeChanged;
        if (!bShouldRefresh && NewHLo && bHashChanged)
            bShouldRefresh = true;
        if (!bShouldRefresh && !bHasOldHash && NewHLo)
            bShouldRefresh = true; // new valid hash, never seen before

        if (bShouldRefresh)
        {
            const char* Reason = !bFound ? "new" : (bSizeChanged ? "size" : "hash");
            UE_LOG(LogTemp, Verbose, TEXT("[LiveUpdate] File changed '%s': %s"), *Fname, ANSI_TO_TCHAR(Reason));
            if (!bFound) NewCount++; else ChangedCount++;
            ChangedFiles.Add(Fname);
            RefreshRemainingFiles.Add(TPair<FString, int32>(Fname, Rank));
        }
    }

    // FIX1: Detect orphaned actors for files that no longer exist (e.g. rank redistribution)
    // Build set of new filenames for O(1) lookup
    TSet<FString> NewFileSet;
    for (const FString& Nf : NewFiles) NewFileSet.Add(Nf);

    int32 DeletedCount = 0;
    TSet<AActor*> DeletedOrphanActors;
    // Iterate a snapshot to avoid modifying FilteredFiles mid-iteration (it gets reassigned anyway)
    TArray<FString> OldFileList = FilteredFiles;
    for (const FString& OldName : OldFileList)
    {
        if (NewFileSet.Contains(OldName)) continue; // still exists
        // File gone — destroy its actors
        TArray<AActor*>* pGoneActors = FilenameToActors.Find(OldName);
        if (pGoneActors && pGoneActors->Num() > 0)
        {
            for (AActor* GoneActor : *pGoneActors)
            {
                if (GoneActor && GoneActor->IsValidLowLevel() && !DeletedOrphanActors.Contains(GoneActor))
                {
                    SpawnedActors.Remove(GoneActor);
                    if (ActorsSpawned > 0) ActorsSpawned--;
                    DeletedOrphanActors.Add(GoneActor);
                    DeletedCount++;
                    GoneActor->Destroy();
                    UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Destroyed orphan actor for deleted file '%s'"), *OldName);
                }
            }
            pGoneActors->Empty();
        }
    }
    // Clean up FileToActorMap entries referencing deleted actors
    for (auto It = FileToActorMap.CreateIterator(); It; ++It)
    {
        if (DeletedOrphanActors.Contains(It.Value()))
            It.RemoveCurrent();
    }
    if (DeletedCount > 0)
    {
        UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] Cleaned up %d orphan actors from deleted files"), DeletedCount);
    }

    FString Tag = bIsManual ? TEXT("[ManualRefresh]") : TEXT("[LiveUpdate]");
    FString Summary = FString::Printf(TEXT("%s %d changed, %d new, %d deleted, %d skipped"), *Tag, ChangedCount, NewCount, DeletedCount, SkippedCount);
    UE_LOG(LogTemp, Display, TEXT("%s"), *Summary);
    GEngine->AddOnScreenDebugMessage(-1, 4.0f, bIsManual ? FColor::Green : FColor::Orange, Summary);

    // Always update tracking maps — even if only deletions occurred
    RawFileList = NewFiles;
    RawFileSizes = NewSizes;
    RawFileRanks = NewRanks;
    FilteredFiles = NewFiles;
    FilteredSizes = NewSizes;
    FilteredRanks = NewRanks;
    FilteredHashLo = RawHashLo;
    FilteredHashHi = RawHashHi;

    // Reset bCommitDiffInProgress if nothing changed and nothing deleted
    if (RefreshRemainingFiles.Num() == 0)
    {
        bCommitDiffInProgress = false;
        if (DeletedCount > 0)
        {
            UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] No files to refresh, cleaned %d orphan actors"), DeletedCount);
        }
        else
        {
            UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] No files changed, nothing to refresh"));
        }
        return;
    }

    RefreshActive = 0;
    ChainRefreshNext();
}

void AJUSYNCFileSpawnerActor::ManualRefresh()
{
    UE_LOG(LogTemp, Display, TEXT("[ManualRefresh] Triggered manual refresh"));
    GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow, TEXT("[Spawner] Manual refresh started..."));

    if (CurrentState != EJUSYNCSpawnerState::Complete)
    {
        UE_LOG(LogTemp, Warning, TEXT("[ManualRefresh] Not in Complete state (%d), cannot refresh"), (int32)CurrentState);
        return;
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem || !Subsystem->IsBrokerConnected())
    {
        UE_LOG(LogTemp, Warning, TEXT("[ManualRefresh] Broker not connected"));
        return;
    }

    bCommitDiffInProgress = true;
    RefreshedFiles.Empty();

    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis]()
    {
        if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

        TArray<FString> NewFiles;
        TArray<int64> NewSizes;
        TArray<int32> NewRanks;
        TArray<uint64> NewHashLo;
        TArray<uint64> NewHashHi;
        bool bSuccess = WeakSubsystem->RequestFileListWithSizesAndRanks(-1, 15000, NewFiles, NewSizes, NewRanks, &NewHashLo, &NewHashHi);

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakThis, NewFiles, NewSizes, NewRanks, NewHashLo, NewHashHi, bSuccess]()
            {
                if (!WeakThis.IsValid() || !bSuccess || NewFiles.Num() == 0)
                {
                    if (WeakThis.IsValid()) WeakThis->bCommitDiffInProgress = false;
                    return;
                }

                WeakThis->RawHashLo = NewHashLo;
                WeakThis->RawHashHi = NewHashHi;
                WeakThis->DiffAndRefreshFileList(NewFiles, NewSizes, NewRanks, true);
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
}

void AJUSYNCFileSpawnerActor::StartLiveUpdatePolling()
{
    if (!bEnableLiveUpdates || LiveUpdatePollInterval <= 0.0f)
        return;

    if (GWorld)
    {
        GWorld->GetTimerManager().ClearTimer(LiveUpdateTimerHandle);
        GWorld->GetTimerManager().SetTimer(LiveUpdateTimerHandle,
            FTimerDelegate::CreateUObject(this, &AJUSYNCFileSpawnerActor::OnLiveUpdateTimer),
            LiveUpdatePollInterval, true);
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Polling started (interval: %.1fs)"), LiveUpdatePollInterval);
    }
}

void AJUSYNCFileSpawnerActor::StopLiveUpdatePolling()
{
    if (GWorld)
    {
        GWorld->GetTimerManager().ClearTimer(LiveUpdateTimerHandle);
    }
}

void AJUSYNCFileSpawnerActor::OnLiveUpdateTimer()
{
    if (CurrentState != EJUSYNCSpawnerState::Complete)
        return;

    // If commit complete handler is enabled, try it first (will hit cooldown if recently called)
            // BUT: if V2 per-file refreshes are still downloading+spawning on background threads,
            // skip the chain refresh entirely — it would destroy actors V2 just spawned,
            // then V2's async handler would destroy the chain's new actors. (race condition)
            if (bAutoRefreshMeshes)
            {
                if (V2ActiveDownloads.load() > 0)
                {
                    UE_LOG(LogTemp, Verbose, TEXT("[LiveUpdate] Timer: V2 downloads in-flight (%d), skipping chain refresh"), V2ActiveDownloads.load());
                }
                else
                {
                    HandleCommitCompleteNotification(true);  // true = timer fallback, skip cooldown
                }
                return;
            }

    // Fallback: even if auto-refresh is disabled, still detect changes and log them
    // This ensures hash-based diff runs when CommitComplete notification never arrives
    if (!bInitialSpawnDone) return;
    if (bCommitDiffInProgress) return;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem || !Subsystem->IsBrokerConnected()) return;

    // Quick size-based check only (hash diff is expensive)
    bCommitDiffInProgress = true;
    RefreshedFiles.Empty();

    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis]()
    {
        if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

        TArray<FString> NewFiles;
        TArray<int64> NewSizes;
        TArray<int32> NewRanks;
        TArray<uint64> NewHashLo;
        TArray<uint64> NewHashHi;
        bool bSuccess = WeakSubsystem->RequestFileListWithSizesAndRanks(-1, 10000, NewFiles, NewSizes, NewRanks, &NewHashLo, &NewHashHi);

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakThis, NewFiles, NewSizes, NewRanks, NewHashLo, NewHashHi, bSuccess]()
            {
                if (!WeakThis.IsValid() || !bSuccess || NewFiles.Num() == 0)
                {
                    if (WeakThis.IsValid()) WeakThis->bCommitDiffInProgress = false;
                    return;
                }

                WeakThis->RawHashLo = NewHashLo;
                WeakThis->RawHashHi = NewHashHi;
                WeakThis->DiffAndRefreshFileList(NewFiles, NewSizes, NewRanks, false);
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
}

bool AJUSYNCFileSpawnerActor::RefreshSingleFile(const FString& Filename, int32 TargetRank)
{
    if (Filename.IsEmpty())
        return false;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem || !Subsystem->IsBrokerConnected())
    {
        UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Cannot refresh '%s': broker not connected"), *Filename);
        return false;
    }

    //     Mark as refreshing (in-flight dedup) + refreshed
    RefreshingFiles.Add(Filename);
    RefreshedFiles.Add(Filename);

    UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] RefreshSingleFile START: '%s' (rank %d)"), *Filename, TargetRank);

    // Download and re-spawn (async — no destruction yet)
    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    FString FilenameCopy = Filename;
    int32 RankCopy = TargetRank;

    // Track V2 download activity so the timer chain refresh won't fire while
    // our V2's async spawn is still pending.
    V2ActiveDownloads++;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis, FilenameCopy, RankCopy]()
    {
        if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

        int32 Timeout = 30000;
        TArray<uint8> FileData;
        bool bSuccess = WeakSubsystem->RequestFile(FilenameCopy, RankCopy, Timeout, FileData);

        if (!bSuccess || FileData.Num() == 0)
        {
            // Remove from RefreshingFiles so we don't block future V2 notifications for this file
            if (WeakThis.IsValid())
            {
                WeakThis->RefreshingFiles.Remove(FilenameCopy);
                WeakThis->V2ActiveDownloads--;
            }
            UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Failed to download updated file: '%s'"), *FilenameCopy);
            return;
        }

        TArray<FJUSYNCMeshData> MeshData;
        TArray<FJUSYNCPointCloudData> PointCloudData;
        FString Preview;
        bool bParsed = UJUSYNCBlueprintLibrary::LoadUSDFullFromBufferNoCopy(FileData, FilenameCopy, MeshData, PointCloudData, Preview);

        TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakCopy = WeakThis;
        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakCopy, FilenameCopy, bParsed, RankCopy, MeshData = MoveTemp(MeshData), PointCloudData = MoveTemp(PointCloudData)]() mutable
            {
                // Always remove from in-flight set
                if (WeakCopy.IsValid()) WeakCopy->RefreshingFiles.Remove(FilenameCopy);

                if (!WeakCopy.IsValid()) return;

                // Stale guard: if chain refresh is now active (downloading/spawning from CommitComplete/timer),
                // bail out — the chain will handle this file with proper dedup
                if (WeakCopy->RefreshActive > 0)
                {
                    UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Stale: chain refresh active (%d in flight), discarding V2 for '%s'"), WeakCopy->RefreshActive, *FilenameCopy);
                    WeakCopy->V2ActiveDownloads--;
                    return;
                }

                if (!bParsed || (MeshData.Num() == 0 && PointCloudData.Num() == 0))
                {
                    UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Parse returned no data for: '%s' — keeping existing actors"), *FilenameCopy);
                    WeakCopy->V2ActiveDownloads--;
                    return;
                }

                // O(1) collect old actors via FilenameToActors index
                FString MeshKeySuffix = TEXT("|") + FilenameCopy;
                TArray<AActor*> OldActorsToDestroy;
                TArray<AActor*>* pOldList = WeakCopy->FilenameToActors.Find(FilenameCopy);
                if (pOldList) OldActorsToDestroy.Append(*pOldList);
                // Track old PC actors separately — they are NOT destroyed sync because new
                // PCs spawn ASYNC. Let OnPointCloudSpawnedHandler destroy them when the new
                // PC is live (atomic swap). Destroying them sync would leave a 400-500ms gap.
                TSet<AActor*> OldPCs;
                // Also find old PC actors by element name matching (key is "ElementName_rRank")
                for (const auto& PC : PointCloudData)
                {
                    if (!PC.IsValid()) continue;
                    FString PCKey = FString::Printf(TEXT("%s_r%d"), *PC.ElementName, RankCopy);
                    AActor* OldPC = WeakCopy->FileToActorMap.FindRef(PCKey);
                    if (!OldPC) OldPC = WeakCopy->FileToActorMap.FindRef(PC.ElementName); // fallback bare name
                    if (OldPC && OldPC->IsValidLowLevel() && !OldActorsToDestroy.Contains(OldPC))
                    {
                        OldActorsToDestroy.Add(OldPC);
                        OldPCs.Add(OldPC);
                    }
                }

                bool bIsRefresh = OldActorsToDestroy.Num() > 0;
                UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] %s: %d old actors found for '%s'"),
                    bIsRefresh ? TEXT("REFRESH") : TEXT("NEW SPAWN"), OldActorsToDestroy.Num(), *FilenameCopy);

                // Spawn new meshes
                int32 SpawnCount = 0;
                for (int32 i = 0; i < MeshData.Num(); ++i)
                {
                    if (!MeshData[i].IsValid()) continue;

                    FVector SpawnLoc = WeakCopy->GetNextSpawnLocation();
                    AActor* Spawned = UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(MeshData[i], SpawnLoc);

                    if (Spawned)
                    {
                        WeakCopy->SpawnedActors.Add(Spawned);
                        WeakCopy->ActorsSpawned++;
                        WeakCopy->FileToActorMap.Add(MeshData[i].ElementName + MeshKeySuffix, Spawned);
                        WeakCopy->FilenameToActors.FindOrAdd(FilenameCopy).Add(Spawned);
                        Spawned->SetActorEnableCollision(false);

                        if (WeakCopy->SpawnScale != FVector::ZeroVector)
                        {
                            FVector FinalScale = WeakCopy->bUseUniformScaling ? FVector(WeakCopy->SpawnScale.X) : WeakCopy->SpawnScale;
                            Spawned->SetActorScale3D(FinalScale);
                        }

                        WeakCopy->OnFileComplete.Broadcast(FilenameCopy, Spawned);
                        WeakCopy->NextSpawnIndex++;
                        SpawnCount++;
                    }
                }

                // Spawn new point clouds
                if (WeakCopy->bSpawnPointClouds && PointCloudData.Num() > 0)
                {
                    UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
                    if (S && S->GetPointCloudSpawner())
                    {
                        FJUSYNCPointCloudSpawner* Spawner = S->GetPointCloudSpawner();
                        Spawner->SetSpawnLocation(WeakCopy->GetNextSpawnLocation());
                        Spawner->SetSpawnScale(WeakCopy->bUseUniformScaling ? WeakCopy->SpawnScale.X : 1.0f);

                        // Move each cloud into the async task (last use of
                        // PointCloudData in this lambda).
                        for (FJUSYNCPointCloudData& PC : PointCloudData)
                        {
                            if (PC.IsValid())
                            {
                                WeakCopy->PendingAsyncPCS++;
                                Spawner->EnqueuePointCloud(MoveTemp(PC), RankCopy);
                            }
                        }
                    }
                }

                // Destroy old actors (atomic swap — done after new actors are live)
                // CRITICAL: Point cloud actors from OnPointCloudSpawnedHandler are NOT destroyed
                // here because the new PCs spawn ASYNC (several frames later). Destroying the
                // old PC sync would leave it missing until the new one spawns (400-500ms gap).
                // Instead, let OnPointCloudSpawnedHandler destroy the old PC after the new one
                // is live — it finds it by ElementName_rRank key in FileToActorMap.

                // Mesh actors (from FilenameToActors) — destroy NOW, new meshes are live
                for (AActor* OldActor : OldActorsToDestroy)
                {
                    if (OldActor && OldActor->IsValidLowLevel() && !OldPCs.Contains(OldActor))
                    {
                        WeakCopy->SpawnedActors.Remove(OldActor);
                        if (WeakCopy->ActorsSpawned > 0) WeakCopy->ActorsSpawned--;
                        OldActor->Destroy();
                    }
                }
                // Clean FileToActorMap for mesh actors only (not PCs — handler will swap them)
                for (AActor* OldActor : OldActorsToDestroy)
                {
                    if (!OldPCs.Contains(OldActor))
                    {
                        for (auto It = WeakCopy->FileToActorMap.CreateIterator(); It; ++It)
                        {
                            if (It.Value() == OldActor) { It.RemoveCurrent(); break; }
                        }
                    }
                }
                // FIX: Remove only the DESTROYED old actors from FilenameToActors.
                // Do NOT call pOldList->Empty() — new mesh actors were just added to the
                // same array at line ~1970 and must remain for the next refresh cycle.
                if (pOldList)
                {
                    pOldList->RemoveAll([OldActorsToDestroy, OldPCs](AActor* A)
                    {
                        return A && !A->IsValidLowLevel();
                    });
                }

                FString ResultMsg;
                if (bIsRefresh)
                {
                    ResultMsg = FString::Printf(TEXT("[Refresh] %s: %d new, %d destroyed, %d PCs async"),
                        *FilenameCopy, SpawnCount, OldActorsToDestroy.Num() - OldPCs.Num(), OldPCs.Num());
                }
                else
                {
                    ResultMsg = FString::Printf(TEXT("[New] %s: %d meshes spawned"), *FilenameCopy, SpawnCount);
                }
                UE_LOG(LogTemp, Display, TEXT("%s"), *ResultMsg);
                GEngine->AddOnScreenDebugMessage(-1, 2.0f, bIsRefresh ? FColor::Green : FColor::Cyan, ResultMsg);
                WeakCopy->V2ActiveDownloads--;
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });

    return true;
}
