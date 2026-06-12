#include "JUSYNCFileSpawnerActor.h"
#include <atomic>
#include <mutex>
#include "JUSYNCSubsystem.h"
#include "JUSYNCPointCloudSpawner.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Kismet/GameplayStatics.h"

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
    bEnableLiveUpdates = false;
    LiveUpdatePollInterval = 5.0f;
    bAutoRefreshMeshes = true;
    LastCommitCompleteTime = 0.0;
    CommitCompleteCooldown = 5.0;
    bCommitDiffInProgress = false;
    bInitialSpawnDone = false;
    bSpawnPointClouds = true;
    bUseGradientColors = true;
    GradientPngFilename = TEXT("");
    PointCloudSize = 1.0f;
    bGradientReady = false;
    PipelineDepth = 3;
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

    // Single-pass categorization: bucket files by type, apply all filters in one pass
    TArray<FString> PngFiles;
    TArray<int32> PngRanks;
    TArray<FString> LocalFiles;
    TArray<int64> LocalSizes;
    TArray<int32> LocalRanks;
    LocalFiles.Reserve(FileList.Num());
    LocalSizes.Reserve(FileList.Num());
    LocalRanks.Reserve(FileList.Num());

    for (int32 i = 0; i < FileList.Num(); ++i)
    {
        const FString& Fname = FileList[i];
        int64 Fsize = FileSizes.IsValidIndex(i) ? FileSizes[i] : 0;
        int32 Frank = FileRanks.IsValidIndex(i) ? FileRanks[i] : 0;

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
    }

    for (int32 i = 0; i < PngFiles.Num(); ++i)
    {
        GradientPngRankMap.Add(PngFiles[i], PngRanks[i]);
    }

    RawFileList = LocalFiles;
    RawFileSizes = LocalSizes;
    RawFileRanks = LocalRanks;
    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: single-pass filter: %d geometry files, %d PNGs"), RawFileList.Num(), PngFiles.Num());

    FilteredFiles = RawFileList;
    FilteredSizes = RawFileSizes;
    FilteredRanks = RawFileRanks;
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

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis, Filename, TargetRank, DynamicTimeout, FileIndex]()
        {
            if (!WeakSubsystem.IsValid() || !WeakThis.IsValid())
            {
                return;
            }

            TArray<uint8> FileData;
            bool bSuccess = WeakSubsystem->RequestFile(Filename, TargetRank, DynamicTimeout, FileData);

            TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThisCopy = WeakThis;
            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [WeakThisCopy, Filename, FileData, bSuccess, FileIndex, TargetRank]()
                {
                    if (!WeakThisCopy.IsValid()) return;
                    WeakThisCopy->OnSingleFileDownloaded(Filename, FileData, bSuccess, FileIndex, TargetRank);
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

void AJUSYNCFileSpawnerActor::OnSingleFileDownloaded(const FString& Filename, const TArray<uint8>& FileData, bool bSuccess, int32 FileIndex, int32 TargetRank)
{
    if (bIsCancelled) return;

    if (!bSuccess || FileData.Num() == 0)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: [DOWNLOAD FAILED] '%s' (rank %d)"), *Filename, FilteredRanks.IsValidIndex(FileIndex) ? FilteredRanks[FileIndex] : -1);
        if (!FailedFileIndices.Contains(FileIndex))
            FailedFileIndices.Add(FileIndex);
        FilesDownloaded++;
        OnFileProgress.Broadcast(FilesDownloaded, FilesTotal);
        PipelineActive--;
        // Chain pending work that was depth-gated
        if (RefreshRemainingFiles.Num() > 0) ChainRefreshNext();
        else if (FailedFileIndices.Num() > 0 && CurrentRetryCount < MaxRetries) RetryRemainingFiles();
        else if (PipelineNextIndex < FilteredFiles.Num()) {
            UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
            if (S)
                PipelineDownloadNext(S);
        }
        CheckAllDownloadsComplete();
        return;
    }

    // Quick bookkeeping on game thread, then dispatch heavy parse to background
    FilesDownloaded++;
    OnFileProgress.Broadcast(FilesDownloaded, FilesTotal);
    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: downloaded %s (%d/%d)"), *Filename, FilesDownloaded, FilesTotal);
    NextSpawnIndex = FileIndex;

    // Move USD parse (heavy) to background thread — game thread stays responsive
    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis, Filename, FileData, FileIndex, TargetRank]() mutable
        {
            if (!WeakThis.IsValid()) return;

            TArray<FJUSYNCMeshData> MeshData;
            TArray<FJUSYNCPointCloudData> PointCloudData;
            FString Preview;
            bool bParsed = UJUSYNCBlueprintLibrary::LoadUSDFullFromBuffer(FileData, Filename, MeshData, PointCloudData, Preview);

            // Dispatch lightweight spawn to game thread
            TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakCopy = WeakThis;
            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [WeakCopy, Filename, bParsed, FileIndex, TargetRank, MeshData = MoveTemp(MeshData), PointCloudData = MoveTemp(PointCloudData)]() mutable
                {
                    if (!WeakCopy.IsValid()) return;
                    WeakCopy->SpawnMeshFromData(Filename, bParsed, MoveTemp(MeshData), MoveTemp(PointCloudData), FileIndex, TargetRank);

                    // Chain next download in pipeline (overlap download with spawn)
                    WeakCopy->PipelineActive--;
                    // Chain pending work that was depth-gated
                    if (WeakCopy->RefreshRemainingFiles.Num() > 0) WeakCopy->ChainRefreshNext();
                    else if (WeakCopy->FailedFileIndices.Num() > 0 && WeakCopy->CurrentRetryCount < WeakCopy->MaxRetries) WeakCopy->RetryRemainingFiles();
                    else if (WeakCopy->PipelineNextIndex < WeakCopy->FilteredFiles.Num()) {
                        UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
                        if (S)
                            WeakCopy->PipelineDownloadNext(S);
                    }
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

void AJUSYNCFileSpawnerActor::OnFileDownloaded(const FString& Filename, const TArray<uint8>& FileData)
{
    OnSingleFileDownloaded(Filename, FileData, true, FilesDownloaded, 0);
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
        bool bOk = WeakSub->RequestFile(PngCopy, PngRank, 10000, PngData);

        if (!bOk || PngData.Num() == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: failed to download gradient PNG '%s'"), *PngCopy);
            return;
        }

        // Decode PNG and extract color LUT (first row pixels → 256-entry gradient)
        TArray<FColor> LUT;
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
    if (bParsed && bSpawnPointClouds && PointCloudData.Num() > 0 && !bGradientReady)
    {
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

    // Spawn meshes directly (already on game thread)
    if (bParsed && MeshData.Num() > 0)
    {
        int32 ValidMeshCount = 0;
        for (const FJUSYNCMeshData& m : MeshData) if (m.IsValid()) ValidMeshCount++;

        if (ValidMeshCount > 0)
        {
            bHadMeshes = true;

            FString MeshKeySuffix = TEXT("|") + Filename;
            int32 SpawnCount = 0;
            for (int32 i = 0; i < MeshData.Num(); ++i)
            {
                if (!MeshData[i].IsValid()) continue;

                FVector SpawnLoc = GetNextSpawnLocation();
                AActor* Spawned = UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(MeshData[i], SpawnLoc);

                if (Spawned)
                {
                    SpawnedActors.Add(Spawned);
                    ActorsSpawned++;

                    // Track for live updates (pre-concatenated key)
                    FileToActorMap.Add(MeshData[i].ElementName + MeshKeySuffix, Spawned);
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

            CheckAllDownloadsComplete();

            FString ResultMsg = FString::Printf(TEXT("[Spawner] %s: spawned %d meshes"), *Filename, SpawnCount);
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
                    for (const FJUSYNCPointCloudData& PC : PointCloudData)
                    {
                        if (PC.IsValid())
                        {
                            PendingAsyncPCS++;
                            Spawner->EnqueuePointCloud(PC, TargetRank);
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

/** Dispatch buffered point clouds if gradient failed to load */
void AJUSYNCFileSpawnerActor::FlushBufferedPointClouds()
{
    if (PendingPointClouds.Num() == 0) return;

    UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!S) return;

    FJUSYNCPointCloudSpawner* Spawner = S->GetPointCloudSpawner();
    if (!Spawner) return;

    UE_LOG(LogTemp, Warning, TEXT("[Spawner] Flushing %d buffered PCs (gradient LUT unavailable, using white)"), PendingPointClouds.Num());
    for (FJUSYNCPointCloudData& PC : PendingPointClouds)
    {
        PendingAsyncPCS++;
        Spawner->EnqueuePointCloud(PC);
    }
    PendingPointClouds.Empty();
}

void AJUSYNCFileSpawnerActor::OnPointCloudSpawnedHandler(const FString& EleName, AActor* Spawned)
{
    if (Spawned)
    {
        SpawnedActors.Add(Spawned);
        ActorsSpawned++;
        Spawned->SetActorEnableCollision(false);
        Spawned->SetActorLabel(EleName);

        // Track for live updates (use element name as key, now includes rank)
        if (!EleName.IsEmpty())
        {
            AActor* OldActor = FileToActorMap.FindRef(EleName);
            if (OldActor && OldActor != Spawned && OldActor->IsValidLowLevel())
            {
                // Stale async spawn — new actor already exists, destroy this one
                SpawnedActors.Remove(Spawned);
                ActorsSpawned--;
                Spawned->Destroy();
                UE_LOG(LogTemp, Warning, TEXT("[Spawner] Destroying stale async spawn '%s' (actor #+%d)"), *EleName, ActorsSpawned);
                PendingAsyncPCS--;
                if (PendingAsyncPCS < 0) PendingAsyncPCS = 0;
                CheckAllDownloadsComplete();
                return;
            }
            FileToActorMap.Add(EleName, Spawned);
        }

        OnFileComplete.Broadcast(EleName, Spawned);
        NextSpawnIndex++;

        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: loaded point cloud '%s' (actor #%d)"),
               *EleName, ActorsSpawned);
    }

    PendingAsyncPCS--;
    if (PendingAsyncPCS < 0) PendingAsyncPCS = 0;

    CheckAllDownloadsComplete();
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
            FilesDownloaded = FilesTotal - FailedFileIndices.Num(); // Count only remaining failed
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
        if (PendingAsyncSpawns > 0 || PendingAsyncPCS > 0)
        {
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
    if (FailedFileIndices.Num() == 0)
    {
        CheckAllDownloadsComplete();
        return;
    }

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
        AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis, Filename, TargetRank, DynamicTimeout, idx]()
            {
                if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

                TArray<uint8> FileData;
                bool bSuccess = WeakSubsystem->RequestFile(Filename, TargetRank, DynamicTimeout, FileData);

                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [WeakThis, Filename, FileData, bSuccess, idx, TargetRank]()
                    {
                        if (!WeakThis.IsValid()) return;
                        WeakThis->OnSingleFileDownloaded(Filename, FileData, bSuccess, idx, TargetRank);
                    },
                    TStatId(), nullptr, ENamedThreads::GameThread);
            });
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

                TArray<uint8> FileData;
                bool bSuccess = WeakSubsystem->RequestFile(Fname, Rank, 30000, FileData);

                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [WeakThis, Fname, FileData, bSuccess, Rank]()
                    {
                        if (!WeakThis.IsValid()) return;
                        WeakThis->OnSingleFileDownloaded(Fname, FileData, bSuccess, -1, Rank);
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
    if (!bEnableLiveUpdates || CurrentState != EJUSYNCSpawnerState::Complete)
        return;

    if (Notification.Type == EJUSYNCNotificationType::FileUpdate)
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] File update notification: '%s' (rank %d)"),
               *Notification.Filename, Notification.SourceRank);
        HandleFileUpdateNotification(Notification.Filename, Notification.SourceRank);
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

    // Deduplicate: skip if we already refreshed this file this cycle
    if (RefreshedFiles.Contains(Filename))
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Dedup: already refreshed '%s' this cycle"), *Filename);
        return;
    }

    // Skip non-geometry files — only geometry clip files are worth refreshing
    if (!Filename.EndsWith(TEXT(".usda")))
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping non-USDA file: '%s'"), *Filename);
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

    UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] Refreshing file: '%s' from rank %d"), *Filename, SourceRank);
    GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Blue,
        FString::Printf(TEXT("[LiveUpdate] Refreshing: %s"), *Filename));

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
    }

    RefreshSingleFile(Filename, TargetRank);
}

void AJUSYNCFileSpawnerActor::HandleCommitCompleteNotification()
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
    if (Now - LastCommitCompleteTime < CommitCompleteCooldown)
    {
        double Remaining = CommitCompleteCooldown - (Now - LastCommitCompleteTime);
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping commit complete: cooldown active (%.1fs remaining)"), Remaining);
        return;
    }

    LastCommitCompleteTime = Now;
    bCommitDiffInProgress = true;
    RefreshedFiles.Empty();
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
        bool bSuccess = WeakSubsystem->RequestFileListWithSizesAndRanks(-1, 15000, NewFiles, NewSizes, NewRanks);

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakThis, NewFiles, NewSizes, NewRanks, bSuccess]()
            {
                if (!WeakThis.IsValid() || !bSuccess || NewFiles.Num() == 0)
                {
                    if (WeakThis.IsValid()) WeakThis->bCommitDiffInProgress = false;
                    return;
                }

                WeakThis->DiffAndRefreshFileList(NewFiles, NewSizes, NewRanks, false);
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
}

void AJUSYNCFileSpawnerActor::DiffAndRefreshFileList(const TArray<FString>& NewFiles, const TArray<int64>& NewSizes, const TArray<int32>& NewRanks, bool bIsManual)
{
    TMap<FString, int64> OldSizes;
    for (int32 i = 0; i < FilteredFiles.Num(); ++i)
    {
        OldSizes.Add(FilteredFiles[i], FilteredSizes.IsValidIndex(i) ? FilteredSizes[i] : 0);
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

        if (!Fname.EndsWith(TEXT(".usda"))) { SkippedCount++; continue; }
        if (bIsManual == false && (Fname.StartsWith(TEXT("Session_")) || Fname == TEXT("scene.usda") ||
            Fname.Contains(TEXT("manifest")) || Fname.Contains(TEXT("images/")) || Fname.Contains(TEXT("shared/"))))
        { SkippedCount++; continue; }

        int64 OldSize = 0;
        int64* pOldSize = OldSizes.Find(Fname);
        bool bFound = pOldSize != nullptr;
        if (bFound) OldSize = *pOldSize;

        if (!bFound || NewSize != OldSize)
        {
            if (!bFound) NewCount++; else ChangedCount++;
            ChangedFiles.Add(Fname);
            RefreshRemainingFiles.Add(TPair<FString, int32>(Fname, Rank));
        }
    }

    FString Tag = bIsManual ? TEXT("[ManualRefresh]") : TEXT("[LiveUpdate]");
    FString Summary = FString::Printf(TEXT("%s %d changed, %d new, %d skipped"), *Tag, ChangedCount, NewCount, SkippedCount);
    UE_LOG(LogTemp, Display, TEXT("%s"), *Summary);
    GEngine->AddOnScreenDebugMessage(-1, 4.0f, bIsManual ? FColor::Green : FColor::Orange, Summary);

    if (ChangedFiles.Num() > 0)
    {
        RawFileList = NewFiles;
        RawFileSizes = NewSizes;
        RawFileRanks = NewRanks;
        FilteredFiles = NewFiles;
        FilteredSizes = NewSizes;
        FilteredRanks = NewRanks;
    }

    RefreshActive = 0;
    ChainRefreshNext();
    bCommitDiffInProgress = false;
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
        bool bSuccess = WeakSubsystem->RequestFileListWithSizesAndRanks(-1, 15000, NewFiles, NewSizes, NewRanks);

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakThis, NewFiles, NewSizes, NewRanks, bSuccess]()
            {
                if (!WeakThis.IsValid() || !bSuccess || NewFiles.Num() == 0)
                {
                    if (WeakThis.IsValid()) WeakThis->bCommitDiffInProgress = false;
                    return;
                }

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

    HandleCommitCompleteNotification();
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

    // Mark as refreshed to deduplicate
    RefreshedFiles.Add(Filename);

    UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] RefreshSingleFile START: '%s' (rank %d)"), *Filename, TargetRank);

    // Remove old actors from tracking — they'll be re-added when new ones spawn
    FString MeshKeySuffix = TEXT("|") + Filename;
    
    // Extract element name from clip filename: "clips/NAME_Geom__rRANK_TIMESTAMP.usda"
    FString PCKey;
    {
        FString NoExt = Filename;
        if (NoExt.EndsWith(TEXT(".usda"))) NoExt = NoExt.LeftChop(5);
        else if (NoExt.EndsWith(TEXT(".usd"))) NoExt = NoExt.LeftChop(4);
        
        int32 GeomIdx = NoExt.Find(TEXT("_Geom_"), ESearchCase::CaseSensitive);
        if (GeomIdx > 0)
        {
            // Strip leading path
            FString BaseName = FPaths::GetCleanFilename(NoExt.Left(GeomIdx));
            PCKey = FString::Printf(TEXT("%s_r%d"), *BaseName, TargetRank);
        }
    }
    
    TArray<AActor*> ActorsToDestroy;

    UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] PCKey='%s', MeshKeySuffix='%s'"), *PCKey, *MeshKeySuffix);

    for (auto It = FileToActorMap.CreateIterator(); It; ++It)
    {
        bool bMatch = It->Key.EndsWith(MeshKeySuffix, ESearchCase::CaseSensitive);
        if (!bMatch && !PCKey.IsEmpty())
            bMatch = (It->Key == PCKey);
        if (bMatch)
        {
            if (It->Value && It->Value->IsValidLowLevel())
            {
                ActorsToDestroy.Add(It->Value);
                UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Destroying old actor for key='%s'"), *(It->Key));
            }
            It.RemoveCurrent();
        }
    }

    for (AActor* OldActor : ActorsToDestroy)
    {
        SpawnedActors.Remove(OldActor);
        ActorsSpawned--;
        OldActor->Destroy();
    }

    UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Destroyed %d old actors (ActorsSpawned now %d)"), ActorsToDestroy.Num(), ActorsSpawned);

    // Download and re-spawn
    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    FString FilenameCopy = Filename;
    int32 RankCopy = TargetRank;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis, FilenameCopy, RankCopy]()
    {
        if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

        int32 Timeout = 30000;
        TArray<uint8> FileData;
        bool bSuccess = WeakSubsystem->RequestFile(FilenameCopy, RankCopy, Timeout, FileData);

        if (!bSuccess || FileData.Num() == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Failed to download updated file: '%s'"), *FilenameCopy);
            return;
        }

        TArray<FJUSYNCMeshData> MeshData;
        TArray<FJUSYNCPointCloudData> PointCloudData;
        FString Preview;
        bool bParsed = UJUSYNCBlueprintLibrary::LoadUSDFullFromBuffer(FileData, FilenameCopy, MeshData, PointCloudData, Preview);

        TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakCopy = WeakThis;
        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakCopy, FilenameCopy, bParsed, RankCopy, MeshData = MoveTemp(MeshData), PointCloudData = MoveTemp(PointCloudData)]() mutable
            {
                if (!WeakCopy.IsValid()) return;

                if (!bParsed || (MeshData.Num() == 0 && PointCloudData.Num() == 0))
                {
                    UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Parse returned no data for: '%s'"), *FilenameCopy);
                    return;
                }

                for (int32 i = 0; i < MeshData.Num(); ++i)
                {
                    if (!MeshData[i].IsValid()) continue;

                    FVector SpawnLoc = WeakCopy->GetNextSpawnLocation();
                    AActor* Spawned = UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(MeshData[i], SpawnLoc);

                    if (Spawned)
                    {
                        WeakCopy->SpawnedActors.Add(Spawned);
                        WeakCopy->ActorsSpawned++;
                        WeakCopy->FileToActorMap.Add(MeshData[i].ElementName + TEXT("|") + FilenameCopy, Spawned);
                        Spawned->SetActorEnableCollision(false);

                        if (WeakCopy->SpawnScale != FVector::ZeroVector)
                        {
                            FVector FinalScale = WeakCopy->bUseUniformScaling ? FVector(WeakCopy->SpawnScale.X) : WeakCopy->SpawnScale;
                            Spawned->SetActorScale3D(FinalScale);
                        }

                        WeakCopy->OnFileComplete.Broadcast(FilenameCopy, Spawned);
                        WeakCopy->NextSpawnIndex++;
                    }
                }

                if (WeakCopy->bSpawnPointClouds && PointCloudData.Num() > 0)
                {
                    UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
                    if (S && S->GetPointCloudSpawner())
                    {
                        FJUSYNCPointCloudSpawner* Spawner = S->GetPointCloudSpawner();
                        Spawner->SetSpawnLocation(WeakCopy->GetNextSpawnLocation());
                        Spawner->SetSpawnScale(WeakCopy->bUseUniformScaling ? WeakCopy->SpawnScale.X : 1.0f);

                        for (const FJUSYNCPointCloudData& PC : PointCloudData)
                        {
                            if (PC.IsValid())
                            {
                                WeakCopy->PendingAsyncPCS++;
                                Spawner->EnqueuePointCloud(PC, RankCopy);
                            }
                        }
                    }
                }

                FString ResultMsg = FString::Printf(TEXT("[LiveUpdate] Refreshed: %s"), *FilenameCopy);
                GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Green, ResultMsg);
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });

    return true;
}
