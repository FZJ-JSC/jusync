#include "JUSYNCBenchmarkActor.h"

#include <atomic>
#include "JUSYNCSubsystem.h"
#include "JUSYNCPointCloudSpawner.h"
#include "JUSYNCModule.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMemory.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "TimerManager.h"

// Byte conversion for GB-based memory queries
#define GB_TO_BYTES(x) static_cast<int64>((x) * 1073741824.0)

AJUSYNCBenchmarkActor::AJUSYNCBenchmarkActor()
{
    PrimaryActorTick.bCanEverTick = false;

    BrokerEndpoint = TEXT("tcp://localhost:5556");
    RequestTimeoutMs = 5000;
    BandwidthBytesPerSecond = 10737418240.0f; // 10GB/s
    MinimumFileSizeBytes = 1000;
    bFilterUSDOnly = true;
    bClipsOnly = true;
    PipelineDepth = 10;

    WarmupRuns = 1;
    MeasuredRuns = 5;
    OutputDirectory = TEXT("");
    OutputFormat = 2; // Both CSV and JSON
    bAutoStart = true;
    bGarbageCollectBetweenRuns = true;

    BaseSpawnLocation = FVector::ZeroVector;
    SpawnSpacing = 0.0f;
    SpawnGridColumns = 10;
    SpawnScale = FVector::OneVector;
    bUseUniformScaling = true;
    SpawnMaterial = nullptr;
    bSpawnPointClouds = true;
    PointCloudSize = 1.0f;
    bUseGradientColors = true;
    MaxSpawnsPerFrame = 10;

    CompletedRuns = 0;
    bIsRunning = false;
    ActorsSpawned = 0;
    bIsCancelled = false;
    bGradientReady = false;
    bGradientAttempted = false;
}

void AJUSYNCBenchmarkActor::BeginPlay()
{
    Super::BeginPlay();

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (Subsystem && Subsystem->GetPointCloudSpawner())
    {
        FJUSYNCPointCloudSpawner* Spawner = Subsystem->GetPointCloudSpawner();
        Spawner->OnPointCloudSpawned.Clear();
        Spawner->OnPointCloudSpawned.AddUObject(this, &AJUSYNCBenchmarkActor::OnPointCloudSpawnedHandler);
    }

    if (bAutoStart)
    {
        StartBenchmarkRun();
    }
}

void AJUSYNCBenchmarkActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->OnNotificationReceived.RemoveAll(this);
    }

    CancelBenchmark();
    ClearSpawnedActors();
    Super::EndPlay(EndPlayReason);
}

// ========== MEMORY / TIMING HELPERS ==========

void AJUSYNCBenchmarkActor::SnapshotRAMAndVRAM(int64& OutRAM, int64& OutVRAM)
{
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (Subsystem)
    {
        OutRAM = Subsystem->GetSystemRAMUsage_GB() > 0.0f ? GB_TO_BYTES(Subsystem->GetSystemRAMUsage_GB()) : 0;
        OutVRAM = Subsystem->GetVRAMUsage_GB() > 0.0f ? GB_TO_BYTES(Subsystem->GetVRAMUsage_GB()) : 0;
    }
    else
    {
        OutRAM = 0;
        OutVRAM = 0;
    }
}

void AJUSYNCBenchmarkActor::StartStage()
{
    StageStartTime = FPlatformTime::Seconds();
}

void AJUSYNCBenchmarkActor::EndStage(double& OutMs)
{
    const double Now = FPlatformTime::Seconds();
    OutMs = (Now - StageStartTime) * 1000.0;
}

// ========== BENCHMARK LIFECYCLE ==========

void AJUSYNCBenchmarkActor::StartBenchmarkRun()
{
    if (bIsRunning)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("JUSYNC Bench Actor: already running"));
        return;
    }

    TotalRuns = WarmupRuns + MeasuredRuns;
    if (TotalRuns < 1)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Bench Actor: WarmupRuns + MeasuredRuns must be >= 1"));
        return;
    }

    bIsRunning = true;
    bIsCancelled = false;
    RunIndex = 0;
    CompletedRuns = 0;
    TotalBytesAllRuns = 0;
    TotalTriangleCount = 0;
    TotalVertexCount = 0;
    TotalPCPoints = 0;
    SessionStartTime = FPlatformTime::Seconds();

    // Take system baseline
    SnapshotRAMAndVRAM(SessionRAMStart, SessionVRAMStart);

    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Bench Actor: starting benchmark (%d warmup + %d measured runs)"),
        WarmupRuns, MeasuredRuns);

    bIsWarmup = (RunIndex < WarmupRuns);
    RunPipelineIteration();
}

void AJUSYNCBenchmarkActor::CancelBenchmark()
{
    bIsCancelled = true;
    bIsRunning = false;
    PendingAsyncPCS = 0;
}

