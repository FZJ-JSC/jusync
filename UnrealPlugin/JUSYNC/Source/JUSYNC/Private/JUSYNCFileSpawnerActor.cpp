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
    bSpawnPointClouds = true;
    bUseGradientColors = true;
    GradientPngFilename = TEXT("");
    PointCloudSize = 1.0f;
    bGradientReady = false;
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
    for (AActor* Actor : SpawnedActors)
        if (Actor) Actor->Destroy();
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

    int32 Col = NextSpawnIndex % 10;
    int32 Row = NextSpawnIndex / 10;
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

    // Separate PNG files (for gradient LUT) from USD files
    TArray<FString> PngFiles;
    TArray<int64> PngSizes;
    TArray<int32> PngRanks;

    if (bFilterUSDOnly)
    {
        UJUSYNCBlueprintLibrary::FilterFileListByExtensionEnumWithSizesAndRanks(RawFileList, RawFileSizes, RawFileRanks, EJUSYNCExtension::USD, OutFiles, OutSizes, OutRanks);
        UJUSYNCBlueprintLibrary::FilterFileListByExtensionEnumWithSizesAndRanks(RawFileList, RawFileSizes, RawFileRanks, EJUSYNCExtension::PNG, PngFiles, PngSizes, PngRanks);
        // Store PNG → rank mapping for later gradient download
        for (int32 i = 0; i < PngFiles.Num(); ++i)
        {
            GradientPngRankMap.Add(PngFiles[i], PngRanks.IsValidIndex(i) ? PngRanks[i] : 0);
        }
        RawFileList = MoveTemp(OutFiles); RawFileSizes = MoveTemp(OutSizes); RawFileRanks = MoveTemp(OutRanks);
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: after USD filter: %d files, %d PNGs"), RawFileList.Num(), PngFiles.Num());
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
        GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, TEXT("[Spawner] No files after filtering! Check filters."));
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

    // Throttle: limit concurrent downloads to 8 using atomic counter
    // CAS-based semaphore: only kMaxConcurrentDownloads can proceed at once
    static std::atomic<int32> ActiveDownloadCount{0};
    const int32 kMaxConcurrent = 8;

    for (int32 i = 0; i < FilteredFiles.Num(); ++i)
    {
        if (bIsCancelled) break;

        const FString& Filename = FilteredFiles[i];
        int32 TargetRank = FilteredRanks[i];
        int64 FileSize = FilteredSizes.IsValidIndex(i) ? FilteredSizes[i] : int64(1048576);
        int32 DynamicTimeout = CalculateDynamicTimeout(FileSize);

        TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
        int32 FileIndex = i;

        TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
        Async(EAsyncExecution::Thread, [WeakSubsystem, WeakThis, Filename, TargetRank, DynamicTimeout, FileIndex, kMaxConcurrent]()
            {
                // Spin until we can acquire a slot (on background thread, game thread free)
                int32 Current = 0;
                while (true)
                {
                    Current = ActiveDownloadCount.load(std::memory_order_acquire);
                    if (Current >= kMaxConcurrent)
                    {
                        FPlatformProcess::Sleep(0.02f);
                        continue;
                    }
                    if (ActiveDownloadCount.compare_exchange_weak(Current, Current + 1, std::memory_order_release))
                    {
                        break; // acquired slot
                    }
                    // CAS failed: Current was updated, retry loop
                }

                if (!WeakSubsystem.IsValid() || !WeakThis.IsValid())
                {
                    ActiveDownloadCount.fetch_sub(1, std::memory_order_release);
                    return;
                }

                TArray<uint8> FileData;
                bool bSuccess = WeakSubsystem->RequestFile(Filename, TargetRank, DynamicTimeout, FileData);

                ActiveDownloadCount.fetch_sub(1, std::memory_order_release);

                TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThisCopy = WeakThis;
                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [WeakThisCopy, Filename, FileData, bSuccess, FileIndex]()
                    {
                        if (!WeakThisCopy.IsValid()) return;
                        WeakThisCopy->OnSingleFileDownloaded(Filename, FileData, bSuccess, FileIndex);
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
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis, Filename, FileData, FileIndex]() mutable
        {
            if (!WeakThis.IsValid()) return;

            TArray<FJUSYNCMeshData> MeshData;
            TArray<FJUSYNCPointCloudData> PointCloudData;
            FString Preview;
            bool bParsed = UJUSYNCBlueprintLibrary::LoadUSDFullFromBuffer(FileData, Filename, MeshData, PointCloudData, Preview);

            // Dispatch lightweight spawn to game thread
            TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakCopy = WeakThis;
            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [WeakCopy, Filename, bParsed, MeshData = MoveTemp(MeshData), PointCloudData = MoveTemp(PointCloudData), FileIndex]() mutable
                {
                    if (!WeakCopy.IsValid()) return;
                    WeakCopy->SpawnMeshFromData(Filename, bParsed, MoveTemp(MeshData), MoveTemp(PointCloudData), FileIndex);
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
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

    Async(EAsyncExecution::Thread, [WeakSub, WeakThis, PngCopy, PngRank]()
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

                        // Now dispatch all buffered point clouds
                        TArray<FJUSYNCPointCloudData>& Pending = WeakThis->PendingPointClouds;
                        if (Pending.Num() > 0)
                        {
                            UE_LOG(LogTemp, Log, TEXT("[Spawner] Dispatching %d buffered point clouds with gradient"), Pending.Num());
                            for (FJUSYNCPointCloudData& PC : Pending)
                            {
                                Sp->EnqueuePointCloud(PC);
                            }
                            Pending.Empty();
                        }
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

void AJUSYNCFileSpawnerActor::SpawnMeshFromData(const FString& Filename, bool bParsed, TArray<FJUSYNCMeshData>&& MeshData, TArray<FJUSYNCPointCloudData>&& PointCloudData, int32 FileIndex)
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
                // Dispatch any buffered PCs
                TArray<FJUSYNCPointCloudData>& Pending = PendingPointClouds;
                if (Pending.Num() > 0)
                {
                    UE_LOG(LogTemp, Log, TEXT("[Spawner] Dispatching %d buffered PCs with gradient"), Pending.Num());
                    for (FJUSYNCPointCloudData& PC : Pending)
                    {
                        S->GetPointCloudSpawner()->EnqueuePointCloud(PC);
                    }
                    Pending.Empty();
                }
            }
        }
    }

    bool bHadMeshes = false;
    bool bHadPointClouds = false;

    // Async spawn meshes if any
    if (bParsed && MeshData.Num() > 0)
    {
        int32 ValidMeshCount = 0;
        for (const FJUSYNCMeshData& m : MeshData) if (m.IsValid()) ValidMeshCount++;

        if (ValidMeshCount > 0)
        {
            bHadMeshes = true;

            PendingAsyncSpawns++;
            FString FCopy = Filename;
            AsyncTask(ENamedThreads::GameThread, [this, FCopy, MeshData]()
            {
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

                        // Track for live updates
                        FileToActorMap.Add(MeshData[i].ElementName + TEXT("|") + FCopy, Spawned);

                        // Disable collision on spawned mesh
                        Spawned->SetActorEnableCollision(false);

                        if (SpawnScale != FVector::ZeroVector)
                        {
                            FVector FinalScale = bUseUniformScaling ? FVector(SpawnScale.X) : SpawnScale;
                            Spawned->SetActorScale3D(FinalScale);
                        }

                        ApplyDynamicMaterial(Cast<UPrimitiveComponent>(Spawned->GetRootComponent()), FCopy);
                        OnFileComplete.Broadcast(FCopy, Spawned);
                        NextSpawnIndex++;
                        SpawnCount++;

                        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: spawned mesh '%s' from '%s' (actor #%d)"),
                               *MeshData[i].ElementName, *FCopy, ActorsSpawned);
                    }
                }

                PendingAsyncSpawns--;
                if (PendingAsyncSpawns <= 0) PendingAsyncSpawns = 0;

                FString ResultMsg = FString::Printf(TEXT("[Spawner] %s: spawned %d meshes"), *FCopy, SpawnCount);
                GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green, ResultMsg);
            });
        }
    }

    // Async spawn point clouds if any
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

                    for (const FJUSYNCPointCloudData& PC : PointCloudData)
                    {
                        if (PC.IsValid())
                        {
                            PendingPointClouds.Add(PC);
                        }
                    }

                    UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: buffered %d PCs from '%s' (pending gradient LUT)"),
                           ValidPCCount, *Filename);
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

        // Track for live updates (use element name as key)
        if (!EleName.IsEmpty())
        {
            FileToActorMap.Add(EleName, Spawned);
        }

        OnFileComplete.Broadcast(EleName, Spawned);
        NextSpawnIndex++;

        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: loaded point cloud '%s' (actor #%d)"),
               *EleName, ActorsSpawned);
    }
}

