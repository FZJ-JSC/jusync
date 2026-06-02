#include "JUSYNCFileSpawnerActor.h"
#include <atomic>
#include <mutex>
#include "JUSYNCSubsystem.h"
#include "JUSYNCPointCloudSpawner.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"

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
    SpawnSpacing = 0.0f;
    SpawnMaterial = nullptr;
    TextureSampleParameterName = TEXT("");
    SpawnScale = FVector::OneVector;
    bUseUniformScaling = true;
    bAutoStart = true;
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