void AJUSYNCBenchmarkActor::ClearSpawnedActors()
{
    const double TeardownStart = FPlatformTime::Seconds();

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->ClearProcessedFiles();
        if (Subsystem->GetPointCloudSpawner())
        {
            Subsystem->GetPointCloudSpawner()->DestroyAllActors();
        }
    }

    for (AActor* Actor : SpawnedActors)
    {
        if (Actor && Actor->IsValidLowLevel())
        {
            Actor->Destroy();
        }
    }
    SpawnedActors.Empty();
    ActorsSpawned = 0;

    if (bGarbageCollectBetweenRuns)
    {
        GEngine->ForceGarbageCollection(true);
    }

    CurrentRunTimings.TeardownMs += (FPlatformTime::Seconds() - TeardownStart) * 1000.0;
}

void AJUSYNCBenchmarkActor::RunPipelineIteration()
{
    if (bIsCancelled) return;

    // Reset per-run counters
    FilteredFiles.Empty();
    FilteredSizes.Empty();
    FilteredRanks.Empty();
    RawHashLo.Empty();
    RawHashHi.Empty();
    GradientPngRankMap.Empty();
    FailedFileIndices.Empty();
    ParseFailedIndices.Empty();
    bGradientReady = false;
    bGradientAttempted = false;
    FilesDownloaded = 0;
    FilesTotal = 0;
    PipelineNextIndex = 0;
    PipelineActive = 0;
    PendingAsyncPCS = 0;
    NextSpawnIndex = 0;
    CurrentRunTimings = FRunTimings();
    TotalVertexCount = 0;
    TotalTriangleCount = 0;
    TotalPCPoints = 0;

    ClearSpawnedActors();

    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Bench Actor: run %d/%d (%s)"), RunIndex + 1, TotalRuns,
        bIsWarmup ? TEXT("warmup") : TEXT("measured"));

    ConnectToBroker();
}

void AJUSYNCBenchmarkActor::ConnectToBroker()
{
    if (bIsCancelled) return;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        OnFileListError(TEXT("Subsystem not available"));
        return;
    }

    StartStage();

    bool bInitialized = Subsystem->InitializeMiddleware();
    if (!bInitialized)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Bench Actor: middleware init failed"));
        bIsRunning = false;
        if (OnBenchmarkComplete.IsBound()) OnBenchmarkComplete.Broadcast(0, false);
        return;
    }

    bool bConnected = Subsystem->ConnectToBroker(BrokerEndpoint, FMath::Max(1000, RequestTimeoutMs));
    EndStage(CurrentRunTimings.ConnectMs);

    if (!bConnected)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Bench Actor: failed to connect to broker at %s"), *BrokerEndpoint);
        bIsRunning = false;
        if (OnBenchmarkComplete.IsBound()) OnBenchmarkComplete.Broadcast(0, false);
        return;
    }

    RequestFileList();
}

void AJUSYNCBenchmarkActor::RequestFileList()
{
    if (bIsCancelled) return;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        OnFileListError(TEXT("Subsystem not available"));
        return;
    }

    StartStage();

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    TWeakObjectPtr<AJUSYNCBenchmarkActor> WeakThis = this;

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

void AJUSYNCBenchmarkActor::OnFileListReceived_Internal(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks, bool bSuccess)
{
    if (bIsCancelled) return;

    if (!bSuccess || FileList.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Bench Actor: failed to retrieve file list"));
        bIsRunning = false;
        if (OnBenchmarkComplete.IsBound()) OnBenchmarkComplete.Broadcast(0, false);
        return;
    }

    // Record file list stage timing
    EndStage(CurrentRunTimings.FileListMs);
    CurrentRunTimings.FilesRequested = FileList.Num();

    // Single-pass filtering (mirrors FileSpawnerActor)
    TArray<FString> PngFiles;
    TArray<int32> PngRanks;
    TArray<FString> LocalFiles;
    TArray<int64> LocalSizes;
    TArray<int32> LocalRanks;
    TArray<uint64> LocalHashLo;
    TArray<uint64> LocalHashHi;
    LocalFiles.Reserve(FileList.Num());

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
        CurrentRunTimings.TotalBytes += Fsize;
    }

    for (int32 i = 0; i < PngFiles.Num(); ++i)
    {
        GradientPngRankMap.Add(PngFiles[i], PngRanks[i]);
    }

    FilteredFiles = LocalFiles;
    FilteredSizes = LocalSizes;
    FilteredRanks = LocalRanks;
    RawHashLo = LocalHashLo;
    RawHashHi = LocalHashHi;
    FilesTotal = FilteredFiles.Num();

    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Bench Actor: %d files after filtering (total %d), %d PNGs"),
        FilesTotal, FileList.Num(), PngFiles.Num());

    if (FilteredFiles.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("JUSYNC Bench Actor: no files after filtering"));
        FinalizeRun();
        return;
    }

    if (GradientPngRankMap.Num() > 0)
    {
        UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
        if (Subsystem)
        {
            DownloadGradientPng(Subsystem);
        }
    }

    DownloadStageStart = FPlatformTime::Seconds();
    ProcessAndDownloadFiles();
}

void AJUSYNCBenchmarkActor::OnFileListError(const FString& ErrorMessage)
{
    UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Bench Actor: file list error: %s"), *ErrorMessage);
    bIsRunning = false;
    if (OnBenchmarkComplete.IsBound())
    {
        OnBenchmarkComplete.Broadcast(0, false);
    }
}