void AJUSYNCFileSpawnerActor::CheckAllDownloadsComplete()
{
    if (FilesDownloaded >= FilesTotal)
    {
        // Wait for gradient to arrive before flushing (gives async PNG download time)
        if (PendingPointClouds.Num() > 0 && !bGradientReady)
        {
            UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
            bool bGradientExists = false;
            if (S && S->GetPointCloudSpawner())
                bGradientExists = S->GetPointCloudSpawner()->GetGradientLUT().Num() > 0;

            if (bGradientExists)
            {
                // Gradient exists on spawner but bGradientReady flag wasn't set — use it directly
                UE_LOG(LogTemp, Log, TEXT("[Spawner] bGradientReady=false but LUT exists (%d entries), dispatching %d PCs"),
                    S->GetPointCloudSpawner()->GetGradientLUT().Num(), PendingPointClouds.Num());
                bGradientReady = true;
                for (const auto& PC : PendingPointClouds)
                {
                    S->GetPointCloudSpawner()->EnqueuePointCloud(PC);
                }
                PendingPointClouds.Empty();
            }
            else
            {
                // Gradient not loaded yet — wait up to 10s for PNG download + decode
                TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
                FTimerHandle FlushHandle;
                FTimerDelegate FlushDelay;
                FlushDelay.BindLambda([WeakThis]()
                {
                    if (!WeakThis.IsValid() || WeakThis->PendingPointClouds.Num() == 0) return;

                    if (!WeakThis->bGradientReady)
                    {
                        // Check one more time if LUT appeared while waiting
                        UJUSYNCSubsystem* S2 = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
                        bool bNowReady = false;
                        if (S2 && S2->GetPointCloudSpawner())
                            bNowReady = S2->GetPointCloudSpawner()->GetGradientLUT().Num() > 0;

                        if (bNowReady)
                        {
                            WeakThis->bGradientReady = true;
                            UE_LOG(LogTemp, Log, TEXT("[Spawner] Gradient appeared after wait, dispatching %d PCs"),
                                WeakThis->PendingPointClouds.Num());
                            for (const auto& PC : WeakThis->PendingPointClouds)
                            {
                                S2->GetPointCloudSpawner()->EnqueuePointCloud(PC);
                            }
                            WeakThis->PendingPointClouds.Empty();
                        }
                        else
                        {
                            WeakThis->FlushBufferedPointClouds();
                        }
                    }

                    TWeakObjectPtr<AJUSYNCFileSpawnerActor> Wt = WeakThis;
                    FFunctionGraphTask::CreateAndDispatchWhenReady(
                        [Wt]() { if (Wt.IsValid()) Wt->CheckAllDownloadsComplete(); },
                        TStatId(), nullptr, ENamedThreads::GameThread);
                });
                if (GWorld)
                    GWorld->GetTimerManager().SetTimer(FlushHandle, FlushDelay, 10.0f, false);
                return;
            }
        }

        // Flush any remaining buffered PCs if gradient already arrived
        if (PendingPointClouds.Num() > 0)
        {
            FlushBufferedPointClouds();
        }

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
            FJUSYNCPointCloudSpawner* Spawner = Subsystem->GetPointCloudSpawner();
            Spawner->DrainReadyQueue();
        }

        // Only set delayed drain timer if there might be in-flight conversions still landing
        bool bHasPendingWork = PendingPointClouds.Num() > 0;
        if (bHasPendingWork && GWorld)
        {
            TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
            FTimerHandle FinalDrainHandle;
            FTimerDelegate FinalDrainDelay;
            FinalDrainDelay.BindLambda([WeakThis]()
            {
                if (!WeakThis.IsValid()) return;

                UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
                if (S && S->GetPointCloudSpawner())
                {
                    S->GetPointCloudSpawner()->DrainReadyQueue();
                }

                TWeakObjectPtr<AJUSYNCFileSpawnerActor> Wt = WeakThis;
                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [Wt]() { if (Wt.IsValid()) Wt->CheckAllDownloadsComplete(); },
                    TStatId(), nullptr, ENamedThreads::GameThread);
            });
            GWorld->GetTimerManager().SetTimer(FinalDrainHandle, FinalDrainDelay, 0.5f, false);
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

    // Skip non-geometry files — only clip USD files are worth refreshing
    if (!Filename.EndsWith(TEXT(".usda")) && !Filename.EndsWith(TEXT(".usd")))
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping non-USD file: '%s'"), *Filename);
        return;
    }
    if (Filename.StartsWith(TEXT("Session_")) || Filename == TEXT("scene.usda") ||
        Filename.Contains(TEXT("manifest")) || Filename.Contains(TEXT("images/")) ||
        Filename.Contains(TEXT("shared/")))
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping root/manifest file: '%s'"), *Filename);
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
    UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] Commit complete - re-fetching file list"));

    // Capture subsystem on game thread (lookup fails on background threads)
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem || !Subsystem->IsBrokerConnected())
    {
        UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Cannot refresh: broker not connected"));
        bCommitDiffInProgress = false;
        return;
    }

    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    Async(EAsyncExecution::Thread, [WeakSubsystem, WeakThis]()
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

                // Find files that are new or changed
                TMap<FString, int64> OldSizes;
                for (int32 i = 0; i < WeakThis->FilteredFiles.Num(); ++i)
                {
                    OldSizes.Add(WeakThis->FilteredFiles[i],
                                 WeakThis->FilteredSizes.IsValidIndex(i) ? WeakThis->FilteredSizes[i] : 0);
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

                    // Only refresh USD clip files — skip manifests, root scenes, images, etc.
                    if (!Fname.EndsWith(TEXT(".usda")) && !Fname.EndsWith(TEXT(".usd")))
                    {
                        SkippedCount++;
                        continue;
                    }
                    // Skip root scene files and shared manifests
                    if (Fname.StartsWith(TEXT("Session_")) || Fname == TEXT("scene.usda") ||
                        Fname.Contains(TEXT("manifest")) || Fname.Contains(TEXT("images/")) ||
                        Fname.Contains(TEXT("shared/")))
                    {
                        SkippedCount++;
                        continue;
                    }

                    int64 OldSize = 0;
                    int64* pOldSize = OldSizes.Find(Fname);
                    bool bFound = pOldSize != nullptr;
                    if (bFound) OldSize = *pOldSize;

                    if (!bFound || NewSize != OldSize)
                    {
                        if (!bFound) NewCount++;
                        else ChangedCount++;
                        ChangedFiles.Add(Fname);
                        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Detected change: '%s' (old: %lld, new: %lld)"),
                               *Fname, static_cast<long long>(OldSize), static_cast<long long>(NewSize));
                        WeakThis->RefreshSingleFile(Fname, Rank);
                    }
                }

                // Show summary on screen
                int32 TotalChanges = ChangedCount + NewCount;
                if (TotalChanges > 0)
                {
                    FString Summary = FString::Printf(TEXT("[LiveUpdate] %d changed, %d new, %d skipped"), ChangedCount, NewCount, SkippedCount);
                    if (ChangedFiles.Num() <= 3)
                    {
                        for (const FString& F : ChangedFiles)
                            Summary += FString::Printf(TEXT("\n  - %s"), *F);
                    }
                    else
                    {
                        for (int32 i = 0; i < 3 && i < ChangedFiles.Num(); ++i)
                            Summary += FString::Printf(TEXT("\n  - %s"), *ChangedFiles[i]);
                        Summary += FString::Printf(TEXT("\n  ... and %d more"), ChangedFiles.Num() - 3);
                    }
                    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Orange, Summary);
                }

                // Update stored file lists
                WeakThis->RawFileList = NewFiles;
                WeakThis->RawFileSizes = NewSizes;
                WeakThis->RawFileRanks = NewRanks;
                WeakThis->bCommitDiffInProgress = false;
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

    // Capture subsystem on game thread
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem || !Subsystem->IsBrokerConnected())
    {
        UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Cannot refresh '%s': broker not connected"), *Filename);
        return false;
    }

    // Find existing actor for this file and destroy it
    // Match by: key contains filename, OR filename contains key (handles point clouds keyed by element name)
    TArray<AActor*> ActorsToRemove;
    for (const auto& Pair : FileToActorMap)
    {
        if (Pair.Key.Contains(Filename) || Filename.Contains(Pair.Key))
        {
            if (Pair.Value && Pair.Value->IsValidLowLevel())
            {
                ActorsToRemove.Add(Pair.Value);
            }
        }
    }

    // Remove old entries
    for (auto It = FileToActorMap.CreateIterator(); It; ++It)
    {
        if (It->Key.Contains(Filename) || Filename.Contains(It->Key))
        {
            It.RemoveCurrent();
        }
    }

    // Destroy old actors
    for (AActor* OldActor : ActorsToRemove)
    {
        if (SpawnedActors.Contains(OldActor))
        {
            SpawnedActors.Remove(OldActor);
            ActorsSpawned--;
        }
        if (OldActor) OldActor->Destroy();
    }

    // Download and re-spawn
    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    FString FilenameCopy = Filename;
    int32 RankCopy = TargetRank;

    Async(EAsyncExecution::Thread, [WeakSubsystem, WeakThis, FilenameCopy, RankCopy]()
    {
        if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

        int64 FileSize = 1048576; // Default 1MB for timeout calc
        int32 Timeout = 30000;

        TArray<uint8> FileData;
        bool bSuccess = WeakSubsystem->RequestFile(FilenameCopy, RankCopy, Timeout, FileData);

        if (!bSuccess || FileData.Num() == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Failed to download updated file: '%s'"), *FilenameCopy);
            return;
        }

        // Parse on background thread
        TArray<FJUSYNCMeshData> MeshData;
        TArray<FJUSYNCPointCloudData> PointCloudData;
        FString Preview;
        bool bParsed = UJUSYNCBlueprintLibrary::LoadUSDFullFromBuffer(FileData, FilenameCopy, MeshData, PointCloudData, Preview);

        TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakCopy = WeakThis;
        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakCopy, FilenameCopy, bParsed, MeshData = MoveTemp(MeshData), PointCloudData = MoveTemp(PointCloudData)]() mutable
            {
                if (!WeakCopy.IsValid()) return;

                if (!bParsed || (MeshData.Num() == 0 && PointCloudData.Num() == 0))
                {
                    UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Parse returned no data for: '%s'"), *FilenameCopy);
                    return;
                }

                // Spawn meshes
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
                        UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] Refreshed mesh from '%s'"), *FilenameCopy);
                    }
                }

                // Spawn point clouds
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
                                Spawner->EnqueuePointCloud(PC);
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