void AJUSYNCBenchmarkActor::ProcessAndDownloadFiles()
{
    if (bIsCancelled || FilteredFiles.Num() == 0) return;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Bench Actor: subsystem not available"));
        return;
    }

    PipelineNextIndex = 0;
    PipelineActive = 0;

    const int32 InitialBatch = FMath::Min(PipelineDepth, FilteredFiles.Num());
    for (int32 i = 0; i < InitialBatch; ++i)
    {
        PipelineDownloadNext(Subsystem);
    }
}

void AJUSYNCBenchmarkActor::PipelineDownloadNext(UJUSYNCSubsystem* Subsystem)
{
    if (!Subsystem || bIsCancelled) return;
    if (PipelineNextIndex >= FilteredFiles.Num()) return;

    const int32 i = PipelineNextIndex++;
    const FString& Filename = FilteredFiles[i];
    const int32 TargetRank = FilteredRanks[i];
    const int64 FileSize = FilteredSizes.IsValidIndex(i) ? FilteredSizes[i] : int64(1048576);
    const int32 DynamicTimeout = CalculateDynamicTimeout(FileSize);

    PipelineActive++;
    TWeakObjectPtr<AJUSYNCBenchmarkActor> WeakThis = this;
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis, Filename, TargetRank, DynamicTimeout, FileIndex = i]()
    {
        if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

        TArray<uint8> FileData;
        bool bSuccess = WeakSubsystem->RequestFile(Filename, TargetRank, DynamicTimeout, FileData);

        TWeakObjectPtr<AJUSYNCBenchmarkActor> WeakThisCopy = WeakThis;
        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakThisCopy, Filename, FileData, bSuccess, FileIndex, TargetRank]()
            {
                if (!WeakThisCopy.IsValid()) return;
                WeakThisCopy->OnSingleFileDownloaded(Filename, FileData, bSuccess, FileIndex, TargetRank);
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
}

void AJUSYNCBenchmarkActor::OnSingleFileDownloaded(const FString& Filename, const TArray<uint8>& FileData, bool bSuccess, int32 FileIndex, int32 TargetRank)
{
    if (bIsCancelled) return;

    PipelineActive = FMath::Max(0, PipelineActive - 1);
    FilesDownloaded++;

    if (!bSuccess || FileData.Num() == 0)
    {
        if (!FailedFileIndices.Contains(FileIndex))
            FailedFileIndices.Add(FileIndex);
        ChainNextOrComplete();
        return;
    }

    NextSpawnIndex = FileIndex;

    // Move heavy USD parse to background thread
    TWeakObjectPtr<AJUSYNCBenchmarkActor> WeakThis = this;
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis, Filename, FileData, FileIndex, TargetRank]() mutable
    {
        if (!WeakThis.IsValid()) return;

        // Parse timing measured on the background thread
        const double ParseStart = FPlatformTime::Seconds();

        TArray<FJUSYNCMeshData> MeshData;
        TArray<FJUSYNCPointCloudData> PointCloudData;
        FString Preview;
        bool bParsed = UJUSYNCBlueprintLibrary::LoadUSDFullFromBufferNoCopy(FileData, Filename, MeshData, PointCloudData, Preview);

        const double ParseEnd = FPlatformTime::Seconds();
        const double ParseMs = (ParseEnd - ParseStart) * 1000.0;

        TWeakObjectPtr<AJUSYNCBenchmarkActor> WeakCopy = WeakThis;
        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakCopy, Filename, bParsed, FileIndex, TargetRank, ParseMs,
             MeshData = MoveTemp(MeshData), PointCloudData = MoveTemp(PointCloudData)]() mutable
            {
                if (!WeakCopy.IsValid()) return;
                WeakCopy->CurrentRunTimings.ParseMs += ParseMs;
                WeakCopy->SpawnMeshFromData(Filename, bParsed, MoveTemp(MeshData), MoveTemp(PointCloudData), FileIndex, TargetRank);
                WeakCopy->ChainNextOrComplete();
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
}

void AJUSYNCBenchmarkActor::ChainNextOrComplete()
{
    if (PipelineNextIndex < FilteredFiles.Num())
    {
        UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
        if (S)
        {
            PipelineDownloadNext(S);
        }
    }
    CheckIterationComplete();
}

void AJUSYNCBenchmarkActor::SpawnMeshFromData(const FString& Filename, bool bParsed, TArray<FJUSYNCMeshData>&& MeshData, TArray<FJUSYNCPointCloudData>&& PointCloudData, int32 FileIndex, int32 TargetRank)
{
    if (bIsCancelled) return;

    // Gradient after first PC parse
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
                S->GetPointCloudSpawner()->RecolorGradientPendingActors();
            }
        }
    }

    const double MeshSpawnStart = FPlatformTime::Seconds();

    // Spawn meshes on game thread
    if (bParsed && MeshData.Num() > 0)
    {
        int32 ValidMeshCount = 0;
        for (const FJUSYNCMeshData& m : MeshData) if (m.IsValid()) ValidMeshCount++;
        if (ValidMeshCount > 0)
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
                    CurrentRunTimings.MeshCount++;
                    CurrentRunTimings.TotalBytes += static_cast<int64>(MeshData[i].Vertices.Num()) * sizeof(FVector);
                    TotalVertexCount += MeshData[i].Vertices.Num();
                    TotalTriangleCount += MeshData[i].Triangles.Num() / 3;

                    Spawned->SetActorEnableCollision(false);
                    if (SpawnScale != FVector::ZeroVector)
                    {
                        Spawned->SetActorScale3D(bUseUniformScaling ? FVector(SpawnScale.X) : SpawnScale);
                    }

                    ApplyDynamicMaterial(Cast<UPrimitiveComponent>(Spawned->GetRootComponent()), Filename);
                    NextSpawnIndex++;
                    SpawnCount++;
                }
            }
        }
    }

    CurrentRunTimings.MeshSpawnMs += (FPlatformTime::Seconds() - MeshSpawnStart) * 1000.0;

    // Dispatch point clouds (async via spawner)
    if (bSpawnPointClouds && PointCloudData.Num() > 0)
    {
        StartStage();
        int32 ValidPCCount = 0;
        for (const FJUSYNCPointCloudData& pc : PointCloudData) if (pc.IsValid()) ValidPCCount++;
        if (ValidPCCount > 0)
        {
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
                            PendingAsyncPCS++;
                            CurrentRunTimings.PCCount++;
                            CurrentRunTimings.PCPoints += PC.PointCount;
                            TotalPCPoints += PC.PointCount;
                            Spawner->EnqueuePointCloud(PC, TargetRank);
                        }
                    }
                }
            }
        }
        CurrentRunTimings.PCSpawnMs += (FPlatformTime::Seconds() - StageStartTime) * 1000.0;
    }
}

void AJUSYNCBenchmarkActor::ApplyDynamicMaterial(UPrimitiveComponent* Comp, const FString& Filename)
{
    if (!Comp || !SpawnMaterial) return;

    const double MatStart = FPlatformTime::Seconds();
    UMaterialInstanceDynamic* DynamicMat = UMaterialInstanceDynamic::Create(SpawnMaterial, this);
    if (DynamicMat)
    {
        Comp->SetMaterial(0, DynamicMat);
        CurrentRunTimings.Materials++;
    }
    const double MatEnd = FPlatformTime::Seconds();
    CurrentRunTimings.MaterialMs += (MatEnd - MatStart) * 1000.0;
}

void AJUSYNCBenchmarkActor::DownloadGradientPng(UJUSYNCSubsystem* Subsystem)
{
    if (!Subsystem) return;

    FString PngPath;
    int32 PngRank = 0;
    if (GradientPngRankMap.Num() > 0)
    {
        for (auto It = GradientPngRankMap.CreateConstIterator(); It; ++It)
        {
            PngPath = It.Key();
            PngRank = It.Value();
            break;
        }
    }

    if (PngPath.IsEmpty()) return;

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSub = Subsystem;
    TWeakObjectPtr<AJUSYNCBenchmarkActor> WeakThis = this;
    const FString PngCopy = PngPath;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSub, WeakThis, PngCopy, PngRank]()
    {
        if (!WeakSub.IsValid() || !WeakThis.IsValid()) return;

        TArray<uint8> PngData;
        bool bOk = false;
        for (int32 Retry = 0; Retry < 3; ++Retry)
        {
            if (Retry > 0) FPlatformProcess::Sleep(1.0f * Retry);
            bOk = WeakSub->RequestFile(PngCopy, PngRank, 15000, PngData);
            if (bOk && PngData.Num() > 0) break;
            PngData.Empty();
        }

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
                    const int64 W = Img->GetWidth();
                    LUT.Reserve(FMath::Min(W, 256));
                    for (int64 x = 0; x < W && x < 256; ++x)
                    {
                        const uint8* Pixel = RawData.GetData() + x * 4;
                        LUT.Add(FColor(Pixel[2], Pixel[1], Pixel[0], 255));
                    }
                }
            }
        }

        if (LUT.Num() == 0)
        {
            UJUSYNCSubsystem* S = WeakSub.Get();
            if (S)
            {
                S->ApplyCachedGradientToSpawner();
                FJUSYNCPointCloudSpawner* Sp = S->GetPointCloudSpawner();
                if (Sp && Sp->GetGradientLUT().Num() > 0)
                {
                    LUT = Sp->GetGradientLUT();
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
                    if (!S || !S->GetPointCloudSpawner()) return;
                    S->GetPointCloudSpawner()->SetGradientLUT(LUT);
                    WeakThis->bGradientReady = true;
                    S->GetPointCloudSpawner()->RecolorGradientPendingActors();
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        }
    });
}

void AJUSYNCBenchmarkActor::OnPointCloudSpawnedHandler(const FString& EleName, AActor* Spawned)
{
    if (Spawned)
    {
        SpawnedActors.Add(Spawned);
        ActorsSpawned++;
    }

    PendingAsyncPCS--;
    if (PendingAsyncPCS < 0) PendingAsyncPCS = 0;

    CheckIterationComplete();
}

void AJUSYNCBenchmarkActor::CheckIterationComplete()
{
    if (bIsCancelled || !bIsRunning) return;

    // Not complete until all files downloaded, pipeline drained, and all PCs spawned
    if (FilesDownloaded < FilesTotal || PipelineActive > 0 || PendingAsyncPCS > 0)
    {
        return;
    }

    // Ensure the point cloud spawner queue has been drained to actors
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (Subsystem && Subsystem->GetPointCloudSpawner())
    {
        Subsystem->GetPointCloudSpawner()->DrainReadyQueue();
    }

    // One final check after drain (drain may enqueue more to spawn asynchronously)
    if (PendingAsyncPCS > 0)
    {
        TWeakObjectPtr<AJUSYNCBenchmarkActor> WeakThis = this;
        FTimerDelegate WaitDelay;
        WaitDelay.BindLambda([WeakThis]()
        {
            TWeakObjectPtr<AJUSYNCBenchmarkActor> Wt = WeakThis;
            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [Wt]() { if (Wt.IsValid()) Wt->CheckIterationComplete(); },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
        if (GWorld)
        {
            GWorld->GetTimerManager().SetTimer(CompleteWaitTimerHandle, WaitDelay, 0.2f, false);
        }
        return;
    }

    // End download stage accounts for the full download+parse+spawn window
    CurrentRunTimings.DownloadMs = (FPlatformTime::Seconds() - DownloadStageStart) * 1000.0;

    FinalizeRun();
}

void AJUSYNCBenchmarkActor::FinalizeRun()
{
    if (bIsCancelled) return;

    // Total time for the iteration = file-list completion -> full pipeline complete.
    // Reuse the download window if it was captured; otherwise fall back to stage sum.
    if (DownloadStageStart > 0.0)
    {
        CurrentRunTimings.TotalMs = (FPlatformTime::Seconds() - DownloadStageStart) * 1000.0;
    }
    else
    {
        CurrentRunTimings.TotalMs = CurrentRunTimings.ConnectMs + CurrentRunTimings.FileListMs +
            CurrentRunTimings.DownloadMs + CurrentRunTimings.ParseMs + CurrentRunTimings.MeshSpawnMs +
            CurrentRunTimings.PCSpawnMs + CurrentRunTimings.MaterialMs;
    }

    // Save to BenchmarkResults (only measured runs)
    if (!bIsWarmup)
    {
        ComputeAndRecordResult();
    }

    CompletedRuns++;
    RunIndex++;
    TotalBytesAllRuns += CurrentRunTimings.TotalBytes;

    if (RunIndex < TotalRuns)
    {
        // Advance to next run
        bIsWarmup = (RunIndex < WarmupRuns);
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Bench Actor: run %d complete, moving to run %d"), RunIndex, RunIndex + 1);
        BeginNextRun();
    }
    else
    {
        // Benchmark complete
        bIsRunning = false;
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Bench Actor: benchmark complete (%d runs, %d measured)"), TotalRuns, MeasuredRuns);

        // Export results
        if (!OutputDirectory.IsEmpty())
        {
            switch (OutputFormat)
            {
            case 0: SaveResultsToCSV(OutputDirectory); break;
            case 1: SaveResultsToJSON(OutputDirectory); break;
            case 2:
            default:
                SaveResultsToCSV(OutputDirectory);
                SaveResultsToJSON(OutputDirectory);
                break;
            }
        }

        ClearSpawnedActors();
        if (OnBenchmarkComplete.IsBound())
        {
            OnBenchmarkComplete.Broadcast(ActorsSpawned, true);
        }
    }
}

void AJUSYNCBenchmarkActor::BeginNextRun()
{
    if (bIsCancelled) return;

    TWeakObjectPtr<AJUSYNCBenchmarkActor> WeakThis = this;
    FTimerDelegate NextDelay;
    NextDelay.BindLambda([WeakThis]()
    {
        TWeakObjectPtr<AJUSYNCBenchmarkActor> Wt = WeakThis;
        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [Wt]() { if (Wt.IsValid()) Wt->RunPipelineIteration(); },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
    if (GWorld)
    {
        GWorld->GetTimerManager().SetTimer(CompleteWaitTimerHandle, NextDelay, 0.5f, false);
    }
}

// ========== TIMING HELPERS ==========

int32 AJUSYNCBenchmarkActor::CalculateDynamicTimeout(int64 FileSizeBytes) const
{
    const int32 BaseTimeout = RequestTimeoutMs;
    const int32 MaxTimeout = 600000;
    const float TimeForTransfer = (static_cast<float>(FileSizeBytes) / FMath::Max(1.0f, BandwidthBytesPerSecond)) * 1000.0f;
    const int32 DynamicTimeout = BaseTimeout + static_cast<int32>(TimeForTransfer);
    return FMath::Clamp(DynamicTimeout, RequestTimeoutMs, MaxTimeout);
}

FVector AJUSYNCBenchmarkActor::GetNextSpawnLocation() const
{
    FVector Origin = BaseSpawnLocation;
    if (GetWorld() && !HasAnyFlags(RF_ClassDefaultObject))
    {
        Origin = GetActorLocation();
    }

    const int32 Col = NextSpawnIndex % SpawnGridColumns;
    const int32 Row = NextSpawnIndex / SpawnGridColumns;
    return Origin + FVector(Col * SpawnSpacing, Row * SpawnSpacing, 0.0f);
}

void AJUSYNCBenchmarkActor::ComputeAndRecordResult()
{
    FJUSYNCBenchmarkResult Result;
    Result.TestName = TEXT("JUSYNC_Pipeline");
    Result.Timestamp = FDateTime::Now();
    Result.TotalTimeMs = static_cast<float>(CurrentRunTimings.TotalMs);
    Result.ConnectTimeMs = static_cast<float>(CurrentRunTimings.ConnectMs);
    Result.FileListTimeMs = static_cast<float>(CurrentRunTimings.FileListMs);
    Result.DownloadTimeMs = static_cast<float>(CurrentRunTimings.DownloadMs);
    Result.ParseTimeMs = static_cast<float>(CurrentRunTimings.ParseMs);
    Result.MeshSpawnTimeMs = static_cast<float>(CurrentRunTimings.MeshSpawnMs);
    Result.PointCloudSpawnTimeMs = static_cast<float>(CurrentRunTimings.PCSpawnMs);
    Result.MaterialTimeMs = static_cast<float>(CurrentRunTimings.MaterialMs);

    Result.TriangleCount = TotalTriangleCount;
    Result.VertexCount = TotalVertexCount;
    Result.ActorCount = ActorsSpawned;
    Result.MeshCount = CurrentRunTimings.MeshCount;
    Result.PointCloudCount = CurrentRunTimings.PCCount;
    Result.PointCloudPointsSpawned = CurrentRunTimings.PCPoints;
    Result.MaterialsCreated = CurrentRunTimings.Materials;

    Result.FilesRequested = CurrentRunTimings.FilesRequested;
    Result.FilesDownloaded = FilesDownloaded;
    Result.TotalBytesDownloaded = CurrentRunTimings.TotalBytes;

    if (CurrentRunTimings.DownloadMs > 0.0 && CurrentRunTimings.TotalBytes > 0)
    {
        Result.NetworkThroughput_MBps = static_cast<float>(
            (static_cast<double>(CurrentRunTimings.TotalBytes) / (CurrentRunTimings.DownloadMs / 1000.0)) / 1048576.0);
    }

    Result.ErrorCount = FailedFileIndices.Num() + ParseFailedIndices.Num();
    Result.SuccessRate = FilesTotal > 0 ? (static_cast<float>(FilesDownloaded) / static_cast<float>(FilesTotal)) * 100.0f : 0.0f;
    Result.HitchCount = CurrentRunTimings.Hitches;
    Result.MaxHitchDurationMs = static_cast<float>(CurrentRunTimings.MaxHitchMs);
    Result.FrameTimeMs = static_cast<float>(CurrentRunTimings.TotalMs);
    Result.FPS = CurrentRunTimings.TotalMs > 0.0 ? 1000.0f / static_cast<float>(CurrentRunTimings.TotalMs) : 0.0f;

    int64 RAMNow = 0, VRAMNow = 0;
    SnapshotRAMAndVRAM(RAMNow, VRAMNow);
    Result.RAMBeforeBytes = SessionRAMStart;
    Result.RAMAfterBytes = RAMNow;
    Result.VRAMBeforeBytes = SessionVRAMStart;
    Result.VRAMAfterBytes = VRAMNow;
    Result.RAMPeakBytes = RAMNow > SessionRAMStart ? RAMNow : SessionRAMStart;
    Result.VRAMPeakBytes = VRAMNow > SessionVRAMStart ? VRAMNow : SessionVRAMStart;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Result.CPUUsagePercent = Subsystem->GetCPUUsage_Percent();
        Result.GPUUsagePercent = Subsystem->GetGPUUsage_Percent();
    }

    BenchmarkResults.Add(Result);

    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan,
        FString::Printf(TEXT("[JUSYNC Bench] Run done: %.1fms total, %.1fms download, %d actors"),
            Result.TotalTimeMs, Result.DownloadTimeMs, Result.ActorCount));
}

TArray<FJUSYNCBenchmarkResult> AJUSYNCBenchmarkActor::GetResults() const
{
    return BenchmarkResults;
}

void AJUSYNCBenchmarkActor::ComputeStageStats(const TArray<float>& InValues, float& OutMin, float& OutMedian, float& OutMax, float& OutStdDev, float& OutP95)
{
    OutMin = 0.0f; OutMedian = 0.0f; OutMax = 0.0f; OutStdDev = 0.0f; OutP95 = 0.0f;
    if (InValues.Num() == 0) return;
    TArray<float> Sorted = InValues;
    Sorted.Sort();
    OutMin = Sorted[0];
    OutMax = Sorted[Sorted.Num() - 1];
    OutMedian = Sorted[Sorted.Num() / 2];
    double Mean = 0.0;
    for (float V : InValues) Mean += V;
    Mean /= InValues.Num();
    double VarSum = 0.0;
    for (float V : InValues) VarSum += (V - Mean) * (V - Mean);
    OutStdDev = static_cast<float>(FMath::Sqrt(VarSum / InValues.Num()));
    int32 P95Idx = FMath::Clamp(static_cast<int32>(FMath::CeilToFloat(Sorted.Num() * 0.95f)) - 1, 0, Sorted.Num() - 1);
    OutP95 = Sorted[P95Idx];
}

void AJUSYNCBenchmarkActor::SaveResultsToCSV(const FString& InDirectory)
{
    if (BenchmarkResults.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("No benchmark results to save"));
        return;
    }
    FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*InDirectory);
    FString Filename = TEXT("benchmark_results_") + FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")) + TEXT(".csv");
    FString Path = FPaths::Combine(InDirectory, Filename);
    FString Data = TEXT("TestName,Timestamp,TotalTimeMs,ConnectTimeMs,FileListTimeMs,DownloadMs,ParseMs,MeshSpawnMs,PCSpawnMs,MaterialMs,TeardownMs,ThroughputMBps,FilesReq,FilesDone,BytesDownloaded,Triangles,Vertices,Actors,Meshes,PCs,PCPoints,Materials,RAMBefore,RAMAfter,VRAMBefore,VRAMAfter,CPU%,GPU%,Errors,Success%,Hitches,MaxHitchMs,FrameTimeMs,FPS\n");
    for (const auto& R : BenchmarkResults)
    {
        Data += FString::Printf(TEXT("%s,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%lld,%d,%d,%d,%d,%d,%d,%d,%lld,%lld,%lld,%lld,%.1f,%.1f,%d,%.1f,%d,%.2f,%.2f,%.1f\n"),
            *R.TestName, *R.Timestamp.ToString(TEXT("%Y-%m-%d %H:%M:%S")),
            R.TotalTimeMs, R.ConnectTimeMs, R.FileListTimeMs, R.DownloadTimeMs, R.ParseTimeMs,
            R.MeshSpawnTimeMs, R.PointCloudSpawnTimeMs, R.MaterialTimeMs, R.TeardownTimeMs,
            R.NetworkThroughput_MBps, R.FilesRequested, R.FilesDownloaded, R.TotalBytesDownloaded,
            R.TriangleCount, R.VertexCount, R.ActorCount, R.MeshCount, R.PointCloudCount,
            R.PointCloudPointsSpawned, R.MaterialsCreated,
            R.RAMBeforeBytes, R.RAMAfterBytes, R.VRAMBeforeBytes, R.VRAMAfterBytes,
            R.CPUUsagePercent, R.GPUUsagePercent, R.ErrorCount, R.SuccessRate,
            R.HitchCount, R.MaxHitchDurationMs, R.FrameTimeMs, R.FPS);
    }
    if (FFileHelper::SaveStringToFile(Data, *Path))
        UE_LOG(LogJUSYNC, Log, TEXT("Benchmark CSV saved: %s"), *Path);
}

void AJUSYNCBenchmarkActor::SaveResultsToJSON(const FString& InDirectory)
{
    if (BenchmarkResults.Num() == 0) return;
    FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*InDirectory);
    FString Filename = TEXT("benchmark_results_") + FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")) + TEXT(".json");
    FString Path = FPaths::Combine(InDirectory, Filename);
    FString J = TEXT("{\n");
    // Session aggregates
    int64 TotalTime = 0, TotalTris = 0, TotalVerts = 0, TotalActors = 0, TotalErrors = 0;
    float AvgSuccess = 0.0f, AvgFPS = 0.0f, AvgFrameTime = 0.0f, AvgCPU = 0.0f, AvgGPU = 0.0f;
    int64 VRAMPeak = 0, RAMPeak = 0, HitchesTotal = 0;
    float MaxHitchAll = 0.0f;
    for (const auto& R : BenchmarkResults)
    {
        TotalTime += static_cast<int64>(R.TotalTimeMs);
        TotalTris += R.TriangleCount; TotalVerts += R.VertexCount;
        TotalActors += R.ActorCount; TotalErrors += R.ErrorCount;
        AvgSuccess += R.SuccessRate; AvgFPS += R.FPS; AvgFrameTime += R.FrameTimeMs;
        AvgCPU += R.CPUUsagePercent; AvgGPU += R.GPUUsagePercent;
        VRAMPeak = FMath::Max(VRAMPeak, R.VRAMPeakBytes); RAMPeak = FMath::Max(RAMPeak, R.RAMPeakBytes);
        HitchesTotal += R.HitchCount; MaxHitchAll = FMath::Max(MaxHitchAll, R.MaxHitchDurationMs);
    }
    const int32 N = BenchmarkResults.Num();
    J += FString::Printf(TEXT("  \"session\": { \"tests\": %d, \"total_time_ms\": %lld, \"triangles\": %lld, \"vertices\": %lld, \"actors\": %lld, \"errors\": %lld,\n"), N, TotalTime, TotalTris, TotalVerts, TotalActors, TotalErrors);
    J += FString::Printf(TEXT("    \"avg_success_rate\": %.1f, \"avg_fps\": %.1f, \"avg_frame_ms\": %.2f, \"avg_cpu\": %.1f, \"avg_gpu\": %.1f,\n"), AvgSuccess/N, AvgFPS/N, AvgFrameTime/N, AvgCPU/N, AvgGPU/N);
    J += FString::Printf(TEXT("    \"vram_peak_gb\": %.2f, \"ram_peak_mb\": %.2f, \"hitches\": %d, \"max_hitch_ms\": %.1f }\n"), VRAMPeak/1073741824.0f, RAMPeak/1048576.0f, HitchesTotal, MaxHitchAll);
    // Stage statistics
    J += TEXT("  \"stage_stats\": {\n");
    struct FStageField { const char* Name; float (FJUSYNCBenchmarkResult::*F); };
    const FStageField Fields[] = {
        { "connect_ms", &FJUSYNCBenchmarkResult::ConnectTimeMs }, { "filelist_ms", &FJUSYNCBenchmarkResult::FileListTimeMs },
        { "download_ms", &FJUSYNCBenchmarkResult::DownloadTimeMs }, { "parse_ms", &FJUSYNCBenchmarkResult::ParseTimeMs },
        { "mesh_spawn_ms", &FJUSYNCBenchmarkResult::MeshSpawnTimeMs }, { "pc_spawn_ms", &FJUSYNCBenchmarkResult::PointCloudSpawnTimeMs },
        { "material_ms", &FJUSYNCBenchmarkResult::MaterialTimeMs }, { "total_ms", &FJUSYNCBenchmarkResult::TotalTimeMs },
        { "throughput_mbps", &FJUSYNCBenchmarkResult::NetworkThroughput_MBps } };
    for (int32 s = 0; s < static_cast<int32>(UE_ARRAY_COUNT(Fields)); ++s)
    {
        TArray<float> Samples; Samples.Reserve(N);
        for (const auto& R : BenchmarkResults) Samples.Add((R.*(Fields[s].F)));
        float Mx, Mn, Md, Sd, P95;
        ComputeStageStats(Samples, Mn, Md, Mx, Sd, P95);
        J += FString::Printf(TEXT("    \"%s\": { \"min\": %.2f, \"median\": %.2f, \"max\": %.2f, \"stddev\": %.2f, \"p95\": %.2f }%s\n"),
            *FString(Fields[s].Name), Mn, Md, Mx, Sd, P95, s < static_cast<int32>(UE_ARRAY_COUNT(Fields)) - 1 ? TEXT(",") : TEXT(""));
    }
    J += TEXT("  },\n  \"results\": [\n");
    for (int32 i = 0; i < N; ++i)
    {
        const auto& R = BenchmarkResults[i];
        float TotSec = R.TotalTimeMs / 1000.0f;
        J += TEXT("    {\n");
        J += FString::Printf(TEXT("      \"name\": \"%s\", \"ts\": \"%s\",\n"), *R.TestName, *R.Timestamp.ToIso8601());
        J += FString::Printf(TEXT("      \"total_ms\": %.2f, \"connect_ms\": %.2f, \"filelist_ms\": %.2f, \"download_ms\": %.2f,\n"), R.TotalTimeMs, R.ConnectTimeMs, R.FileListTimeMs, R.DownloadTimeMs);
        J += FString::Printf(TEXT("      \"parse_ms\": %.2f, \"mesh_ms\": %.2f, \"pc_ms\": %.2f, \"material_ms\": %.2f,\n"), R.ParseTimeMs, R.MeshSpawnTimeMs, R.PointCloudSpawnTimeMs, R.MaterialTimeMs);
        J += FString::Printf(TEXT("      \"throughput_mbps\": %.2f, \"files_req\": %d, \"files_done\": %d, \"bytes\": %lld,\n"), R.NetworkThroughput_MBps, R.FilesRequested, R.FilesDownloaded, R.TotalBytesDownloaded);
        J += FString::Printf(TEXT("      \"triangles\": %d, \"vertices\": %d, \"meshes\": %d, \"pcs\": %d, \"pc_points\": %d, \"materials\": %d,\n"), R.TriangleCount, R.VertexCount, R.MeshCount, R.PointCloudCount, R.PointCloudPointsSpawned, R.MaterialsCreated);
        J += FString::Printf(TEXT("      \"ram_before_mb\": %.1f, \"ram_after_mb\": %.1f, \"ram_peak_mb\": %.1f,\n"), R.RAMBeforeBytes/1048576.0f, R.RAMAfterBytes/1048576.0f, R.RAMPeakBytes/1048576.0f);
        J += FString::Printf(TEXT("      \"vram_before_mb\": %.1f, \"vram_after_mb\": %.1f, \"vram_peak_mb\": %.1f,\n"), R.VRAMBeforeBytes/1048576.0f, R.VRAMAfterBytes/1048576.0f, R.VRAMPeakBytes/1048576.0f);
        J += FString::Printf(TEXT("      \"cpu\": %.1f, \"gpu\": %.1f, \"fps\": %.1f, \"frame_ms\": %.2f,\n"), R.CPUUsagePercent, R.GPUUsagePercent, R.FPS, R.FrameTimeMs);
        J += FString::Printf(TEXT("      \"actors\": %d, \"errors\": %d, \"success\": %.1f, \"hitches\": %d, \"max_hitch_ms\": %.2f }%s\n"),
            R.ActorCount, R.ErrorCount, R.SuccessRate, R.HitchCount, R.MaxHitchDurationMs, i < N - 1 ? TEXT(",") : TEXT(""));
    }
    J += TEXT("  ]\n}\n");
    if (FFileHelper::SaveStringToFile(J, *Path))
        UE_LOG(LogJUSYNC, Log, TEXT("Benchmark JSON saved: %s"), *Path);
}
