#include "JUSYNCBlueprintLibrary.h"

#include "JUSYNCModule.h"
#include "JUSYNCSubsystem.h"
#include <cstring>
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "RealtimeMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <tlhelp32.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_LINUX
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#endif

static int64 GetVRAMUsageBytes();
static float GetCPUUsagePercentage();
static int32 GetActiveThreadCount();

TArray<FJUSYNCFileData> UJUSYNCBlueprintLibrary::ReceivedFiles;
TArray<FString> UJUSYNCBlueprintLibrary::ReceivedMessages;
FCriticalSection UJUSYNCBlueprintLibrary::DataMutex;

TArray<FString> UJUSYNCBlueprintLibrary::LastFileList;
FCriticalSection UJUSYNCBlueprintLibrary::LastFileListMutex;

TArray<FString> UJUSYNCBlueprintLibrary::LastFileListWithSizes_Names;
TArray<int64> UJUSYNCBlueprintLibrary::LastFileListWithSizes_Sizes;
FCriticalSection UJUSYNCBlueprintLibrary::LastFileListWithSizesMutex;

TArray<FJUSYNCBenchmarkResult> UJUSYNCBlueprintLibrary::BenchmarkResults;
FString UJUSYNCBlueprintLibrary::CurrentBenchmarkTest = TEXT("");
FJUSYNCBenchmarkConfig UJUSYNCBlueprintLibrary::CurrentBenchmarkConfig;
bool UJUSYNCBlueprintLibrary::bIsBenchmarking = false;
FDateTime UJUSYNCBlueprintLibrary::BenchmarkSessionStartTime;

bool UJUSYNCBlueprintLibrary::InitializeJUSYNCMiddleware(const FString& Endpoint)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->InitializeMiddleware(Endpoint);
    if (bResult)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC middleware initialized"));
    }
    else
    {
    }

    return bResult;
}

void UJUSYNCBlueprintLibrary::ShutdownJUSYNCMiddleware()
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->ShutdownMiddleware();
        ClearReceivedData();
    }
}

bool UJUSYNCBlueprintLibrary::IsJUSYNCConnected()
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    return Subsystem ? Subsystem->IsMiddlewareConnected() : false;
}

FString UJUSYNCBlueprintLibrary::GetJUSYNCStatusInfo()
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        return TEXT("Subsystem not available");
    }

    FString Status = Subsystem->GetStatusInfo();

    FScopeLock Lock(&DataMutex);
    Status += FString::Printf(TEXT("\nReceived Files: %d\nReceived Messages: %d"),
        ReceivedFiles.Num(), ReceivedMessages.Num());

    return Status;
}

bool UJUSYNCBlueprintLibrary::StartJUSYNCReceiving()
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        return false;
    }

    bool bResult = Subsystem->StartReceiving();
    if (bResult)
    {
    }

    return bResult;
}

void UJUSYNCBlueprintLibrary::StopJUSYNCReceiving()
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->StopReceiving();
    }
}

bool UJUSYNCBlueprintLibrary::ConnectToANARIUSDBroker(const FString& BrokerEndpoint, int32 TimeoutMs)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->ConnectToBroker(BrokerEndpoint, TimeoutMs);
    if (bResult)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Connected to ANARI USD broker at %s"), *BrokerEndpoint);
        FJUSYNCBenchmarkTiming::Get().MarkConnectStart();
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to connect to broker at %s"), *BrokerEndpoint);
    }

    return bResult;
}

void UJUSYNCBlueprintLibrary::DisconnectFromANARIUSDBroker()
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->DisconnectFromBroker();
        UE_LOG(LogJUSYNC, Log, TEXT("Disconnected from ANARI USD broker"));
    }
}

bool UJUSYNCBlueprintLibrary::IsANARIUSDBrokerConnected()
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    return Subsystem ? Subsystem->IsBrokerConnected() : false;
}

bool UJUSYNCBlueprintLibrary::RequestFileListFromBroker(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->RequestFileList(TargetRank, TimeoutMs, OutFiles);
    if (bResult)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved %d files from broker"), OutFiles.Num());

        if (OutFiles.Num() > 0)
        {
            FScopeLock Lock(&LastFileListMutex);
            LastFileList = OutFiles;
            UE_LOG(LogJUSYNC, Log, TEXT("Stored %d files in LastFileList"), OutFiles.Num());
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve file list from broker"));
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::RequestFileListWithSizesFromBroker(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles, TArray<int64>& OutSizes)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->RequestFileListWithSizes(TargetRank, TimeoutMs, OutFiles, OutSizes);
    if (bResult)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved %d files with sizes from broker"), OutFiles.Num());
        if (OutFiles.Num() > 0)
        {
            FScopeLock Lock(&LastFileListMutex);
            LastFileList = OutFiles;
            UE_LOG(LogJUSYNC, Log, TEXT("Stored %d files in LastFileList"), OutFiles.Num());
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve file list with sizes from broker"));
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::RequestFileListWithSizesAndRanksFromBroker(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles, TArray<int64>& OutSizes, TArray<int32>& OutRanks)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->RequestFileListWithSizesAndRanks(TargetRank, TimeoutMs, OutFiles, OutSizes, OutRanks);
    if (bResult)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved %d files with sizes, ranks, hashes from broker"), OutFiles.Num());
        if (OutFiles.Num() > 0)
        {
            FScopeLock Lock(&LastFileListMutex);
            LastFileList = OutFiles;
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve file list with sizes and ranks from broker"));
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::GetLastFileListFromBroker(TArray<FString>& OutFileList)
{
    FScopeLock Lock(&LastFileListMutex);
    if (LastFileList.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("No file list stored yet"));
        return false;
    }
    OutFileList = LastFileList;
    UE_LOG(LogJUSYNC, Log, TEXT("Retrieved last file list (%d files)"), OutFileList.Num());
    return true;
}

bool UJUSYNCBlueprintLibrary::GetLastFileListWithSizesFromBroker(TArray<FString>& OutFileList, TArray<int64>& OutFileSizes)
{
    FScopeLock Lock(&LastFileListWithSizesMutex);
    if (LastFileListWithSizes_Names.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("No file list with sizes stored yet"));
        return false;
    }
    OutFileList = LastFileListWithSizes_Names;
    OutFileSizes = LastFileListWithSizes_Sizes;
    UE_LOG(LogJUSYNC, Log, TEXT("Retrieved last file list with sizes (%d files)"), OutFileList.Num());
    return true;
}

bool UJUSYNCBlueprintLibrary::RequestFileFromBroker(const FString& Filename, int32 TargetRank, int32 TimeoutMs, TArray<uint8>& OutData)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->RequestFile(Filename, TargetRank, TimeoutMs, OutData);
    if (bResult)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved file '%s' (%d bytes) from broker"), *Filename, OutData.Num());
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve file '%s' from broker"), *Filename);
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::RequestFrameFromBroker(int32 FrameNumber, int32 TargetRank, int32 TimeoutMs, TArray<FJUSYNCFileData>& OutFiles)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->RequestFrame(FrameNumber, TargetRank, TimeoutMs, OutFiles);
    if (bResult)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved frame %d with %d files from broker"), FrameNumber, OutFiles.Num());
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve frame %d from broker"), FrameNumber);
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::RequestWorkerStatusFromBroker(int32 TargetRank, int32 TimeoutMs, TArray<FJUSYNCWorkerStatus>& OutWorkerStatus)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->RequestWorkerStatus(TargetRank, TimeoutMs, OutWorkerStatus);
    if (bResult)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved worker status for rank %d: %d workers"), TargetRank, OutWorkerStatus.Num());
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve worker status for rank %d"), TargetRank);
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::RequestWorkerCountFromBroker(int32 TimeoutMs, int32& OutWorkerCount)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->RequestWorkerCount(TimeoutMs, OutWorkerCount);
    if (bResult)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved worker count: %d workers"), OutWorkerCount);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve worker count"));
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::RequestTotalWorkerCountFromBroker(int32 TimeoutMs, int32& OutTotalCount)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->RequestTotalWorkerCount(TimeoutMs, OutTotalCount);
    if (bResult)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved total worker count (including rank 0): %d"), OutTotalCount);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve total worker count"));
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::RequestWorkerCountExcludingRank0(int32 TimeoutMs, int32& OutWorkerCount)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->RequestWorkerCountExcludingRank0(TimeoutMs, OutWorkerCount);
    if (bResult)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved worker count (excluding rank 0): %d workers"), OutWorkerCount);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve worker count"));
    }

    return bResult;
}

void UJUSYNCBlueprintLibrary::RequestTotalWorkerCountAsync(int32 TimeoutMs, const FOnTotalWorkerCountReceived& OnComplete, const FOnBrokerError& OnError)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        OnError.ExecuteIfBound(TEXT("Subsystem not available"));
        return;
    }

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, TimeoutMs, OnComplete, OnError]()
        {
            if (!WeakSubsystem.IsValid()) return;

            int32 TotalCount = 0;
            bool bSuccess = WeakSubsystem->RequestTotalWorkerCount(TimeoutMs, TotalCount);

            if (!WeakSubsystem.IsValid()) return;

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, TotalCount, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(TotalCount);
                    else OnError.ExecuteIfBound(TEXT("Failed to retrieve total worker count"));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

void UJUSYNCBlueprintLibrary::RequestWorkerCountAsync(int32 TimeoutMs, const FOnWorkerCountReceived& OnComplete, const FOnBrokerError& OnError)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        OnError.ExecuteIfBound(TEXT("Subsystem not available"));
        return;
    }

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, TimeoutMs, OnComplete, OnError]()
        {
            if (!WeakSubsystem.IsValid()) return;

            int32 WorkerCount = 0;
            bool bSuccess = WeakSubsystem->RequestWorkerCount(TimeoutMs, WorkerCount);

            if (!WeakSubsystem.IsValid()) return;

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, WorkerCount, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(WorkerCount);
                    else OnError.ExecuteIfBound(TEXT("Failed to retrieve worker count"));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

void UJUSYNCBlueprintLibrary::RequestWorkerStatusAsync(int32 TargetRank, int32 TimeoutMs, const FOnWorkerStatusReceived& OnComplete, const FOnBrokerError& OnError)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        OnError.ExecuteIfBound(TEXT("Subsystem not available"));
        return;
    }

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, TargetRank, TimeoutMs, OnComplete, OnError]()
        {
            if (!WeakSubsystem.IsValid()) return;

            TArray<FJUSYNCWorkerStatus> WorkerStatus;
            bool bSuccess = WeakSubsystem->RequestWorkerStatus(TargetRank, TimeoutMs, WorkerStatus);

            if (!WeakSubsystem.IsValid()) return;

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, WorkerStatus, TargetRank, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(WorkerStatus);
                    else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve worker status for rank %d"), TargetRank));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

void UJUSYNCBlueprintLibrary::RequestFileListAsync(int32 TargetRank, int32 TimeoutMs, const FOnFileListReceived& OnComplete, const FOnBrokerError& OnError)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        OnError.ExecuteIfBound(TEXT("Subsystem not available"));
        return;
    }

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, TargetRank, TimeoutMs, OnComplete, OnError]()
        {
            if (!WeakSubsystem.IsValid()) return;

            TArray<FString> FileList;
            bool bSuccess = WeakSubsystem->RequestFileList(TargetRank, TimeoutMs, FileList);

            if (bSuccess && FileList.Num() > 0)
            {
                FScopeLock Lock(&UJUSYNCBlueprintLibrary::LastFileListMutex);
                UJUSYNCBlueprintLibrary::LastFileList = FileList;
            }

            if (!WeakSubsystem.IsValid()) return;

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, FileList, TargetRank, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(FileList);
                    else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve file list from rank %d"), TargetRank));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

void UJUSYNCBlueprintLibrary::RequestFileListWithSizesAsync(int32 TargetRank, int32 TimeoutMs, const FOnFileListWithSizesReceived& OnComplete, const FOnBrokerError& OnError)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        OnError.ExecuteIfBound(TEXT("Subsystem not available"));
        return;
    }

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, TargetRank, TimeoutMs, OnComplete, OnError]()
        {
            if (!WeakSubsystem.IsValid()) return;

            TArray<FString> FileList;
            TArray<int64> FileSizes;
            bool bSuccess = WeakSubsystem->RequestFileListWithSizes(TargetRank, TimeoutMs, FileList, FileSizes);

            if (bSuccess && FileList.Num() > 0)
            {
                FScopeLock Lock(&UJUSYNCBlueprintLibrary::LastFileListWithSizesMutex);
                UJUSYNCBlueprintLibrary::LastFileListWithSizes_Names = FileList;
                UJUSYNCBlueprintLibrary::LastFileListWithSizes_Sizes = FileSizes;
            }

            if (!WeakSubsystem.IsValid()) return;

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, FileList, FileSizes, TargetRank, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(FileList, FileSizes);
                    else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve file list with sizes from rank %d"), TargetRank));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

void UJUSYNCBlueprintLibrary::RequestFileListWithSizesAndRanksAsync(int32 TargetRank, int32 TimeoutMs, const FOnFileListWithSizesAndRanksReceived& OnComplete, const FOnBrokerError& OnError)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        OnError.ExecuteIfBound(TEXT("Subsystem not available"));
        return;
    }

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, TargetRank, TimeoutMs, OnComplete, OnError]()
        {
            if (!WeakSubsystem.IsValid()) return;

            TArray<FString> FileList;
            TArray<int64> FileSizes;
            TArray<int32> FileRanks;
            bool bSuccess = WeakSubsystem->RequestFileListWithSizesAndRanks(TargetRank, TimeoutMs, FileList, FileSizes, FileRanks);

            if (bSuccess && FileList.Num() > 0)
            {
                FScopeLock Lock(&UJUSYNCBlueprintLibrary::LastFileListMutex);
                UJUSYNCBlueprintLibrary::LastFileList = FileList;
            }

            if (!WeakSubsystem.IsValid()) return;

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, FileList, FileSizes, FileRanks, TargetRank, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(FileList, FileSizes, FileRanks);
                    else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve file list with sizes and ranks from rank %d"), TargetRank));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

void UJUSYNCBlueprintLibrary::RequestFileAsync(const FString& Filename, int32 TargetRank, int32 TimeoutMs, const FOnFileReceived& OnComplete, const FOnBrokerError& OnError)
{
    const int32 MAX_REASONABLE_TIMEOUT = 300000;  // 5 minutes max
    if (TimeoutMs > MAX_REASONABLE_TIMEOUT)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("Timeout %dms is too large, using dynamic timeout instead"), TimeoutMs);

        int32 ExtractedRank = ExtractRankFromFilename(Filename);
        if (ExtractedRank >= 0 && ExtractedRank != TargetRank)
        {
            UE_LOG(LogJUSYNC, Log, TEXT("Adjusting target rank from %d to %d based on filename"), TargetRank, ExtractedRank);
            TargetRank = ExtractedRank;
        }

        int32 DynamicTimeout = CalculateDynamicTimeout(Filename, TargetRank, false, 0);
        UE_LOG(LogJUSYNC, Log, TEXT("Using dynamic timeout: %dms for file '%s'"), DynamicTimeout, *Filename);

        RequestFileAsyncDynamic(Filename, TargetRank, OnComplete, OnError, 1, true);
        return;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        OnError.ExecuteIfBound(TEXT("Subsystem not available"));
        return;
    }

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, Filename, TargetRank, TimeoutMs, OnComplete, OnError]()
        {
            if (!WeakSubsystem.IsValid()) return;

            TArray<uint8> FileData;
            bool bSuccess = WeakSubsystem->RequestFile(Filename, TargetRank, TimeoutMs, FileData);

            if (!WeakSubsystem.IsValid()) return;

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, Filename, FileData, TargetRank, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(Filename, FileData);
                    else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve file '%s' from rank %d"), *Filename, TargetRank));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

int32 UJUSYNCBlueprintLibrary::ExtractRankFromFilename(const FString& Filename)
{

    int32 Rank = -1;

    const FString Base = FPaths::GetBaseFilename(Filename);

    for (int32 SearchFrom = 0; SearchFrom < Base.Len();)
    {
        const int32 RankStart = Base.Find(TEXT("_r"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
        if (RankStart == INDEX_NONE)
        {
            break;
        }

        const int32 DigitStart = RankStart + 2;
        int32 DigitEnd = DigitStart;
        while (DigitEnd < Base.Len() && FChar::IsDigit(Base[DigitEnd]))
        {
            ++DigitEnd;
        }

        const bool bWholeToken = DigitEnd > DigitStart && (DigitEnd == Base.Len() || Base[DigitEnd] == TEXT('_'));
        if (bWholeToken)
        {
            Rank = FCString::Atoi(*Base.Mid(DigitStart, DigitEnd - DigitStart));
            UE_LOG(LogJUSYNC, Log, TEXT("Extracted rank %d from '_r<digits>' token in: %s"), Rank, *Filename);
            return Rank;
        }

        SearchFrom = RankStart + 2;
    }

    if (Base.Contains(TEXT("__")))
    {
        UE_LOG(LogJUSYNC, Verbose, TEXT("No rank token in clip key: %s"), *Filename);
        return -1;
    }

    FString BaseName = Filename;
    int32 DotIndex = BaseName.Find(TEXT("."));
    if (DotIndex != INDEX_NONE)
    {
        BaseName = BaseName.Left(DotIndex);
    }

    int32 LastUnderscore = BaseName.Find(TEXT("_"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
    if (LastUnderscore != INDEX_NONE && LastUnderscore < BaseName.Len() - 1)
    {
        FString RankStr = BaseName.Mid(LastUnderscore + 1);
        if (RankStr.IsNumeric())
        {
            Rank = FCString::Atoi(*RankStr);
            if (Rank >= 0)
            {
                UE_LOG(LogJUSYNC, Log, TEXT("Extracted rank %d using pattern _X at end from: %s"), Rank, *Filename);
                return Rank;
            }
        }
    }

    TArray<FString> Parts;
    Filename.ParseIntoArray(Parts, TEXT("_"), true);

    for (int32 i = Parts.Num() - 1; i >= 0; --i)
    {
        if (Parts[i].IsNumeric())
        {
            Rank = FCString::Atoi(*Parts[i]);
            if (Rank >= 0)
            {
                UE_LOG(LogJUSYNC, Log, TEXT("Extracted rank %d using numeric part from: %s"), Rank, *Filename);
                return Rank;
            }
        }
    }

    UE_LOG(LogJUSYNC, Warning, TEXT("Could not extract rank from filename: %s"), *Filename);
    return -1;
}

bool UJUSYNCBlueprintLibrary::LoadUSDFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCMeshData>& OutMeshData, FString& OutPreview)
{
    if (!ValidateBufferSize(Buffer, TEXT("LoadUSDFromBuffer")))
    {
        return false;
    }

#if JUSYNC_ENABLE_USD_PREVIEW
    OutPreview = GetUSDAPreview(Buffer, 15);
#else
    OutPreview = TEXT("USD preview disabled for performance");
#endif

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available for USD loading"));
        return false;
    }

    bool bResult = Subsystem->LoadUSDFromBuffer(Buffer, Filename, OutMeshData);

    if (bResult)
    {
        FString Message = FString::Printf(TEXT("Loaded USD: %s (%d meshes)"), *Filename, OutMeshData.Num());
        UE_LOG(LogJUSYNC, Log, TEXT("Loaded %d meshes from USD buffer"), OutMeshData.Num());
    }
    else
    {
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::LoadUSDFromDisk(const FString& FilePath, TArray<FJUSYNCMeshData>& OutMeshData, FString& OutPreview)
{
    if (!ValidateFilePath(FilePath, TEXT("LoadUSDFromDisk")))
    {
        return false;
    }

    TArray<uint8> Buffer;
    if (!LoadFileToBuffer(FilePath, Buffer))
    {
        return false;
    }

    FString Filename = FPaths::GetCleanFilename(FilePath);

    return LoadUSDFromBuffer(Buffer, Filename, OutMeshData, OutPreview);
}

bool UJUSYNCBlueprintLibrary::LoadUSDFullFromBuffer(const TArray<uint8>& Buffer, const FString& Filename,
    TArray<FJUSYNCMeshData>& OutMeshData, TArray<FJUSYNCPointCloudData>& OutPointCloudData, FString& OutPreview)
{
    if (!ValidateBufferSize(Buffer, TEXT("LoadUSDFullFromBuffer")))
    {
        return false;
    }

#if JUSYNC_ENABLE_USD_PREVIEW
    OutPreview = GetUSDAPreview(Buffer, 15);
#else
    OutPreview = TEXT("USD preview disabled for performance");
#endif

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available for full USD loading"));
        return false;
    }

    bool bResult = Subsystem->LoadUSDFullFromBuffer(Buffer, Filename, OutMeshData, OutPointCloudData);

    if (bResult)
    {
        int32 TotalMeshes = 0;
        for (const FJUSYNCMeshData& m : OutMeshData)
        {
            if (m.IsValid()) TotalMeshes++;
        }
        int32 TotalPCs = 0;
        for (const FJUSYNCPointCloudData& pc : OutPointCloudData)
        {
            if (pc.IsValid()) TotalPCs++;
        }
        UE_LOG(LogJUSYNC, Log, TEXT("Loaded %d meshes + %d point clouds from USD '%s' (single-pass)"),
               TotalMeshes, TotalPCs, *Filename);
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::LoadUSDFullFromBufferNoCopy(const TArray<uint8>& Buffer, const FString& Filename,
    TArray<FJUSYNCMeshData>& OutMeshData, TArray<FJUSYNCPointCloudData>& OutPointCloudData, FString& OutPreview)
{
    if (!ValidateBufferSize(Buffer, TEXT("LoadUSDFullFromBufferNoCopy")))
    {
        return false;
    }

#if JUSYNC_ENABLE_USD_PREVIEW
    OutPreview = GetUSDAPreview(Buffer, 15);
#else
    OutPreview = TEXT("USD preview disabled for performance");
#endif

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available for full USD loading (no-copy)"));
        return false;
    }

    bool bResult = Subsystem->LoadUSDFullFromBufferNoCopy(Buffer, Filename, OutMeshData, OutPointCloudData);

    if (bResult)
    {
        int32 TotalMeshes = 0;
        for (const FJUSYNCMeshData& m : OutMeshData)
        {
            if (m.IsValid()) TotalMeshes++;
        }
        int32 TotalPCs = 0;
        for (const FJUSYNCPointCloudData& pc : OutPointCloudData)
        {
            if (pc.IsValid()) TotalPCs++;
        }
        UE_LOG(LogJUSYNC, Log, TEXT("Loaded %d meshes + %d point clouds from USD '%s' (zero-copy)"),
               TotalMeshes, TotalPCs, *Filename);
    }

    return bResult;
}

FString UJUSYNCBlueprintLibrary::GetUSDAPreview(const TArray<uint8>& Buffer, int32 MaxLines)
{
    return ExtractUSDAPreview(Buffer, MaxLines);
}

bool UJUSYNCBlueprintLibrary::ValidateUSDFormat(const TArray<uint8>& Buffer, const FString& Filename)
{
    if (!ValidateBufferSize(Buffer, TEXT("ValidateUSDFormat")))
    {
        return false;
    }

    FString Extension = FPaths::GetExtension(Filename).ToLower();
    if (Extension != TEXT("usd") && Extension != TEXT("usda") &&
        Extension != TEXT("usdc") && Extension != TEXT("usdz"))
    {
        return false;
    }

    const uint8* BufferData = Buffer.GetData();
    if (!BufferData)
    {
        return false;
    }

    const int32 SCAN_SIZE = FMath::Min(Buffer.Num(), 8 * 1024);

    const char* Marker1 = "#usda";
    const char* Marker2 = "PXR-USDC";
    const char* Marker3 = "def ";
    const char* Marker4 = "over ";

    for (int32 i = 0; i < SCAN_SIZE; ++i)
    {
        if (BufferData[i] == '#' && i + 4 <= SCAN_SIZE)
        {
            if (std::memcmp(BufferData + i, Marker1, 5) == 0) return true;
        }
        if (BufferData[i] == 'P' && i + 8 <= SCAN_SIZE)
        {
            if (std::memcmp(BufferData + i, Marker2, 8) == 0) return true;
        }
        if (BufferData[i] == 'd' && i + 4 <= SCAN_SIZE)
        {
            if (std::memcmp(BufferData + i, Marker3, 4) == 0) return true;
        }
        if (BufferData[i] == 'o' && i + 5 <= SCAN_SIZE)
        {
            if (std::memcmp(BufferData + i, Marker4, 5) == 0) return true;
        }
    }

    return false;
}

bool UJUSYNCBlueprintLibrary::LoadUSDPointCloudFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCPointCloudData>& OutPointCloudData)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available for point cloud loading"));
        return false;
    }

    return Subsystem->LoadPointCloudFromBuffer(Buffer, Filename, OutPointCloudData);
}

AActor* UJUSYNCBlueprintLibrary::SpawnPointCloudAtLocation(
    const FJUSYNCPointCloudData& PointCloudData,
    const FVector& SpawnLocation,
    const FRotator& SpawnRotation,
    const FVector& SpawnScale)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available for point cloud spawning"));
        return nullptr;
    }
    return Subsystem->SpawnLidarPointCloudAtLocation(PointCloudData, SpawnLocation, SpawnRotation, SpawnScale);
}

TArray<AActor*> UJUSYNCBlueprintLibrary::BatchSpawnPointClouds(
    const TArray<FJUSYNCPointCloudData>& PointCloudDataArray,
    const TArray<FVector>& SpawnLocations)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available for point cloud batch spawning"));
        return TArray<AActor*>();
    }
    return Subsystem->BatchSpawnPointCloudsAtLocations(PointCloudDataArray, SpawnLocations);
}

void UJUSYNCBlueprintLibrary::SpawnPointCloudAtLocation_Async(
    const FJUSYNCPointCloudData& PointCloudData,
    const FVector& SpawnLocation,
    const FRotator& SpawnRotation,
    const FVector& SpawnScale,
    const FOnPointCloudSpawnedDyn& OnSpawned)
{
    bool bBound = OnSpawned.IsBound();
    FOnPointCloudSpawnedDyn DelegateCopy = OnSpawned;
    AsyncTask(ENamedThreads::GameThread, [PointCloudData = PointCloudData, SpawnLocation, SpawnRotation, SpawnScale, DelegateCopy]()
    {
        AActor* Actor = SpawnPointCloudAtLocation(PointCloudData, SpawnLocation, SpawnRotation, SpawnScale);
        DelegateCopy.ExecuteIfBound(Actor, Actor != nullptr);
    });
}

void UJUSYNCBlueprintLibrary::BatchSpawnPointClouds_Async(
    const TArray<FJUSYNCPointCloudData>& PointCloudDataArray,
    const TArray<FVector>& SpawnLocations,
    const FOnPointCloudBatchSpawnedDyn& OnBatchSpawned)
{
    FOnPointCloudBatchSpawnedDyn DelegateCopy = OnBatchSpawned;
    AsyncTask(ENamedThreads::GameThread, [PointCloudDataArray = PointCloudDataArray, SpawnLocations = SpawnLocations, DelegateCopy]()
    {
        TArray<AActor*> Spawned = BatchSpawnPointClouds(PointCloudDataArray, SpawnLocations);
        DelegateCopy.ExecuteIfBound(Spawned);
    });
}

FJUSYNCTextureData UJUSYNCBlueprintLibrary::CreateTextureFromBuffer(const TArray<uint8>& Buffer)
{
    FJUSYNCTextureData EmptyTexture;

    if (!ValidateBufferSize(Buffer, TEXT("CreateTextureFromBuffer")))
    {
        return EmptyTexture;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available for texture creation"));
        return EmptyTexture;
    }

    FJUSYNCTextureData Result = Subsystem->CreateTextureFromBuffer(Buffer);

    if (Result.IsValid())
    {
        FString Message = FString::Printf(TEXT("Created Texture: %dx%d (%d channels)"),
            Result.Width, Result.Height, Result.Channels);
    }
    else
    {
    }

    return Result;
}

UTexture2D* UJUSYNCBlueprintLibrary::CreateUETextureFromJUSYNC(const FJUSYNCTextureData& TextureData)
{
    if (!TextureData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid texture data provided"));
        return nullptr;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return nullptr;
    }

    UTexture2D* Result = Subsystem->CreateUETextureFromJUSYNC(TextureData);

    if (Result)
    {
    }

    return Result;
}

bool UJUSYNCBlueprintLibrary::WriteGradientLineAsPNG(const TArray<uint8>& Buffer, const FString& OutputPath)
{
    if (!ValidateBufferSize(Buffer, TEXT("WriteGradientLineAsPNG")))
    {
        return false;
    }

    if (!ValidateFilePath(OutputPath, TEXT("WriteGradientLineAsPNG")))
    {
        return false;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->WriteGradientLineAsPNG(Buffer, OutputPath);

    if (bResult)
    {
        FString Message = FString::Printf(TEXT("Gradient PNG saved: %s"), *OutputPath);
    }
    else
    {
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::GetGradientLineAsPNGBuffer(const TArray<uint8>& Buffer, TArray<uint8>& OutPNGBuffer)
{
    if (!ValidateBufferSize(Buffer, TEXT("GetGradientLineAsPNGBuffer")))
    {
        return false;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->GetGradientLineAsPNGBuffer(Buffer, OutPNGBuffer);

    if (bResult)
    {
        FString Message = FString::Printf(TEXT("Gradient PNG buffer created: %d bytes"), OutPNGBuffer.Num());
    }
    else
    {
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::GetPNGDimensions(const TArray<uint8>& Buffer, int32& OutWidth, int32& OutHeight, int32& OutChannels)
{
    OutWidth = 0;
    OutHeight = 0;
    OutChannels = 0;

    if (!ValidateBufferSize(Buffer, TEXT("GetPNGDimensions")))
    {
        return false;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->GetPNGDimensions(Buffer, OutWidth, OutHeight, OutChannels);

    if (bResult)
    {
        FString Message = FString::Printf(TEXT("PNG dimensions: %dx%d (%d channels)"), OutWidth, OutHeight, OutChannels);
    }
    else
    {
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::GetImageRowAsPNGBuffer(const TArray<uint8>& Buffer, int32 RowIndex, TArray<uint8>& OutPNGBuffer)
{
    OutPNGBuffer.Empty();

    if (!ValidateBufferSize(Buffer, TEXT("GetImageRowAsPNGBuffer")))
    {
        return false;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->GetImageRowAsPNGBuffer(Buffer, RowIndex, OutPNGBuffer);

    if (bResult)
    {
        FString Message = FString::Printf(TEXT("Extracted row %d as PNG buffer: %d bytes"), RowIndex, OutPNGBuffer.Num());
    }
    else
    {
        FString Message = FString::Printf(TEXT("Failed to extract row %d as PNG buffer"), RowIndex);
    }

    return bResult;
}

void UJUSYNCBlueprintLibrary::ClearBroadcastDuplicates()
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->ClearProcessedFiles();
        UE_LOG(LogJUSYNC, Log, TEXT("Cleared broadcast duplicate tracking"));
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
    }
}

bool UJUSYNCBlueprintLibrary::CreateRealtimeMeshFromJUSYNC(const FJUSYNCMeshData& MeshData, URealtimeMeshComponent* RealtimeMeshComponent)
{
    if (!MeshData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid mesh data provided"));
        return false;
    }

    if (!RealtimeMeshComponent)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("RealtimeMeshComponent is null"));
        return false;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->CreateRealtimeMeshFromJUSYNC(MeshData, RealtimeMeshComponent);

    if (bResult)
    {
        FString Message = FString::Printf(TEXT("RealtimeMesh Created: %s (%d verts, %d tris)"),
            *MeshData.ElementName, MeshData.GetVertexCount(), MeshData.GetTriangleCount());
    }

    return bResult;
}

void UJUSYNCBlueprintLibrary::CreateRealtimeMeshFromJUSYNC_Async(const FJUSYNCMeshData& MeshData, URealtimeMeshComponent* RealtimeMeshComponent)
{
    if (!MeshData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid mesh data provided for async creation"));
        return;
    }

    if (!RealtimeMeshComponent)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("RealtimeMeshComponent is null for async creation"));
        return;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available for async creation"));
        return;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Starting async mesh creation for: %s"), *MeshData.ElementName);
    Subsystem->CreateRealtimeMeshFromJUSYNC_Async(MeshData, RealtimeMeshComponent);
}

bool UJUSYNCBlueprintLibrary::BatchCreateRealtimeMeshesFromJUSYNC(const TArray<FJUSYNCMeshData>& MeshDataArray, const TArray<URealtimeMeshComponent*>& MeshComponents)
{
    if (MeshDataArray.Num() != MeshComponents.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Mesh data array and component array size mismatch"));
        return false;
    }

    bool bAllSuccessful = true;
    int32 SuccessCount = 0;

    for (int32 i = 0; i < MeshDataArray.Num(); ++i)
    {
        if (CreateRealtimeMeshFromJUSYNC(MeshDataArray[i], MeshComponents[i]))
        {
            SuccessCount++;
        }
        else
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Failed to create RealtimeMesh %d: %s"), i, *MeshDataArray[i].ElementName);
            bAllSuccessful = false;
        }
    }

    FString Message = FString::Printf(TEXT("Batch RealtimeMesh Creation: %d/%d successful"), SuccessCount, MeshDataArray.Num());
    FLinearColor Color = bAllSuccessful ? FLinearColor(0.0f, 1.0f, 1.0f, 1.0f) : FLinearColor::Yellow;

    return bAllSuccessful;
}

void UJUSYNCBlueprintLibrary::BatchCreateRealtimeMeshesFromJUSYNC_Async(const TArray<FJUSYNCMeshData>& MeshDataArray,
    const TArray<URealtimeMeshComponent*>& MeshComponents,
    int32 MaxMeshesPerFrame)
{
    if (MeshDataArray.Num() != MeshComponents.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Mesh data array and component array size mismatch"));
        return;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available for async batch creation"));
        return;
    }

    Subsystem->BatchCreateRealtimeMeshesFromJUSYNC(MeshDataArray, MeshComponents);

    UE_LOG(LogJUSYNC, Log, TEXT("Batch async mesh creation started for %d meshes"), MeshDataArray.Num());
}

FJUSYNCRealtimeMeshData UJUSYNCBlueprintLibrary::ConvertToRealtimeMeshFormat(const FJUSYNCMeshData& StandardMesh)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return FJUSYNCRealtimeMeshData();
    }

    return Subsystem->ConvertToRealtimeMeshFormat(StandardMesh);
}

bool UJUSYNCBlueprintLibrary::CheckForReceivedFiles(TArray<FJUSYNCFileData>& OutReceivedFiles)
{
    FScopeLock Lock(&DataMutex);

    if (ReceivedFiles.Num() > 0)
    {
        OutReceivedFiles = MoveTemp(ReceivedFiles);

        for (const FJUSYNCFileData& FileData : OutReceivedFiles)
        {
            FString Message = FString::Printf(TEXT("Received File: %s (%d bytes, %s)"),
                *FileData.Filename, FileData.Data.Num(), *FileData.FileType);
            UE_LOG(LogJUSYNC, Log, TEXT("%s"), *Message);
        }

        return true;
    }

    return false;
}

bool UJUSYNCBlueprintLibrary::CheckForReceivedMessages(TArray<FString>& OutReceivedMessages)
{
    FScopeLock Lock(&DataMutex);

    if (ReceivedMessages.Num() > 0)
    {
        OutReceivedMessages = MoveTemp(ReceivedMessages);

        for (const FString& Message : OutReceivedMessages)
        {
            UE_LOG(LogJUSYNC, Log, TEXT("Received Message: %s"), *Message);
        }

        return true;
    }

    return false;
}

void UJUSYNCBlueprintLibrary::ClearReceivedData()
{
    FScopeLock Lock(&DataMutex);
    ReceivedFiles.Empty();
    ReceivedMessages.Empty();
    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC received data cleared"));
}

bool UJUSYNCBlueprintLibrary::ValidateJUSYNCMeshData(const FJUSYNCMeshData& MeshData, FString& ValidationMessage)
{
    if (MeshData.ElementName.IsEmpty())
    {
        ValidationMessage = TEXT("Element name is empty");
        return false;
    }

    if (MeshData.Vertices.Num() == 0)
    {
        ValidationMessage = TEXT("No vertices found");
        return false;
    }

    if (MeshData.Triangles.Num() == 0)
    {
        ValidationMessage = TEXT("No triangles found");
        return false;
    }

    if (MeshData.Triangles.Num() % 3 != 0)
    {
        ValidationMessage = FString::Printf(TEXT("Triangle count (%d) is not divisible by 3"), MeshData.Triangles.Num());
        return false;
    }

    for (int32 Index : MeshData.Triangles)
    {
        if (Index >= MeshData.Vertices.Num())
        {
            ValidationMessage = FString::Printf(TEXT("Triangle index %d exceeds vertex count %d"), Index, MeshData.Vertices.Num());
            return false;
        }
    }

    ValidationMessage = TEXT("Mesh data is valid");
    return true;
}

bool UJUSYNCBlueprintLibrary::ValidateJUSYNCTextureData(const FJUSYNCTextureData& TextureData, FString& ValidationMessage)
{
    if (!TextureData.IsValid())
    {
        ValidationMessage = FString::Printf(TEXT("Invalid texture: %dx%d, %d channels, %d bytes"),
            TextureData.Width, TextureData.Height, TextureData.Channels, TextureData.Data.Num());
        return false;
    }

    ValidationMessage = TEXT("Texture data is valid");
    return true;
}

int32 UJUSYNCBlueprintLibrary::GetFileSize(const TArray<uint8>& FileBuffer)
{
    return FileBuffer.Num();
}

bool UJUSYNCBlueprintLibrary::FilterFileBySize(const TArray<uint8>& FileBuffer, int32 MinimumSizeBytes)
{
    return FileBuffer.Num() >= MinimumSizeBytes;
}

void UJUSYNCBlueprintLibrary::FilterFileListBySize(const TArray<FString>& FileList, const TArray<int64>& FileSizes, int32 MinimumSizeBytes, TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes)
{
    OutFilteredFiles.Empty();
    OutFilteredSizes.Empty();
    if (FileList.Num() != FileSizes.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("FilterFileListBySize: FileList and FileSizes arrays have different lengths (%d vs %d)"), FileList.Num(), FileSizes.Num());
        return;
    }
    for (int32 i = 0; i < FileList.Num(); ++i)
    {
        if (FileSizes[i] >= MinimumSizeBytes)
        {
            OutFilteredFiles.Add(FileList[i]);
            OutFilteredSizes.Add(FileSizes[i]);
        }
    }
    UE_LOG(LogJUSYNC, Log, TEXT("FilterFileListBySize: filtered %d files down to %d (threshold %d bytes)"), FileList.Num(), OutFilteredFiles.Num(), MinimumSizeBytes);
}

void UJUSYNCBlueprintLibrary::FilterFileListByExtensionEnum(const TArray<FString>& FileList, EJUSYNCExtension ExtensionFilter, TArray<FString>& OutFilteredFiles)
{
    OutFilteredFiles.Empty();
    TArray<FString> Extensions;
    switch (ExtensionFilter)
    {
    case EJUSYNCExtension::USD:
        Extensions.Add(TEXT(".usda"));
        break;
    case EJUSYNCExtension::PNG:
        Extensions.Add(TEXT(".png"));
        break;
    case EJUSYNCExtension::JSON:
        Extensions.Add(TEXT(".json"));
        break;
    case EJUSYNCExtension::TXT:
        Extensions.Add(TEXT(".txt"));
        break;
    case EJUSYNCExtension::BIN:
        Extensions.Add(TEXT(".bin"));
        break;
    case EJUSYNCExtension::ALL:
        OutFilteredFiles = FileList;
        UE_LOG(LogJUSYNC, Log, TEXT("FilterFileListByExtensionEnum: ALL selected, returning all %d files"), FileList.Num());
        return;
    default:
        UE_LOG(LogJUSYNC, Warning, TEXT("FilterFileListByExtensionEnum: unknown extension enum value %d"), (int32)ExtensionFilter);
        return;
    }

    for (const FString& Filename : FileList)
    {
        for (const FString& Ext : Extensions)
        {
            if (Filename.EndsWith(Ext, ESearchCase::IgnoreCase))
            {
                OutFilteredFiles.Add(Filename);
                break;
            }
        }
    }
    UE_LOG(LogJUSYNC, Log, TEXT("FilterFileListByExtensionEnum: filtered %d files down to %d (extension filter %s)"),
        FileList.Num(), OutFilteredFiles.Num(), *UEnum::GetValueAsString(ExtensionFilter));
}

void UJUSYNCBlueprintLibrary::FilterFileListByExtensions(const TArray<FString>& FileList, const TArray<FString>& AllowedExtensions, TArray<FString>& OutFilteredFiles)
{
    OutFilteredFiles.Empty();
    if (AllowedExtensions.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("FilterFileListByExtensions: AllowedExtensions array is empty, returning empty result"));
        return;
    }
    for (const FString& Filename : FileList)
    {
        for (const FString& Ext : AllowedExtensions)
        {
            FString NormalizedExt = Ext;
            if (!NormalizedExt.StartsWith(TEXT(".")))
                NormalizedExt = TEXT(".") + NormalizedExt;
            if (Filename.EndsWith(NormalizedExt, ESearchCase::IgnoreCase))
            {
                OutFilteredFiles.Add(Filename);
                break;
            }
        }
    }
    UE_LOG(LogJUSYNC, Log, TEXT("FilterFileListByExtensions: filtered %d files down to %d (allowed extensions: %s)"),
        FileList.Num(), OutFilteredFiles.Num(), *FString::Join(AllowedExtensions, TEXT(", ")));
}

void UJUSYNCBlueprintLibrary::FilterFileListByExtensionEnumWithSizes(const TArray<FString>& FileList, const TArray<int64>& FileSizes, EJUSYNCExtension ExtensionFilter, TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes)
{
    OutFilteredFiles.Empty();
    OutFilteredSizes.Empty();
    if (FileList.Num() != FileSizes.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("FilterFileListByExtensionEnumWithSizes: FileList and FileSizes arrays have different lengths (%d vs %d)"), FileList.Num(), FileSizes.Num());
        return;
    }
    TArray<FString> Extensions;
    switch (ExtensionFilter)
    {
    case EJUSYNCExtension::USD:
        Extensions.Add(TEXT(".usda"));
        break;
    case EJUSYNCExtension::PNG:
        Extensions.Add(TEXT(".png"));
        break;
    case EJUSYNCExtension::JSON:
        Extensions.Add(TEXT(".json"));
        break;
    case EJUSYNCExtension::TXT:
        Extensions.Add(TEXT(".txt"));
        break;
    case EJUSYNCExtension::BIN:
        Extensions.Add(TEXT(".bin"));
        break;
    case EJUSYNCExtension::ALL:
        OutFilteredFiles = FileList;
        OutFilteredSizes = FileSizes;
        UE_LOG(LogJUSYNC, Log, TEXT("FilterFileListByExtensionEnumWithSizes: ALL selected, returning all %d files"), FileList.Num());
        return;
    default:
        UE_LOG(LogJUSYNC, Warning, TEXT("FilterFileListByExtensionEnumWithSizes: unknown extension enum value %d"), (int32)ExtensionFilter);
        return;
    }

    for (int32 i = 0; i < FileList.Num(); ++i)
    {
        const FString& Filename = FileList[i];
        for (const FString& Ext : Extensions)
        {
            if (Filename.EndsWith(Ext, ESearchCase::IgnoreCase))
            {
                OutFilteredFiles.Add(Filename);
                OutFilteredSizes.Add(FileSizes[i]);
                break;
            }
        }
    }
    UE_LOG(LogJUSYNC, Log, TEXT("FilterFileListByExtensionEnumWithSizes: filtered %d files down to %d (extension filter %s)"),
        FileList.Num(), OutFilteredFiles.Num(), *UEnum::GetValueAsString(ExtensionFilter));
}

void UJUSYNCBlueprintLibrary::FilterFileListByExtensionsWithSizes(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<FString>& AllowedExtensions, TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes)
{
    OutFilteredFiles.Empty();
    OutFilteredSizes.Empty();
    if (FileList.Num() != FileSizes.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("FilterFileListByExtensionsWithSizes: FileList and FileSizes arrays have different lengths (%d vs %d)"), FileList.Num(), FileSizes.Num());
        return;
    }
    if (AllowedExtensions.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("FilterFileListByExtensionsWithSizes: AllowedExtensions array is empty, returning empty result"));
        return;
    }
    for (int32 i = 0; i < FileList.Num(); ++i)
    {
        const FString& Filename = FileList[i];
        for (const FString& Ext : AllowedExtensions)
        {
            FString NormalizedExt = Ext;
            if (!NormalizedExt.StartsWith(TEXT(".")))
                NormalizedExt = TEXT(".") + NormalizedExt;
            if (Filename.EndsWith(NormalizedExt, ESearchCase::IgnoreCase))
            {
                OutFilteredFiles.Add(Filename);
                OutFilteredSizes.Add(FileSizes[i]);
                break;
            }
        }
    }
    UE_LOG(LogJUSYNC, Log, TEXT("FilterFileListByExtensionsWithSizes: filtered %d files down to %d (allowed extensions: %s)"),
        FileList.Num(), OutFilteredFiles.Num(), *FString::Join(AllowedExtensions, TEXT(", ")));
}

void UJUSYNCBlueprintLibrary::FilterFileListBySizeWithRanks(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks, int32 MinimumSizeBytes,
    TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes, TArray<int32>& OutFilteredRanks)
{
    OutFilteredFiles.Empty();
    OutFilteredSizes.Empty();
    OutFilteredRanks.Empty();

    if (FileList.Num() != FileSizes.Num() || FileList.Num() != FileRanks.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("FilterFileListBySizeWithRanks: Arrays have different lengths (FileList: %d, FileSizes: %d, FileRanks: %d)"),
            FileList.Num(), FileSizes.Num(), FileRanks.Num());
        return;
    }

    for (int32 i = 0; i < FileList.Num(); ++i)
    {
        if (FileSizes[i] >= MinimumSizeBytes)
        {
            OutFilteredFiles.Add(FileList[i]);
            OutFilteredSizes.Add(FileSizes[i]);
            OutFilteredRanks.Add(FileRanks[i]);
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("FilterFileListBySizeWithRanks: filtered %d files down to %d (threshold %d bytes)"),
        FileList.Num(), OutFilteredFiles.Num(), MinimumSizeBytes);
}

void UJUSYNCBlueprintLibrary::FilterFileListByExtensionEnumWithSizesAndRanks(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks,
    EJUSYNCExtension ExtensionFilter, TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes, TArray<int32>& OutFilteredRanks)
{
    OutFilteredFiles.Empty();
    OutFilteredSizes.Empty();
    OutFilteredRanks.Empty();

    if (FileList.Num() != FileSizes.Num() || FileList.Num() != FileRanks.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("FilterFileListByExtensionEnumWithSizesAndRanks: Arrays have different lengths (FileList: %d, FileSizes: %d, FileRanks: %d)"),
            FileList.Num(), FileSizes.Num(), FileRanks.Num());
        return;
    }

    TArray<FString> Extensions;
    switch (ExtensionFilter)
    {
    case EJUSYNCExtension::USD:
        Extensions.Add(TEXT(".usda"));
        break;
    case EJUSYNCExtension::PNG:
        Extensions.Add(TEXT(".png"));
        break;
    case EJUSYNCExtension::JSON:
        Extensions.Add(TEXT(".json"));
        break;
    case EJUSYNCExtension::TXT:
        Extensions.Add(TEXT(".txt"));
        break;
    case EJUSYNCExtension::BIN:
        Extensions.Add(TEXT(".bin"));
        break;
    case EJUSYNCExtension::ALL:
        OutFilteredFiles = FileList;
        OutFilteredSizes = FileSizes;
        OutFilteredRanks = FileRanks;
        UE_LOG(LogJUSYNC, Log, TEXT("FilterFileListByExtensionEnumWithSizesAndRanks: ALL selected, returning all %d files"), FileList.Num());
        return;
    default:
        UE_LOG(LogJUSYNC, Warning, TEXT("FilterFileListByExtensionEnumWithSizesAndRanks: unknown extension enum value %d"), (int32)ExtensionFilter);
        return;
    }

    for (int32 i = 0; i < FileList.Num(); ++i)
    {
        const FString& Filename = FileList[i];
        for (const FString& Ext : Extensions)
        {
            if (Filename.EndsWith(Ext, ESearchCase::IgnoreCase))
            {
                OutFilteredFiles.Add(Filename);
                OutFilteredSizes.Add(FileSizes[i]);
                OutFilteredRanks.Add(FileRanks[i]);
                break;
            }
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("FilterFileListByExtensionEnumWithSizesAndRanks: filtered %d files down to %d (extension filter: %d)"),
        FileList.Num(), OutFilteredFiles.Num(), (int32)ExtensionFilter);
}

void UJUSYNCBlueprintLibrary::FilterFileListByExtensionsWithSizesAndRanks(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks,
    const TArray<FString>& AllowedExtensions, TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes, TArray<int32>& OutFilteredRanks)
{
    OutFilteredFiles.Empty();
    OutFilteredSizes.Empty();
    OutFilteredRanks.Empty();

    if (FileList.Num() != FileSizes.Num() || FileList.Num() != FileRanks.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("FilterFileListByExtensionsWithSizesAndRanks: Arrays have different lengths (FileList: %d, FileSizes: %d, FileRanks: %d)"),
            FileList.Num(), FileSizes.Num(), FileRanks.Num());
        return;
    }

    if (AllowedExtensions.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("FilterFileListByExtensionsWithSizesAndRanks: AllowedExtensions array is empty, returning empty results"));
        return;
    }

    for (int32 i = 0; i < FileList.Num(); ++i)
    {
        const FString& Filename = FileList[i];
        for (const FString& Ext : AllowedExtensions)
        {
            if (Filename.EndsWith(Ext, ESearchCase::IgnoreCase))
            {
                OutFilteredFiles.Add(Filename);
                OutFilteredSizes.Add(FileSizes[i]);
                OutFilteredRanks.Add(FileRanks[i]);
                break;
            }
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("FilterFileListByExtensionsWithSizesAndRanks: filtered %d files down to %d (allowed extensions: %s)"),
        FileList.Num(), OutFilteredFiles.Num(), *FString::Join(AllowedExtensions, TEXT(", ")));
}

void UJUSYNCBlueprintLibrary::ExtractGeometryClips(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks,
    TArray<FString>& OutFilteredFiles, TArray<int64>& OutFilteredSizes, TArray<int32>& OutFilteredRanks)
{
    OutFilteredFiles.Empty();
    OutFilteredSizes.Empty();
    OutFilteredRanks.Empty();

    if (FileList.Num() != FileSizes.Num() || FileList.Num() != FileRanks.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("ExtractGeometryClips: Arrays have different lengths (FileList: %d, FileSizes: %d, FileRanks: %d)"),
            FileList.Num(), FileSizes.Num(), FileRanks.Num());
        return;
    }

    for (int32 i = 0; i < FileList.Num(); ++i)
    {
        const FString& Filename = FileList[i];
        if (Filename.StartsWith(TEXT("clips/"), ESearchCase::CaseSensitive))
        {
            OutFilteredFiles.Add(Filename);
            OutFilteredSizes.Add(FileSizes[i]);
            OutFilteredRanks.Add(FileRanks[i]);
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("ExtractGeometryClips: filtered %d files down to %d geometry clips"),
        FileList.Num(), OutFilteredFiles.Num());
}

int32 UJUSYNCBlueprintLibrary::CalculateTimeoutFromFileSize(int64 FileSizeBytes, int32 BaseTimeoutMs, float BandwidthBytesPerSecond)
{
    if (FileSizeBytes <= 0)
        return BaseTimeoutMs;

    float TransferTimeSeconds = static_cast<float>(FileSizeBytes) / BandwidthBytesPerSecond;
    int32 AdditionalMs = FMath::CeilToInt(TransferTimeSeconds * 1000.0f);
    AdditionalMs = FMath::CeilToInt(AdditionalMs * 1.2f);
    int32 TotalTimeout = BaseTimeoutMs + AdditionalMs;

    const int32 MaxTimeout = 60000;
    if (TotalTimeout > MaxTimeout)
        TotalTimeout = MaxTimeout;

    UE_LOG(LogJUSYNC, Log, TEXT("CalculateTimeoutFromFileSize: size=%lld bytes, bandwidth=%.0f B/s, base=%d ms, total=%d ms"),
        FileSizeBytes, BandwidthBytesPerSecond, BaseTimeoutMs, TotalTimeout);
    return TotalTimeout;
}

FString UJUSYNCBlueprintLibrary::GetJUSYNCMeshStatistics(const FJUSYNCMeshData& MeshData)
{
    return FString::Printf(TEXT("Mesh '%s': %d vertices, %d triangles, %s normals, %s UVs"),
        *MeshData.ElementName,
        MeshData.GetVertexCount(),
        MeshData.GetTriangleCount(),
        MeshData.HasNormals() ? TEXT("has") : TEXT("no"),
        MeshData.HasUVs() ? TEXT("has") : TEXT("no"));
}

FString UJUSYNCBlueprintLibrary::GetJUSYNCTextureStatistics(const FJUSYNCTextureData& TextureData)
{
    return FString::Printf(TEXT("Texture: %dx%d, %d channels, %d bytes, %s"),
        TextureData.Width,
        TextureData.Height,
        TextureData.Channels,
        TextureData.Data.Num(),
        TextureData.IsValid() ? TEXT("valid") : TEXT("invalid"));
}

bool UJUSYNCBlueprintLibrary::LoadFileToBuffer(const FString& FilePath, TArray<uint8>& OutBuffer)
{
    if (!ValidateFilePath(FilePath, TEXT("LoadFileToBuffer")))
    {
        return false;
    }

    bool bResult = FFileHelper::LoadFileToArray(OutBuffer, *FilePath);

    if (bResult)
    {
        FString Message = FString::Printf(TEXT("File loaded: %s (%d bytes)"), *FilePath, OutBuffer.Num());
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::SaveBufferToFile(const TArray<uint8>& Buffer, const FString& FilePath)
{
    if (!ValidateBufferSize(Buffer, TEXT("SaveBufferToFile")))
    {
        return false;
    }

    if (!ValidateFilePath(FilePath, TEXT("SaveBufferToFile")))
    {
        return false;
    }

    bool bResult = FFileHelper::SaveArrayToFile(Buffer, *FilePath);

    if (bResult)
    {
        FString Message = FString::Printf(TEXT("File saved: %s (%d bytes)"), *FilePath, Buffer.Num());
    }

    return bResult;
}

void UJUSYNCBlueprintLibrary::DisplayDebugMessage(const FString& Message, float Duration, FLinearColor Color)
{

    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC: %s"), *Message);
}

void UJUSYNCBlueprintLibrary::LogJUSYNCMessage(const FString& Message, bool bIsError)
{
    if (bIsError)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC: %s"), *Message);
    }
    else
    {
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC: %s"), *Message);
    }
}

UJUSYNCSubsystem* UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem()
{
    if (GWorld)
    {
        if (UGameInstance* GameInstance = GWorld->GetGameInstance())
        {
            if (UJUSYNCSubsystem* Subsystem = GameInstance->GetSubsystem<UJUSYNCSubsystem>())
            {
                return Subsystem;
            }
        }
    }

    if (GEngine)
    {
        for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
        {
            if (WorldContext.World() && WorldContext.WorldType == EWorldType::PIE)
            {
                if (UGameInstance* GameInstance = WorldContext.World()->GetGameInstance())
                {
                    if (UJUSYNCSubsystem* Subsystem = GameInstance->GetSubsystem<UJUSYNCSubsystem>())
                    {
                        return Subsystem;
                    }
                }
            }
        }

        for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
        {
            if (WorldContext.World() && WorldContext.WorldType == EWorldType::Game)
            {
                if (UGameInstance* GameInstance = WorldContext.World()->GetGameInstance())
                {
                    if (UJUSYNCSubsystem* Subsystem = GameInstance->GetSubsystem<UJUSYNCSubsystem>())
                    {
                        return Subsystem;
                    }
                }
            }
        }
    }

    UE_LOG(LogJUSYNC, Warning, TEXT("JUSYNC Subsystem not found in any world context"));
    return nullptr;
}

bool UJUSYNCBlueprintLibrary::ValidateBufferSize(const TArray<uint8>& Buffer, const FString& Context)
{
    if (Buffer.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("%s: Buffer is empty"), *Context);
        return false;
    }

    const size_t MaxSize = 1024 * 1024 * 1024;  // 1GB limit from your middleware
    if (Buffer.Num() > MaxSize)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("%s: Buffer too large (%d bytes, max: %llu)"), *Context, Buffer.Num(), MaxSize);
        return false;
    }

    return true;
}

bool UJUSYNCBlueprintLibrary::ValidateFilePath(const FString& FilePath, const FString& Context)
{
    if (FilePath.IsEmpty())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("%s: File path is empty"), *Context);
        return false;
    }

    if (FilePath.Len() > 1000)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("%s: File path too long"), *Context);
        return false;
    }

    if (FilePath.Contains(TEXT("..")) || FilePath.Contains(TEXT("~/")))
    {
        UE_LOG(LogJUSYNC, Error, TEXT("%s: Unsafe file path detected: %s"), *Context, *FilePath);
        return false;
    }

    return true;
}

FString UJUSYNCBlueprintLibrary::ExtractUSDAPreview(const TArray<uint8>& Buffer, int32 MaxLines)
{
    if (Buffer.Num() == 0)
    {
        return TEXT("Empty buffer");
    }

    const uint8* BufferData = Buffer.GetData();
    if (!BufferData)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("ExtractUSDAPreview: Buffer has null data pointer"));
        return TEXT("Invalid buffer");
    }

    const int32 MAX_PREVIEW_SIZE = 64 * 1024;  // 64KB
    int32 SearchSize = FMath::Min(Buffer.Num(), MAX_PREVIEW_SIZE);

    FString Content;
    Content.Reserve(SearchSize);

    const int32 CHUNK_SIZE = 4096;
    for (int32 i = 0; i < SearchSize; i += CHUNK_SIZE)
    {
        int32 ChunkEnd = FMath::Min(i + CHUNK_SIZE, SearchSize);
        FString Chunk;
        Chunk.Reserve(CHUNK_SIZE);

        for (int32 j = i; j < ChunkEnd; ++j)
        {
            char Char = static_cast<char>(BufferData[j]);
            if ((Char >= 32 && Char <= 126) || Char == '\n' || Char == '\r' || Char == '\t')
            {
                Chunk.AppendChar(Char);
            }
            else if (Char == 0)
            {
                Chunk.AppendChar(' ');
            }
        }

        Content += Chunk;
    }

    TArray<FString> Lines;
    Content.ParseIntoArrayLines(Lines, false);

    FString Preview = TEXT("=== USD PREVIEW ===\n");
    int32 LinesToShow = FMath::Min(MaxLines, Lines.Num());

    for (int32 i = 0; i < LinesToShow; ++i)
    {
        FString Line = Lines[i];
        if (Line.Len() > 200)
        {
            Line = Line.Left(200) + TEXT("...");
        }
        Preview += FString::Printf(TEXT("Line %d: %s\n"), i + 1, *Line);
    }

    if (Lines.Num() > MaxLines)
    {
        Preview += FString::Printf(TEXT("... (%d more lines)\n"), Lines.Num() - MaxLines);
    }

    Preview += TEXT("=== END PREVIEW ===");

    return Preview;
}

void UJUSYNCBlueprintLibrary::ApplyEnhancedDefaultMaterial(URealtimeMeshComponent* MeshComp)
{
    if (!MeshComp)
    {
        return;
    }

    UMaterial* DefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
    if (DefaultMaterial)
    {
        UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(DefaultMaterial, MeshComp);
        if (DynamicMaterial)
        {
            DynamicMaterial->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
            DynamicMaterial->SetScalarParameterValue(TEXT("Roughness"), 0.8f);
            DynamicMaterial->SetVectorParameterValue(TEXT("BaseColor"), FLinearColor::White);
            MeshComp->SetMaterial(0, DynamicMaterial);
            UE_LOG(LogJUSYNC, Log, TEXT("Applied default material"));
        }
    }
}

void UJUSYNCBlueprintLibrary::AsyncBatchSpawnInternal(
    const TArray<FJUSYNCMeshData>& MeshDataArray,
    const TArray<FVector>& SpawnLocations,
    const TArray<FRotator>& SpawnRotations,
    TSharedPtr<TArray<AActor*>> SharedResults,
    int32 CurrentBatch,
    int32 BatchSize,
    float BatchDelay)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("No subsystem for async spawn"));
        return;
    }

    UWorld* World = Subsystem->GetWorld();
    if (!World)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("No world for async spawn"));
        return;
    }

    int32 StartIndex = CurrentBatch * BatchSize;
    int32 EndIndex = FMath::Min(StartIndex + BatchSize, MeshDataArray.Num());

    UE_LOG(LogJUSYNC, Log, TEXT("Processing async batch %d: indices %d-%d with rotations"),
        CurrentBatch, StartIndex, EndIndex - 1);

    for (int32 i = StartIndex; i < EndIndex; ++i)
    {
        FRotator UERotation = ConvertParaViewToUERotation(SpawnRotations[i]);
        AActor* SpawnedActor = SpawnRealtimeMeshAtLocation(
            MeshDataArray[i], SpawnLocations[i], UERotation);

        SharedResults->Add(SpawnedActor);

        if (SpawnedActor)
        {
            int32 Rank = ExtractRankFromFilename(MeshDataArray[i].ElementName);

            UE_LOG(LogJUSYNC, Log, TEXT("Async spawned mesh %d (Rank %d) at %s with rotation %s"),
                i, Rank, *SpawnLocations[i].ToString(), *UERotation.ToString());
        }
    }

    if (EndIndex >= MeshDataArray.Num())
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Async batch spawn complete: %d/%d successful"),
            SharedResults->Num(), MeshDataArray.Num());
        return;
    }

    FTimerHandle TimerHandle;
    World->GetTimerManager().SetTimer(TimerHandle,
        FTimerDelegate::CreateLambda([=]()
            {
                AsyncBatchSpawnInternal(MeshDataArray, SpawnLocations, SpawnRotations, SharedResults,
                    CurrentBatch + 1, BatchSize, BatchDelay);
            }),
        BatchDelay, false);
}

FString UJUSYNCBlueprintLibrary::DetectUSDContentType(const TArray<uint8>& Buffer)
{
    const int32 MAX_SCAN_SIZE = 16 * 1024;  // 16KB is enough for content detection
    int32 ScanSize = FMath::Min(Buffer.Num(), MAX_SCAN_SIZE);

    if (ScanSize == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("DetectUSDContentType: Empty buffer"));
        return TEXT("GEOMETRY_ONLY");
    }

    const uint8* BufferData = Buffer.GetData();
    if (!BufferData)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("DetectUSDContentType: Buffer has null data pointer"));
        return TEXT("GEOMETRY_ONLY");
    }

    FString FirstChunk;
    FirstChunk.Reserve(ScanSize);

    for (int32 i = 0; i < ScanSize; ++i)
    {
        char Char = static_cast<char>(BufferData[i]);
        if ((Char >= 32 && Char <= 126) || Char == '\n' || Char == '\r' || Char == '\t')
        {
            FirstChunk.AppendChar(Char);
        }
        else if (Char == 0)
        {
            FirstChunk.AppendChar(' ');
        }
    }

#if JUSYNC_VERBOSE_LOGGING
    UE_LOG(LogJUSYNC, Log, TEXT("Buffer size: %d bytes, Scanned: %d bytes"), Buffer.Num(), ScanSize);
    UE_LOG(LogJUSYNC, Log, TEXT("First chunk length: %d characters"), FirstChunk.Len());
#endif

    bool bHasPrimvarsColor = FirstChunk.Contains(TEXT("primvars:color.timeSamples"));

#if JUSYNC_VERBOSE_LOGGING
    UE_LOG(LogJUSYNC, Log, TEXT("Contains 'primvars:color.timeSamples': %s"),
        bHasPrimvarsColor ? TEXT("YES") : TEXT("NO"));
#endif

    if (bHasPrimvarsColor)
    {
        bool bHasActualColorData = FirstChunk.Contains(TEXT("0: [(0.")) ||
            FirstChunk.Contains(TEXT("0: [(1.")) ||
            FirstChunk.Contains(TEXT("), (0.")) ||
            FirstChunk.Contains(TEXT("), (1.")) ||
            (FirstChunk.Contains(TEXT("0: [(")) && FirstChunk.Contains(TEXT("), (")));

        UE_LOG(LogJUSYNC, Log, TEXT("Contains actual color data patterns: %s"),
            bHasActualColorData ? TEXT("YES") : TEXT("NO"));

        if (bHasActualColorData)
        {
            UE_LOG(LogJUSYNC, Log, TEXT("DETECTED: VERTEX_COLORS (actual color data found)"));
            return TEXT("VERTEX_COLORS");
        }
        else if (FirstChunk.Contains(TEXT("0: None")))
        {
            UE_LOG(LogJUSYNC, Log, TEXT("DETECTED: TEXTURES (None values found, no color data)"));
            return TEXT("TEXTURES");
        }
    }

    if (FirstChunk.Contains(TEXT("asset inputs:file")) ||
        FirstChunk.Contains(TEXT("UsdUVTexture")) ||
        FirstChunk.Contains(TEXT(".jpg")) ||
        FirstChunk.Contains(TEXT(".png")))
    {
        UE_LOG(LogJUSYNC, Log, TEXT("DETECTED: TEXTURES (explicit references)"));
        return TEXT("TEXTURES");
    }

    UE_LOG(LogJUSYNC, Log, TEXT("DETECTED: GEOMETRY_ONLY"));
    return TEXT("GEOMETRY_ONLY");
}

TArray<AActor*> UJUSYNCBlueprintLibrary::BatchSpawnRealtimeMeshesAtLocationsSync(
    const TArray<FJUSYNCMeshData>& MeshDataArray,
    const TArray<FVector>& SpawnLocations,
    const TArray<FRotator>& SpawnRotations)
{
    TArray<AActor*> SpawnedActors;

    TArray<FRotator> FinalRotations = SpawnRotations;
    if (FinalRotations.Num() == 0)
    {
        FinalRotations = GenerateDefaultRotations(MeshDataArray.Num());
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Processing %d meshes with locations and rotations"), MeshDataArray.Num());

    SpawnedActors.Reserve(MeshDataArray.Num());
    int32 SuccessCount = 0;

    for (int32 i = 0; i < MeshDataArray.Num(); ++i)
    {
        FRotator UERotation = ConvertParaViewToUERotation(FinalRotations[i]);

        int32 Rank = ExtractRankFromFilename(MeshDataArray[i].ElementName);

        UE_LOG(LogJUSYNC, Log, TEXT("Spawning mesh %d (Rank %d) '%s' at location %s with rotation %s"),
            i, Rank, *MeshDataArray[i].ElementName, *SpawnLocations[i].ToString(), *UERotation.ToString());

        AActor* SpawnedActor = SpawnRealtimeMeshAtLocation(MeshDataArray[i], SpawnLocations[i], UERotation);
        SpawnedActors.Add(SpawnedActor);

        if (SpawnedActor)
        {
            SuccessCount++;
            UE_LOG(LogJUSYNC, Log, TEXT("Spawned at %s with rotation %s"),
                *SpawnedActor->GetActorLocation().ToString(),
                *SpawnedActor->GetActorRotation().ToString());
        }
        else
        {
            UE_LOG(LogJUSYNC, Error, TEXT("Failed to spawn mesh %d"), i);
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Sync batch spawn complete: %d/%d successful"),
        SuccessCount, MeshDataArray.Num());
    return SpawnedActors;
}

FRotator UJUSYNCBlueprintLibrary::ConvertParaViewToUERotation(const FRotator& ParaViewRotation)
{
    FRotator UERotation;

    UERotation.Pitch = ParaViewRotation.Pitch;
    UERotation.Yaw = -ParaViewRotation.Yaw;
    UERotation.Roll = ParaViewRotation.Roll;

    UE_LOG(LogJUSYNC, Log, TEXT("Converted ParaView rotation %s to UE rotation %s"),
        *ParaViewRotation.ToString(), *UERotation.ToString());

    return UERotation;
}

TArray<FVector> UJUSYNCBlueprintLibrary::GenerateDefaultLocations(int32 Count, const FVector& BaseLocation, float Spacing)
{
    TArray<FVector> Locations;
    Locations.Reserve(Count);

    if (Count <= 0)
    {
        return Locations;
    }

    int32 GridSize = FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Count)));

    for (int32 i = 0; i < Count; ++i)
    {
        int32 Row = i / GridSize;
        int32 Col = i % GridSize;

        FVector Location = BaseLocation + FVector(Col * Spacing, Row * Spacing, 0.0f);
        Locations.Add(Location);
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Generated %d default locations in %dx%d grid (spacing: %.1f)"),
        Count, GridSize, GridSize, Spacing);

    return Locations;
}

TArray<FRotator> UJUSYNCBlueprintLibrary::GenerateDefaultRotations(int32 Count, const FRotator& BaseRotation)
{
    TArray<FRotator> Rotations;
    Rotations.Reserve(Count);

    for (int32 i = 0; i < Count; ++i)
    {
        Rotations.Add(BaseRotation);
    }

    return Rotations;
}

AActor* UJUSYNCBlueprintLibrary::SpawnRealtimeMeshWithMaterial(
    const FJUSYNCMeshData& MeshData,
    const FVector& SpawnLocation,
    const FRotator& SpawnRotation,
    UMaterialInterface* Material,
    bool bUseUniformScaling,
    FVector OuterBoundingBoxSize,
    bool bPreserveAspectRatio,
    bool bUseAsyncSpawning)
{
    if (MeshData.Vertices.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("Empty mesh data provided"));
        return nullptr;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return nullptr;
    }

    UWorld* World = Subsystem->GetWorld();
    if (!World)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("No valid world context"));
        return nullptr;
    }

    FJUSYNCMeshData ProcessedMeshData = FixMeshDataForSpawning(MeshData);
    FRotator UERotation = ConvertParaViewToUERotation(SpawnRotation);

    UE_LOG(LogJUSYNC, Log, TEXT("Spawning mesh '%s' at location %s with rotation %s"),
        *ProcessedMeshData.ElementName, *SpawnLocation.ToString(), *UERotation.ToString());

    FVector MeshScaleFactor = FVector::OneVector;
    if (bUseUniformScaling && OuterBoundingBoxSize != FVector::ZeroVector)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Applying uniform scaling with bounding box: %s"),
            *OuterBoundingBoxSize.ToString());

        FVector MeshSize(40.0f, 40.0f, 40.0f);
        if (ProcessedMeshData.Vertices.Num() > 0)
        {
            FBox MeshBounds(EForceInit::ForceInit);
            for (const FVector& Vertex : ProcessedMeshData.Vertices)
            {
                MeshBounds += Vertex;
            }
            MeshSize = MeshBounds.GetSize();
            UE_LOG(LogJUSYNC, Log, TEXT("Mesh size from vertices: %s"), *MeshSize.ToString());
        }
        else
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Mesh has no vertices, using default size"));
        }

        if (bPreserveAspectRatio)
        {
            float MinScale = FMath::Min3(
                MeshSize.X > 0 ? OuterBoundingBoxSize.X / MeshSize.X : 1.0f,
                MeshSize.Y > 0 ? OuterBoundingBoxSize.Y / MeshSize.Y : 1.0f,
                MeshSize.Z > 0 ? OuterBoundingBoxSize.Z / MeshSize.Z : 1.0f
            );
            MeshScaleFactor = FVector(MinScale, MinScale, MinScale);
        }
        else
        {
            MeshScaleFactor = FVector(
                MeshSize.X > 0 ? OuterBoundingBoxSize.X / MeshSize.X : 1.0f,
                MeshSize.Y > 0 ? OuterBoundingBoxSize.Y / MeshSize.Y : 1.0f,
                MeshSize.Z > 0 ? OuterBoundingBoxSize.Z / MeshSize.Z : 1.0f
            );
        }

        UE_LOG(LogJUSYNC, Log, TEXT("Mesh scale factor: %s (MeshSize: %s, TargetSize: %s)"),
            *MeshScaleFactor.ToString(), *MeshSize.ToString(), *OuterBoundingBoxSize.ToString());
    }
    else
    {
        UE_LOG(LogJUSYNC, Log, TEXT("No uniform scaling applied"));
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

    AActor* SpawnedActor = World->SpawnActor<AActor>(SpawnParams);
    if (!SpawnedActor)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to spawn actor"));
        return nullptr;
    }

    SpawnedActor->Tags.Add(FName(*FString::Printf(TEXT("JUSYNC_%s"), *ProcessedMeshData.ElementName)));
    UE_LOG(LogJUSYNC, Log, TEXT("Spawned actor with auto-generated name: %s"), *SpawnedActor->GetName());

    URealtimeMeshComponent* MeshComp = NewObject<URealtimeMeshComponent>(SpawnedActor);
    SpawnedActor->SetRootComponent(MeshComp);
    MeshComp->RegisterComponent();

    FTransform ActorTransform(UERotation, SpawnLocation, MeshScaleFactor);
    SpawnedActor->SetActorTransform(ActorTransform);

    if (bUseUniformScaling && MeshScaleFactor != FVector::OneVector)
    {
        if (!MeshComp)
        {
            UE_LOG(LogJUSYNC, Error, TEXT("Cannot apply scaling: MeshComp is null"));
        }
        else if (!SpawnedActor->GetRootComponent())
        {
            UE_LOG(LogJUSYNC, Error, TEXT("Cannot apply scaling: Actor has no root component"));
        }
        else
        {
            MeshComp->SetWorldScale3D(MeshScaleFactor);
            SpawnedActor->SetActorScale3D(MeshScaleFactor);
            SpawnedActor->SetActorTransform(FTransform(UERotation, SpawnLocation, MeshScaleFactor));
            MeshComp->MarkRenderStateDirty();

            FVector ActualActorScale = SpawnedActor->GetActorScale3D();
            FVector ActualComponentScale = MeshComp->GetComponentScale();

            if (ActualActorScale.Equals(MeshScaleFactor, 0.01f) && ActualComponentScale.Equals(MeshScaleFactor, 0.01f))
            {
                UE_LOG(LogJUSYNC, Log, TEXT("Applied scale %s to actor '%s' (Verified: Actor=%s, Component=%s)"),
                    *MeshScaleFactor.ToString(), *ProcessedMeshData.ElementName,
                    *ActualActorScale.ToString(), *ActualComponentScale.ToString());
            }
            else
            {
                UE_LOG(LogJUSYNC, Error, TEXT("Scaling mismatch for actor: Target=%s, Actor=%s, Component=%s"),
                    *MeshScaleFactor.ToString(),
                    *ActualActorScale.ToString(), *ActualComponentScale.ToString());
            }
        }
    }

    if (Material)
    {
        MeshComp->SetMaterial(0, Material);
        UE_LOG(LogJUSYNC, Log, TEXT("Applied provided material to mesh"));
    }
    else
    {
        ApplyEnhancedDefaultMaterial(MeshComp);
        UE_LOG(LogJUSYNC, Log, TEXT("Applied default material to mesh"));
    }

    bool bSuccess;

    const int32 TotalVertices = ProcessedMeshData.Vertices.Num();
    const int32 TotalTriangles = ProcessedMeshData.Triangles.Num() / 3;
    const int32 RMCVertexLimit = 32768;  // From CheckMemoryLimitsForMesh
    const int32 RMCTriangleLimit = 65536;  // From CheckMemoryLimitsForMesh

    bool bNeedsSplitting = (TotalVertices > RMCVertexLimit) || (TotalTriangles > RMCTriangleLimit);

    if (bUseAsyncSpawning)
    {
        if (bNeedsSplitting)
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Large mesh detected (%d vertices, %d triangles) - using synchronous splitting with async spawn"),
                TotalVertices, TotalTriangles);

            bSuccess = Subsystem->CreateRealtimeMeshFromJUSYNCWithSplitting(
                ProcessedMeshData,
                MeshComp,
                RMCVertexLimit
            );
            UE_LOG(LogJUSYNC, Log, TEXT("Using SPLITTING mesh creation (%d vertices > %d limit)"),
                TotalVertices, RMCVertexLimit);
        }
        else
        {

            Subsystem->CreateRealtimeMeshFromJUSYNC_Async(ProcessedMeshData, MeshComp);
            bSuccess = true;
            UE_LOG(LogJUSYNC, Log, TEXT("Using ASYNC mesh creation"));
        }

        UE_LOG(LogJUSYNC, Log, TEXT("Mesh creation started for mesh at %s"),
            *SpawnLocation.ToString());
    }
    else
    {
        if (bNeedsSplitting)
        {
            bSuccess = Subsystem->CreateRealtimeMeshFromJUSYNCWithSplitting(
                ProcessedMeshData,
                MeshComp,
                RMCVertexLimit
            );
            UE_LOG(LogJUSYNC, Log, TEXT("Using SPLITTING mesh creation (%d vertices > %d limit)"),
                TotalVertices, RMCVertexLimit);
        }
        else
        {
            bSuccess = Subsystem->CreateRealtimeMeshFromJUSYNC(ProcessedMeshData, MeshComp);
        }

        if (bSuccess)
        {
            FVector ActualLocation = SpawnedActor->GetActorLocation();
            FVector ActualScale = SpawnedActor->GetActorScale3D();
            FRotator ActualRotation = SpawnedActor->GetActorRotation();

            UE_LOG(LogJUSYNC, Log, TEXT("Spawned mesh at %s (Scale: %s, Rotation: %s)"),
                *ActualLocation.ToString(), *ActualScale.ToString(), *ActualRotation.ToString());

            if (bNeedsSplitting)
            {
                UE_LOG(LogJUSYNC, Log, TEXT("Mesh split into multiple RMC components for better performance"));
            }
        }
        else
        {
            UE_LOG(LogJUSYNC, Error, TEXT("Failed to create RealtimeMesh for actor, destroying"));
            SpawnedActor->Destroy();
            return nullptr;
        }
    }

    return SpawnedActor;
}

AActor* UJUSYNCBlueprintLibrary::SpawnRealtimeMeshWithMaterial_Benchmarked(
    const FJUSYNCMeshData& MeshData,
    const FVector& SpawnLocation,
    const FRotator& SpawnRotation,
    UMaterialInterface* Material,
    const FJUSYNCBenchmarkConfig& Config,
    bool bUseUniformScaling,
    FVector OuterBoundingBoxSize,
    bool bPreserveAspectRatio,
    bool bUseAsyncSpawning)
{
    bool bWasBenchmarking = bIsBenchmarking;
    if (Config.bEnableBenchmarking && !bIsBenchmarking)
    {
        StartBenchmark(TEXT("SpawnRealtimeMeshWithMaterial"), Config);
    }

    FPlatformMemoryStats StatsBefore = FPlatformMemory::GetStats();
    int64 RAMBefore = StatsBefore.UsedPhysical;
    int64 VRAMBefore = GetVRAMUsageBytes();
    double StartTime = FPlatformTime::Seconds();

    AActor* SpawnedActor = SpawnRealtimeMeshWithMaterial(
        MeshData, SpawnLocation, SpawnRotation, Material,
        bUseUniformScaling, OuterBoundingBoxSize,
        bPreserveAspectRatio, bUseAsyncSpawning);

    double EndTime = FPlatformTime::Seconds();
    float TimeMs = (EndTime - StartTime) * 1000.0f;
    FPlatformMemoryStats StatsAfter = FPlatformMemory::GetStats();
    int64 RAMAfter = StatsAfter.UsedPhysical;
    int64 VRAMAfter = GetVRAMUsageBytes();

    if (Config.bEnableBenchmarking)
    {
        float CPUUsage = GetCPUUsagePercentage();
        int32 ActiveThreads = GetActiveThreadCount();
        int64 RAMPeak = StatsAfter.PeakUsedPhysical;

        float GPUUsage = 0.0f;
        UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
        if (Subsystem)
        {
            GPUUsage = Subsystem->GetGPUUsage_Percent();
        }

        FJUSYNCBenchmarkResult Result = CreateBenchmarkResultExtended(
            CurrentBenchmarkTest,
            TimeMs,
            MeshData.GetTriangleCount(),
            MeshData.GetVertexCount(),
            RAMBefore,
            RAMAfter,
            RAMPeak,
            RAMAfter,  // RAMDuring - same as after for now
            1,
            (SpawnedActor ? 0 : 1),
            0,
            CPUUsage,
            VRAMBefore,
            VRAMAfter,
            VRAMAfter,  // VRAMPeak - same as after for now
            ActiveThreads,
            GPUUsage,
            0,  // HitchCount
            0.0f,  // AvgHitchDurationMs
            0.0f  // MaxHitchDurationMs
        );

        RecordBenchmarkResult(Result);

        if (!bWasBenchmarking)
        {
            EndBenchmark();
        }
    }

    return SpawnedActor;
}

FJUSYNCMeshData UJUSYNCBlueprintLibrary::FixMeshDataForSpawning(const FJUSYNCMeshData& InputMeshData)
{
    FJUSYNCMeshData FixedData = InputMeshData;

    for (int32 i = 0; i < FixedData.Vertices.Num(); ++i)
    {
        FVector& Vertex = FixedData.Vertices[i];
        if (!FMath::IsFinite(Vertex.X) || !FMath::IsFinite(Vertex.Y) || !FMath::IsFinite(Vertex.Z))
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Fixed invalid vertex at index %d"), i);
            Vertex = FVector::ZeroVector;
        }
    }

    TArray<int32> ValidTriangles;
    ValidTriangles.Reserve(FixedData.Triangles.Num());

    for (int32 i = 0; i < FixedData.Triangles.Num(); i += 3)
    {
        if (i + 2 < FixedData.Triangles.Num())
        {
            int32 i0 = FixedData.Triangles[i];
            int32 i1 = FixedData.Triangles[i + 1];
            int32 i2 = FixedData.Triangles[i + 2];

            if (i0 >= 0 && i0 < FixedData.Vertices.Num() &&
                i1 >= 0 && i1 < FixedData.Vertices.Num() &&
                i2 >= 0 && i2 < FixedData.Vertices.Num() &&
                i0 != i1 && i1 != i2 && i0 != i2)
            {
                ValidTriangles.Add(i0);
                ValidTriangles.Add(i1);
                ValidTriangles.Add(i2);
            }
        }
    }

    FixedData.Triangles = ValidTriangles;

    if (!FixedData.HasNormals() || FixedData.Normals.Num() != FixedData.Vertices.Num())
    {
        FixedData.Normals.SetNum(FixedData.Vertices.Num());
        for (int32 i = 0; i < FixedData.Normals.Num(); ++i)
        {
            FixedData.Normals[i] = FVector::ZeroVector;
        }

        for (int32 i = 0; i < FixedData.Triangles.Num(); i += 3)
        {
            int32 i0 = FixedData.Triangles[i];
            int32 i1 = FixedData.Triangles[i + 1];
            int32 i2 = FixedData.Triangles[i + 2];

            if (i0 < FixedData.Vertices.Num() && i1 < FixedData.Vertices.Num() && i2 < FixedData.Vertices.Num())
            {
                FVector v0 = FixedData.Vertices[i0];
                FVector v1 = FixedData.Vertices[i1];
                FVector v2 = FixedData.Vertices[i2];

                FVector FaceNormal = FVector::CrossProduct(v1 - v0, v2 - v0).GetSafeNormal();

                FixedData.Normals[i0] += FaceNormal;
                FixedData.Normals[i1] += FaceNormal;
                FixedData.Normals[i2] += FaceNormal;
            }
        }

        for (int32 i = 0; i < FixedData.Normals.Num(); ++i)
        {
            FixedData.Normals[i] = FixedData.Normals[i].GetSafeNormal();
            if (FixedData.Normals[i].IsNearlyZero())
            {
                FixedData.Normals[i] = FVector::UpVector;
            }
        }

        UE_LOG(LogJUSYNC, Log, TEXT("Recalculated normals for mesh: %s"), *InputMeshData.ElementName);
    }

    if (FixedData.HasUVs())
    {
        for (int32 i = 0; i < FixedData.UVs.Num(); ++i)
        {
            FVector2D& UV = FixedData.UVs[i];
            if (!FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y))
            {
                UV = FVector2D::ZeroVector;
            }
        }
    }

    return FixedData;
}

FBox UJUSYNCBlueprintLibrary::CalculateMeshBounds(const FJUSYNCMeshData& MeshData, const FVector& Location)
{
    FBox MeshBounds(EForceInit::ForceInit);

    for (const FVector& Vertex : MeshData.Vertices)
    {
        FVector WorldVertex = Vertex + Location;
        MeshBounds += WorldVertex;
    }

    return MeshBounds;
}

TArray<FVector> UJUSYNCBlueprintLibrary::CalculateScaledPositions(
    const TArray<FVector>& OriginalLocations,
    const FVector& BoundingBoxSize,
    bool bPreserveAspectRatio,
    FVector& OutScaleFactor)
{
    if (OriginalLocations.Num() == 0 || BoundingBoxSize == FVector::ZeroVector)
    {
        OutScaleFactor = FVector::OneVector;
        return OriginalLocations;
    }

    if (OriginalLocations.Num() == 1)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Single spawn point - calculating scale based on mesh bounds"));

        FVector MeshSize(40.0f, 40.0f, 40.0f);

        if (bPreserveAspectRatio)
        {
            float MinScale = FMath::Min3(
                BoundingBoxSize.X / MeshSize.X,
                BoundingBoxSize.Y / MeshSize.Y,
                BoundingBoxSize.Z / MeshSize.Z
            );
            OutScaleFactor = FVector(MinScale, MinScale, MinScale);
        }
        else
        {
            OutScaleFactor = FVector(
                BoundingBoxSize.X / MeshSize.X,
                BoundingBoxSize.Y / MeshSize.Y,
                BoundingBoxSize.Z / MeshSize.Z
            );
        }

        UE_LOG(LogJUSYNC, Log, TEXT("Single point scale factor: %s"), *OutScaleFactor.ToString());
        return OriginalLocations;
    }

    FBox CombinedBounds(EForceInit::ForceInit);
    for (const FVector& Location : OriginalLocations)
    {
        CombinedBounds += Location;
    }

    FVector CurrentSize = CombinedBounds.GetSize();
    FVector BoundingBoxCenter = CombinedBounds.GetCenter();

    if (CurrentSize.IsNearlyZero())
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Zero-size bounding box detected - no scaling needed"));
        OutScaleFactor = FVector::OneVector;
        return OriginalLocations;
    }

    if (bPreserveAspectRatio)
    {
        float MinScale = FMath::Min3(
            CurrentSize.X > 0 ? BoundingBoxSize.X / CurrentSize.X : 1.0f,
            CurrentSize.Y > 0 ? BoundingBoxSize.Y / CurrentSize.Y : 1.0f,
            CurrentSize.Z > 0 ? BoundingBoxSize.Z / CurrentSize.Z : 1.0f
        );
        OutScaleFactor = FVector(MinScale, MinScale, MinScale);
    }
    else
    {
        OutScaleFactor = FVector(
            CurrentSize.X > 0 ? BoundingBoxSize.X / CurrentSize.X : 1.0f,
            CurrentSize.Y > 0 ? BoundingBoxSize.Y / CurrentSize.Y : 1.0f,
            CurrentSize.Z > 0 ? BoundingBoxSize.Z / CurrentSize.Z : 1.0f
        );
    }

    TArray<FVector> ScaledLocations;
    ScaledLocations.Reserve(OriginalLocations.Num());

    for (const FVector& Location : OriginalLocations)
    {
        FVector RelativePosition = Location - BoundingBoxCenter;
        FVector ScaledPosition = RelativePosition * OutScaleFactor;
        FVector NewLocation = BoundingBoxCenter + ScaledPosition;
        ScaledLocations.Add(NewLocation);
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Multi-point scaling applied: %s"), *OutScaleFactor.ToString());
    return ScaledLocations;
}

void UJUSYNCBlueprintLibrary::RequestFilesParallelAsync(
    const TArray<FString>& Filenames,
    const TArray<int32>& TargetRanks,
    int32 TimeoutMs,
    const FOnParallelFileReceived& OnFileReceived,
    const FOnParallelDownloadComplete& OnComplete,
    const FOnParallelDownloadError& OnError)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        OnError.ExecuteIfBound(TEXT(""), TEXT("Subsystem not available"));
        return;
    }

    if (!Subsystem->IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Middleware not connected to broker"));
        OnError.ExecuteIfBound(TEXT(""), TEXT("Middleware not connected"));
        return;
    }

    std::vector<std::string> FilenameStorage;
    std::vector<const char*> FilenameCStrs;
    std::vector<int32_t> TargetRanksC;

    FilenameStorage.reserve(Filenames.Num());
    FilenameCStrs.reserve(Filenames.Num());
    TargetRanksC.reserve(TargetRanks.Num());

    for (const FString& Filename : Filenames)
    {
        FTCHARToUTF8 FilenameConverter(*Filename);
        FilenameStorage.push_back(std::string(FilenameConverter.Get()));
        FilenameCStrs.push_back(FilenameStorage.back().c_str());
    }

    for (int32 Rank : TargetRanks)
    {
        TargetRanksC.push_back(Rank);
    }

    struct FParallelDownloadState : public TSharedFromThis<FParallelDownloadState>
    {
        FOnParallelFileReceived OnFileReceived;
        FOnParallelDownloadComplete OnComplete;
        FOnParallelDownloadError OnError;
        TArray<FString> Filenames;
        std::atomic<int> FilesReceived{ 0 };
        std::atomic<int> FilesExpected{ 0 };
        std::atomic<bool> bAllComplete{ false };

        void ExecuteFileReceived(const FString& Filename, const TArray<uint8>& Data)
        {
            if (OnFileReceived.IsBound())
            {
                AsyncTask(ENamedThreads::GameThread, [WeakThis = AsWeak(), Filename, Data]() {
                    if (auto Pinned = WeakThis.Pin())
                    {
                        Pinned->OnFileReceived.Execute(Filename, Data);
                    }
                    });
            }
        }

        void ExecuteComplete()
        {
            if (OnComplete.IsBound())
            {
                AsyncTask(ENamedThreads::GameThread, [WeakThis = AsWeak()]() {
                    if (auto Pinned = WeakThis.Pin())
                    {
                        Pinned->OnComplete.Execute();
                    }
                    });
            }
        }

        void ExecuteError(const FString& Filename, const FString& ErrorMessage)
        {
            if (OnError.IsBound())
            {
                AsyncTask(ENamedThreads::GameThread, [WeakThis = AsWeak(), Filename, ErrorMessage]() {
                    if (auto Pinned = WeakThis.Pin())
                    {
                        Pinned->OnError.Execute(Filename, ErrorMessage);
                    }
                    });
            }
        }
    };

    auto State = MakeShared<FParallelDownloadState>();
    State->OnFileReceived = OnFileReceived;
    State->OnComplete = OnComplete;
    State->OnError = OnError;
    State->Filenames = Filenames;
    State->FilesExpected = Filenames.Num();

    auto FileReceivedCallback = [State](const char* filename, const unsigned char* data, size_t data_size) {
        FString FilenameUTF8 = UTF8_TO_TCHAR(filename);
        TArray<uint8> DataArray;
        DataArray.Append(data, data_size);

        int Received = State->FilesReceived.fetch_add(1) + 1;
        UE_LOG(LogJUSYNC, Log, TEXT("Pipeline: File %d/%d received: %s (%d bytes)"),
            Received, State->FilesExpected.load(), *FilenameUTF8, DataArray.Num());

        State->ExecuteFileReceived(FilenameUTF8, DataArray);

        if (Received >= State->FilesExpected)
        {
            State->bAllComplete = true;
            State->ExecuteComplete();
        }
        };

    auto CompletionCallback = [State]() {
        UE_LOG(LogJUSYNC, Log, TEXT("Pipeline: All parallel downloads completed"));
        State->bAllComplete = true;
        State->ExecuteComplete();
        };

    auto ErrorCallback = [State](const char* filename, const char* error_message) {
        FString FilenameUTF8 = UTF8_TO_TCHAR(filename);
        FString ErrorUTF8 = UTF8_TO_TCHAR(error_message);

        UE_LOG(LogJUSYNC, Error, TEXT("Pipeline: Parallel download error for %s: %s"), *FilenameUTF8, *ErrorUTF8);
        State->ExecuteError(FilenameUTF8, ErrorUTF8);

        int Received = State->FilesReceived.fetch_add(1) + 1;
        if (Received >= State->FilesExpected && !State->bAllComplete)
        {
            State->bAllComplete = true;
            State->ExecuteComplete();
        }
        };

    UE_LOG(LogJUSYNC, Log, TEXT("Pipeline: Calling RequestFilesParallelAsync_C for %d files (true async)"), Filenames.Num());

    UE_LOG(LogJUSYNC, Warning, TEXT("Pipeline: Using synchronous subsystem call - files will download before callbacks"));

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, Filenames, TargetRanks, TimeoutMs, OnFileReceived, OnComplete, OnError]()
        {
            if (!WeakSubsystem.IsValid()) return;

            TArray<FJUSYNCFileData> DownloadedFiles;
            bool bSuccess = WeakSubsystem->RequestFilesParallel(Filenames, TargetRanks, TimeoutMs, DownloadedFiles);

            if (!WeakSubsystem.IsValid()) return;

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, DownloadedFiles, Filenames, OnFileReceived, OnComplete, OnError]()
                {
                    if (bSuccess)
                    {
                        for (const FJUSYNCFileData& FileData : DownloadedFiles)
                        {
                            OnFileReceived.ExecuteIfBound(FileData.Filename, FileData.Data);
                        }
                        OnComplete.ExecuteIfBound();
                    }
                    else
                    {
                        TSet<FString> DownloadedFilenames;
                        for (const FJUSYNCFileData& FileData : DownloadedFiles)
                        {
                            DownloadedFilenames.Add(FileData.Filename);
                        }

                        for (const FString& Filename : Filenames)
                        {
                            if (!DownloadedFilenames.Contains(Filename))
                            {
                                OnError.ExecuteIfBound(Filename, TEXT("Failed to download file"));
                            }
                        }
                    }
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        });
}

bool UJUSYNCBlueprintLibrary::RequestFilesParallelSync(
    const TArray<FString>& Filenames,
    const TArray<int32>& TargetRanks,
    int32 TimeoutMs,
    TArray<FJUSYNCFileData>& OutFiles)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    return Subsystem->RequestFilesParallel(Filenames, TargetRanks, TimeoutMs, OutFiles);
}

static void ApplyEnhancedDefaultMaterial(URealtimeMeshComponent* MeshComp)
{
    UMaterial* DefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
    if (DefaultMaterial)
    {
        UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(DefaultMaterial, MeshComp);
        if (DynamicMaterial)
        {
            DynamicMaterial->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
            DynamicMaterial->SetScalarParameterValue(TEXT("Roughness"), 0.8f);
            DynamicMaterial->SetVectorParameterValue(TEXT("BaseColor"), FLinearColor::White);
            MeshComp->SetMaterial(0, DynamicMaterial);
        }
    }
}

void UJUSYNCBlueprintLibrary::CreateMaterialFromTexture_Async(UTexture2D* Texture, URealtimeMeshComponent* TargetComponent)
{
    if (!Texture || !TargetComponent)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("CreateMaterialFromTexture_Async: Invalid texture or component"));
        return;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("CreateMaterialFromTexture_Async: JUSYNC subsystem not available"));
        return;
    }

    Subsystem->CreateMaterialFromTexture_Async(Texture, TargetComponent);
}

void UJUSYNCBlueprintLibrary::CreateMaterialFromTexture_Async_Return(
    UTexture2D* Texture,
    const FOnMaterialCreated& OnMaterialCreated)
{
    CreateMaterialFromTexture_Async_Return_Extended(Texture, nullptr, NAME_None, OnMaterialCreated);
}

void UJUSYNCBlueprintLibrary::CreateMaterialFromTexture_Async_Return_Extended(
    UTexture2D* Texture,
    UMaterialInterface* BaseMaterial,
    FName TextureParameterName,
    const FOnMaterialCreated& OnMaterialCreated)
{
    if (!Texture)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("CreateMaterialFromTexture_Async_Return_Extended: Invalid texture"));
        return;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("CreateMaterialFromTexture_Async_Return_Extended: JUSYNC subsystem not available"));
        return;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Creating async material from texture (Base: %s, Param: %s)"),
        BaseMaterial ? *BaseMaterial->GetName() : TEXT("Default"),
        *TextureParameterName.ToString());

    Subsystem->CreateMaterialFromTexture_Async_Return_Internal(
        Texture,
        BaseMaterial,
        TextureParameterName,
        [OnMaterialCreated](UMaterialInstanceDynamic* CreatedMaterial)
        {
            if (OnMaterialCreated.IsBound())
            {
                OnMaterialCreated.Execute(CreatedMaterial);
            }
        });
}

void UJUSYNCBlueprintLibrary::StartBenchmark(const FString& TestName, const FJUSYNCBenchmarkConfig& Config)
{
    if (bIsBenchmarking)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("Benchmark already in progress: %s"), *CurrentBenchmarkTest);
        return;
    }

    ClearBenchmarkResults();

    FJUSYNCBenchmarkTiming::Get().Reset();

    CurrentBenchmarkTest = TestName;
    CurrentBenchmarkConfig = Config;
    bIsBenchmarking = true;
    BenchmarkSessionStartTime = FDateTime::UtcNow();

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->StartMetricsCollection();
        UE_LOG(LogJUSYNC, Log, TEXT("Started metrics collection for benchmark"));
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Started benchmark: %s at %s"), *TestName, *BenchmarkSessionStartTime.ToString());
}

void UJUSYNCBlueprintLibrary::EndBenchmark()
{
    if (!bIsBenchmarking)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("No benchmark in progress"));
        return;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Ended benchmark: %s"), *CurrentBenchmarkTest);

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->StopMetricsCollection();
        UE_LOG(LogJUSYNC, Log, TEXT("Stopped metrics collection for benchmark"));
    }

    if (!CurrentBenchmarkConfig.OutputDirectory.IsEmpty())
    {
        switch (CurrentBenchmarkConfig.OutputFormat)
        {
        case 0:  // CSV only
            SaveAllBenchmarkResultsToCSV(CurrentBenchmarkConfig.OutputDirectory);
            break;
        case 1:  // JSON only
            SaveAllBenchmarkResultsToJSON(CurrentBenchmarkConfig.OutputDirectory);
            break;
        case 2:  // Both CSV and JSON
            SaveAllBenchmarkResultsToCSV(CurrentBenchmarkConfig.OutputDirectory);
            SaveAllBenchmarkResultsToJSON(CurrentBenchmarkConfig.OutputDirectory);
            break;
        default:
            SaveAllBenchmarkResultsToCSV(CurrentBenchmarkConfig.OutputDirectory);
            break;
        }
    }

    CurrentBenchmarkTest = TEXT("");
    CurrentBenchmarkConfig = FJUSYNCBenchmarkConfig();
    bIsBenchmarking = false;
}

void UJUSYNCBlueprintLibrary::SaveAllBenchmarkResultsToCSV(const FString& OutputDirectory)
{
    if (BenchmarkResults.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("No benchmark results to save"));
        return;
    }

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    FString OutputPath = OutputDirectory;
    PlatformFile.CreateDirectoryTree(*OutputPath);

    FString Filename = TEXT("benchmark_results");
    if (CurrentBenchmarkConfig.bAppendTimestamp)
    {
        Filename += TEXT("_") + FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
    }
    Filename += TEXT(".csv");

    FString CSVPath = OutputPath / Filename;

    FString CSVData = TEXT("TestName,Timestamp,TotalTimeMs,TriangleCount,VertexCount,RAMBeforeMB,RAMAfterMB,FPS,FrameTimeMs,ActorCount,ErrorCount,SuccessRate,SplitMeshCount,InteractWindowMs,FirstFrameMs,FullSceneMs,FPSMeasured,VRAMBeforeMB,VRAMAfterMB,VRAMPeakMB,ChunkCount,BytesReceivedMB,FilesRequested,FilesCompleted,DownloadTotalMs,ClusterWorkers,ClusterCommits,ClusterRawPayloadMB,ClusterServingMB,ClusterServingChunks,ClusterSerializeMs,ClusterStoreMs\n");

    for (const FJUSYNCBenchmarkResult& Result : BenchmarkResults)
    {
        CSVData += FString::Printf(TEXT("%s,%s,%.2f,%d,%d,%.2f,%.2f,%.1f,%.2f,%d,%d,%.1f,%d,%.2f,%.2f,%.2f,%d,%.2f,%.2f,%.2f,%lld,%.3f,%lld,%lld,%.2f,%d,%lld,%.3f,%.3f,%lld,%.2f,%.2f\n"),
            *Result.TestName,
            *Result.Timestamp.ToString(TEXT("%Y-%m-%d %H:%M:%S")),
            Result.TotalTimeMs,
            Result.TriangleCount,
            Result.VertexCount,
            Result.RAMBeforeBytes / (1024.0f * 1024.0f),
            Result.RAMAfterBytes / (1024.0f * 1024.0f),
            Result.FPS,
            Result.FrameTimeMs,
            Result.ActorCount,
            Result.ErrorCount,
            Result.SuccessRate,
            Result.SplitMeshCount,
            Result.InteractWindowMs,
            Result.FirstFrameMs,
            Result.FullSceneMs,
            Result.bFPSMeasured ? 1 : 0,
            Result.VRAMBeforeBytes / (1024.0f * 1024.0f),
            Result.VRAMAfterBytes / (1024.0f * 1024.0f),
            Result.VRAMPeakBytes / (1024.0f * 1024.0f),
            Result.ChunkCount,
            Result.BytesReceived / (1024.0f * 1024.0f),
            Result.FilesRequested,
            Result.FilesCompleted,
            Result.DownloadTotalMs,
            Result.ClusterWorkerCount,
            Result.ClusterCommitCount,
            Result.ClusterRawBytes / (1024.0f * 1024.0f),
            Result.ClusterServingBytes / (1024.0f * 1024.0f),
            Result.ClusterServingChunks,
            Result.ClusterSerializeMsTotal,
            Result.ClusterStoreMsTotal
        );
    }

    if (FFileHelper::SaveStringToFile(CSVData, *CSVPath))
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Benchmark results saved to: %s"), *CSVPath);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to save benchmark results to: %s"), *CSVPath);
    }
}

void UJUSYNCBlueprintLibrary::SaveAllBenchmarkResultsToJSON(const FString& OutputDirectory)
{
    if (BenchmarkResults.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("No benchmark results to save"));
        return;
    }

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    FString OutputPath = OutputDirectory;
    PlatformFile.CreateDirectoryTree(*OutputPath);

    FString Filename = TEXT("benchmark_results");
    if (CurrentBenchmarkConfig.bAppendTimestamp)
    {
        Filename += TEXT("_") + FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
    }
    Filename += TEXT(".json");

    FString JSONPath = OutputPath / Filename;

    FString JSONData = TEXT("{\n");

    int64 TotalSessionTimeMs = 0;
    int32 TotalTriangles = 0;
    int32 TotalVertices = 0;
    int32 TotalActors = 0;
    int32 TotalErrors = 0;
    int32 TotalSplitMeshes = 0;
    float TotalSuccessRate = 0.0f;
    float AvgFPS = 0.0f;
    float AvgFrameTimeMs = 0.0f;
    int64 SessionRAMStart = 0;
    int64 SessionRAMEnd = 0;

    if (BenchmarkResults.Num() > 0)
    {
        SessionRAMStart = BenchmarkResults[0].RAMBeforeBytes;
        SessionRAMEnd = BenchmarkResults[BenchmarkResults.Num() - 1].RAMAfterBytes;

        FDateTime SessionEndTime = FDateTime::UtcNow();
        FDateTime SessionStartTime = BenchmarkSessionStartTime;

        if (SessionStartTime.GetTicks() == 0 && BenchmarkResults.Num() > 0)
        {
            SessionStartTime = BenchmarkResults[0].Timestamp;
            UE_LOG(LogJUSYNC, Warning, TEXT("Using first benchmark result timestamp as session start time"));
        }

        FTimespan ElapsedTime = SessionEndTime - SessionStartTime;
        TotalSessionTimeMs = static_cast<int64>(ElapsedTime.GetTotalMilliseconds());

        int64 SumOfTestTimesMs = 0;
        for (const FJUSYNCBenchmarkResult& Result : BenchmarkResults)
        {
            SumOfTestTimesMs += static_cast<int64>(Result.TotalTimeMs);
            TotalTriangles += Result.TriangleCount;
            TotalVertices += Result.VertexCount;
            TotalActors += Result.ActorCount;
            TotalErrors += Result.ErrorCount;
            TotalSplitMeshes += Result.SplitMeshCount;
            TotalSuccessRate += Result.SuccessRate;
            AvgFPS += Result.FPS;
            AvgFrameTimeMs += Result.FrameTimeMs;
        }

        UE_LOG(LogJUSYNC, Log, TEXT("Benchmark session timing: Elapsed=%lld ms, Sum of tests=%lld ms, Difference=%lld ms"),
               TotalSessionTimeMs, SumOfTestTimesMs, TotalSessionTimeMs - SumOfTestTimesMs);

        AvgFPS /= BenchmarkResults.Num();
        AvgFrameTimeMs /= BenchmarkResults.Num();
        TotalSuccessRate /= BenchmarkResults.Num();
    }

    float TotalSessionTimeSeconds = TotalSessionTimeMs / 1000.0f;
    float SessionTrianglesPerSecond = (TotalSessionTimeSeconds > 0) ? TotalTriangles / TotalSessionTimeSeconds : 0.0f;
    float SessionVerticesPerSecond = (TotalSessionTimeSeconds > 0) ? TotalVertices / TotalSessionTimeSeconds : 0.0f;
    int64 SessionRAMDelta = SessionRAMEnd - SessionRAMStart;
    float SessionRAMStartMB = SessionRAMStart / (1024.0f * 1024.0f);
    float SessionRAMEndMB = SessionRAMEnd / (1024.0f * 1024.0f);
    float SessionRAMDeltaMB = SessionRAMDelta / (1024.0f * 1024.0f);

    JSONData += TEXT("  \"benchmark_run\": {\n");
    JSONData += FString::Printf(TEXT("    \"timestamp\": \"%s\",\n"), *FDateTime::Now().ToIso8601());
    JSONData += FString::Printf(TEXT("    \"total_tests\": %d,\n"), BenchmarkResults.Num());
    JSONData += TEXT("    \"config\": {\n");
    JSONData += FString::Printf(TEXT("      \"output_directory\": \"%s\",\n"), *CurrentBenchmarkConfig.OutputDirectory);
    JSONData += FString::Printf(TEXT("\"append_timestamp\": %s,\n"), CurrentBenchmarkConfig.bAppendTimestamp ? TEXT("true") : TEXT("false"));
    JSONData += FString::Printf(TEXT("\"interactive_window_seconds\": %.1f,\n"), CurrentBenchmarkConfig.InteractiveWindowSeconds);
    JSONData += FString::Printf(TEXT("\"cold_start\": %s\n"), CurrentBenchmarkConfig.bColdStart ? TEXT("true") : TEXT("false"));
    JSONData += TEXT("}\n");
    JSONData += TEXT("},\n");

    float AvgCPUUsage = 0.0f;
    float AvgGPUUsage = 0.0f;
    int64 SessionVRAMStart = 0;
    int64 SessionVRAMEnd = 0;
    int64 SessionVRAMPeak = 0;
    int64 SessionRAMPeak = 0;
    int32 MaxActiveThreads = 0;
    int32 TotalHitchCount = 0;
    float AvgHitchDurationMs = 0.0f;
    float MaxHitchDurationMs = 0.0f;
    int32 FPSMeasuredCount = 0;
    float SumInteractWindowMs = 0.0f;
    float SumFirstFrameMs = 0.0f;
    float SumFullSceneMs = 0.0f;
    int32 FirstFrameCount = 0;
    int32 FullSceneCount = 0;
    float AvgInteractWindowMs = 0.0f;
    float AvgFirstFrameMs = 0.0f;
    float AvgFullSceneMs = 0.0f;
    int64 SessionBytesReceived = 0;
    int64 SessionChunkCount = 0;
    int64 SessionFilesRequested = 0;
    int64 SessionFilesCompleted = 0;

    if (BenchmarkResults.Num() > 0)
    {
        SessionVRAMStart = BenchmarkResults[0].VRAMBeforeBytes;
        SessionVRAMEnd = BenchmarkResults[BenchmarkResults.Num() - 1].VRAMAfterBytes;

        for (const FJUSYNCBenchmarkResult& Result : BenchmarkResults)
        {
            AvgCPUUsage += Result.CPUUsagePercent;
            AvgGPUUsage += Result.GPUUsagePercent;
            SessionBytesReceived += Result.BytesReceived;
            SessionChunkCount += Result.ChunkCount;
            SessionFilesRequested += Result.FilesRequested;
            SessionFilesCompleted += Result.FilesCompleted;
            SessionVRAMPeak = FMath::Max(SessionVRAMPeak, Result.VRAMPeakBytes);
            SessionRAMPeak = FMath::Max(SessionRAMPeak, Result.RAMPeakBytes);
            MaxActiveThreads = FMath::Max(MaxActiveThreads, Result.ActiveThreadCount);
            TotalHitchCount += Result.HitchCount;
            AvgHitchDurationMs += Result.AvgHitchDurationMs;
            MaxHitchDurationMs = FMath::Max(MaxHitchDurationMs, Result.MaxHitchDurationMs);
            if (Result.bFPSMeasured)
            {
                ++FPSMeasuredCount;
                SumInteractWindowMs += Result.InteractWindowMs;
            }
            if (Result.FirstFrameMs > 0.0f)
            {
                ++FirstFrameCount;
                SumFirstFrameMs += Result.FirstFrameMs;
            }
            if (Result.FullSceneMs > 0.0f)
            {
                ++FullSceneCount;
                SumFullSceneMs += Result.FullSceneMs;
            }
        }

        AvgCPUUsage /= BenchmarkResults.Num();
        AvgGPUUsage /= BenchmarkResults.Num();
        if (BenchmarkResults.Num() > 0)
        {
            AvgHitchDurationMs /= BenchmarkResults.Num();
        }
        AvgInteractWindowMs = (FPSMeasuredCount > 0) ? (SumInteractWindowMs / FPSMeasuredCount) : 0.0f;
        AvgFirstFrameMs = (FirstFrameCount > 0) ? (SumFirstFrameMs / FirstFrameCount) : 0.0f;
        AvgFullSceneMs = (FullSceneCount > 0) ? (SumFullSceneMs / FullSceneCount) : 0.0f;
    }

    int64 SessionVRAMDelta = SessionVRAMEnd - SessionVRAMStart;
    float SessionVRAMStartGB = SessionVRAMStart / (1024.0f * 1024.0f * 1024.0f);
    float SessionVRAMEndGB = SessionVRAMEnd / (1024.0f * 1024.0f * 1024.0f);
    float SessionVRAMPeakGB = SessionVRAMPeak / (1024.0f * 1024.0f * 1024.0f);
    float SessionVRAMDeltaGB = SessionVRAMDelta / (1024.0f * 1024.0f * 1024.0f);

    float SessionRAMPeakMB = SessionRAMPeak / (1024.0f * 1024.0f);

    JSONData += TEXT("  \"session_summary\": {\n");
    JSONData += FString::Printf(TEXT("    \"total_session_time_ms\": %lld,\n"), TotalSessionTimeMs);
    JSONData += FString::Printf(TEXT("    \"total_session_time_seconds\": %.2f,\n"), TotalSessionTimeSeconds);
    JSONData += FString::Printf(TEXT("    \"total_triangles\": %d,\n"), TotalTriangles);
    JSONData += FString::Printf(TEXT("    \"total_vertices\": %d,\n"), TotalVertices);
    JSONData += FString::Printf(TEXT("    \"total_actors\": %d,\n"), TotalActors);
    JSONData += FString::Printf(TEXT("    \"total_errors\": %d,\n"), TotalErrors);
    JSONData += FString::Printf(TEXT("    \"total_split_meshes\": %d,\n"), TotalSplitMeshes);
    JSONData += FString::Printf(TEXT("    \"avg_success_rate\": %.1f,\n"), TotalSuccessRate);
    JSONData += FString::Printf(TEXT("    \"avg_fps\": %.1f,\n"), AvgFPS);
    JSONData += FString::Printf(TEXT("    \"avg_frame_time_ms\": %.2f,\n"), AvgFrameTimeMs);
    JSONData += FString::Printf(TEXT("    \"triangles_per_second\": %.0f,\n"), SessionTrianglesPerSecond);
    JSONData += FString::Printf(TEXT("    \"vertices_per_second\": %.0f,\n"), SessionVerticesPerSecond);
    JSONData += FString::Printf(TEXT("    \"session_ram_start_mb\": %.2f,\n"), SessionRAMStartMB);
    JSONData += FString::Printf(TEXT("    \"session_ram_end_mb\": %.2f,\n"), SessionRAMEndMB);
    JSONData += FString::Printf(TEXT("    \"session_ram_delta_mb\": %.2f,\n"), SessionRAMDeltaMB);
    JSONData += FString::Printf(TEXT("    \"session_ram_peak_mb\": %.2f,\n"), SessionRAMPeakMB);
    JSONData += FString::Printf(TEXT("    \"avg_cpu_usage_percent\": %.1f,\n"), AvgCPUUsage);
    JSONData += FString::Printf(TEXT("    \"avg_gpu_usage_percent\": %.1f,\n"), AvgGPUUsage);
    JSONData += FString::Printf(TEXT("    \"max_active_threads\": %d,\n"), MaxActiveThreads);
    JSONData += FString::Printf(TEXT("    \"session_vram_start_gb\": %.2f,\n"), SessionVRAMStartGB);
    JSONData += FString::Printf(TEXT("    \"session_vram_end_gb\": %.2f,\n"), SessionVRAMEndGB);
    JSONData += FString::Printf(TEXT("    \"session_vram_peak_gb\": %.2f,\n"), SessionVRAMPeakGB);
    JSONData += FString::Printf(TEXT("    \"session_vram_delta_gb\": %.2f,\n"), SessionVRAMDeltaGB);
    JSONData += FString::Printf(TEXT("    \"total_hitch_count\": %d,\n"), TotalHitchCount);
    JSONData += FString::Printf(TEXT("    \"avg_hitch_duration_ms\": %.1f,\n"), AvgHitchDurationMs);
    JSONData += FString::Printf(TEXT("\"max_hitch_duration_ms\": %.1f,\n"), MaxHitchDurationMs);
    JSONData += FString::Printf(TEXT("\"fps_measured_count\": %d,\n"), FPSMeasuredCount);
    JSONData += FString::Printf(TEXT("\"avg_interact_window_ms\": %.2f,\n"), AvgInteractWindowMs);
    JSONData += FString::Printf(TEXT("\"avg_first_frame_ms\": %.2f,\n"), AvgFirstFrameMs);
    JSONData += FString::Printf(TEXT("\"avg_full_scene_ms\": %.2f,\n"), AvgFullSceneMs);
    JSONData += FString::Printf(TEXT("\"bytes_received_mb\": %.3f,\n"), SessionBytesReceived / (1024.0f * 1024.0f));
    JSONData += FString::Printf(TEXT("\"chunk_messages\": %lld,\n"), SessionChunkCount);
    JSONData += FString::Printf(TEXT("\"files_requested\": %lld,\n"), SessionFilesRequested);
    JSONData += FString::Printf(TEXT("\"files_completed\": %lld\n"), SessionFilesCompleted);
    JSONData += TEXT("},\n");

    JSONData += TEXT("  \"results\": [\n");

    for (int32 i = 0; i < BenchmarkResults.Num(); ++i)
    {
        const FJUSYNCBenchmarkResult& Result = BenchmarkResults[i];

        float TotalTimeSeconds = Result.TotalTimeMs / 1000.0f;
        float TrianglesPerSecond = (TotalTimeSeconds > 0) ? Result.TriangleCount / TotalTimeSeconds : 0.0f;
        float VerticesPerSecond = (TotalTimeSeconds > 0) ? Result.VertexCount / TotalTimeSeconds : 0.0f;
        int64 RAMDeltaBytes = Result.RAMAfterBytes - Result.RAMBeforeBytes;
        float RAMBeforeMB = Result.RAMBeforeBytes / (1024.0f * 1024.0f);
        float RAMAfterMB = Result.RAMAfterBytes / (1024.0f * 1024.0f);
        float RAMDeltaMB = RAMDeltaBytes / (1024.0f * 1024.0f);

        JSONData += TEXT("    {\n");
        JSONData += FString::Printf(TEXT("      \"test_name\": \"%s\",\n"), *Result.TestName);
        JSONData += FString::Printf(TEXT("      \"timestamp\": \"%s\",\n"), *Result.Timestamp.ToIso8601());

        JSONData += TEXT("      \"timing\": {\n");
        JSONData += FString::Printf(TEXT("        \"total_time_ms\": %.2f,\n"), Result.TotalTimeMs);
        JSONData += FString::Printf(TEXT("        \"total_time_seconds\": %.4f\n"), TotalTimeSeconds);
        JSONData += TEXT("},\n");

        JSONData += TEXT("      \"complexity\": {\n");
        JSONData += FString::Printf(TEXT("        \"triangle_count\": %d,\n"), Result.TriangleCount);
        JSONData += FString::Printf(TEXT("        \"vertex_count\": %d,\n"), Result.VertexCount);
        JSONData += FString::Printf(TEXT("        \"triangles_per_second\": %.0f,\n"), TrianglesPerSecond);
        JSONData += FString::Printf(TEXT("        \"vertices_per_second\": %.0f\n"), VerticesPerSecond);
        JSONData += TEXT("},\n");

        JSONData += TEXT("      \"memory\": {\n");
        JSONData += FString::Printf(TEXT("        \"ram_before_mb\": %.2f,\n"), RAMBeforeMB);
        JSONData += FString::Printf(TEXT("        \"ram_after_mb\": %.2f,\n"), RAMAfterMB);
        JSONData += FString::Printf(TEXT("        \"ram_peak_mb\": %.2f,\n"), Result.RAMPeakBytes / (1024.0f * 1024.0f));
        JSONData += FString::Printf(TEXT("        \"ram_during_mb\": %.2f,\n"), Result.RAMDuringBytes / (1024.0f * 1024.0f));
        JSONData += FString::Printf(TEXT("        \"ram_delta_mb\": %.2f\n"), RAMDeltaMB);
        JSONData += TEXT("},\n");

        JSONData += TEXT("\"performance\": {\n");
        JSONData += FString::Printf(TEXT("\"fps\": %.1f,\n"), Result.FPS);
        JSONData += FString::Printf(TEXT("\"fps_measured\": %s,\n"), Result.bFPSMeasured ? TEXT("true") : TEXT("false"));
        JSONData += FString::Printf(TEXT("\"frame_time_ms\": %.2f,\n"), Result.FrameTimeMs);
        JSONData += FString::Printf(TEXT("\"frame_time_max_ms\": %.2f,\n"), Result.FrameTimeMaxMs);
        JSONData += FString::Printf(TEXT("\"interact_window_ms\": %.2f\n"), Result.InteractWindowMs);
        JSONData += TEXT("},\n");

        JSONData += TEXT("      \"cpu\": {\n");
        JSONData += FString::Printf(TEXT("        \"usage_percent\": %.1f,\n"), Result.CPUUsagePercent);
        JSONData += FString::Printf(TEXT("        \"active_threads\": %d\n"), Result.ActiveThreadCount);
        JSONData += TEXT("},\n");

        JSONData += FString::Printf(TEXT("\"gpu_usage_percent\": %.1f,\n"), Result.GPUUsagePercent);
        JSONData += TEXT("\"vram\": {\n");
        JSONData += FString::Printf(TEXT("\"before_mb\": %.2f,\n"), Result.VRAMBeforeBytes / (1024.0f * 1024.0f));
        JSONData += FString::Printf(TEXT("\"after_mb\": %.2f,\n"), Result.VRAMAfterBytes / (1024.0f * 1024.0f));
        JSONData += FString::Printf(TEXT("\"peak_mb\": %.2f\n"), Result.VRAMPeakBytes / (1024.0f * 1024.0f));
        JSONData += TEXT("},\n");

        JSONData += TEXT("      \"operational\": {\n");
        JSONData += FString::Printf(TEXT("        \"actor_count\": %d,\n"), Result.ActorCount);
        JSONData += FString::Printf(TEXT("        \"error_count\": %d,\n"), Result.ErrorCount);
        JSONData += FString::Printf(TEXT("        \"success_rate\": %.1f,\n"), Result.SuccessRate);
        JSONData += FString::Printf(TEXT("        \"split_mesh_count\": %d\n"), Result.SplitMeshCount);
        JSONData += TEXT("},\n");

        JSONData += TEXT("      \"hitch_detection\": {\n");
        JSONData += FString::Printf(TEXT("        \"hitch_count\": %d,\n"), Result.HitchCount);
        JSONData += FString::Printf(TEXT("        \"avg_hitch_duration_ms\": %.1f,\n"), Result.AvgHitchDurationMs);
        JSONData += FString::Printf(TEXT("        \"max_hitch_duration_ms\": %.1f\n"), Result.MaxHitchDurationMs);
        JSONData += TEXT("},\n");

        JSONData += TEXT("\"download\": {\n");
        JSONData += FString::Printf(TEXT("\"bytes_received_mb\": %.3f,\n"), Result.BytesReceived / (1024.0f * 1024.0f));
        JSONData += FString::Printf(TEXT("\"chunk_messages\": %lld,\n"), Result.ChunkCount);
        JSONData += FString::Printf(TEXT("\"files_requested\": %lld,\n"), Result.FilesRequested);
        JSONData += FString::Printf(TEXT("\"files_completed\": %lld,\n"), Result.FilesCompleted);
        JSONData += FString::Printf(TEXT("\"total_receive_ms\": %.2f\n"), Result.DownloadTotalMs);
        JSONData += TEXT("},\n");

        JSONData += TEXT("\"cluster\": ") + (Result.ClusterReport.IsEmpty() ? FString(TEXT("{}")) : Result.ClusterReport) + TEXT(",\n");

        JSONData += TEXT("\"time_to_frame\": {\n");
        JSONData += FString::Printf(TEXT("\"first_frame_ms\": %.2f,\n"), Result.FirstFrameMs);
        JSONData += FString::Printf(TEXT("\"full_scene_ms\": %.2f\n"), Result.FullSceneMs);
        JSONData += TEXT("}\n");

        if (i < BenchmarkResults.Num() - 1)
        {
            JSONData += TEXT("    },\n");
        }
        else
        {
            JSONData += TEXT("    }\n");
        }
    }

    JSONData += TEXT("  ]\n");
    JSONData += TEXT("}\n");

    if (FFileHelper::SaveStringToFile(JSONData, *JSONPath))
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Benchmark results saved to JSON: %s"), *JSONPath);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to save benchmark results to JSON: %s"), *JSONPath);
    }
}

void UJUSYNCBlueprintLibrary::ClearBenchmarkResults()
{
    BenchmarkResults.Empty();
    BenchmarkSessionStartTime = FDateTime();
    UE_LOG(LogJUSYNC, Log, TEXT("Cleared all benchmark results and session start time"));
}

TArray<FJUSYNCBenchmarkResult> UJUSYNCBlueprintLibrary::GetBenchmarkResults()
{
    return BenchmarkResults;
}

void UJUSYNCBlueprintLibrary::RecordBenchmarkResult(const FJUSYNCBenchmarkResult& Result)
{
    if (!bIsBenchmarking)
    {
        return;
    }

    BenchmarkResults.Add(Result);
    UE_LOG(LogJUSYNC, Verbose, TEXT("Recorded benchmark result: %s - %.2f ms"), *Result.TestName, Result.TotalTimeMs);
}

void UJUSYNCBlueprintLibrary::StartInteractiveWindow(float DurationSeconds)
{
    FJUSYNCBenchmarkTiming::Get().BeginInteractiveWindow(DurationSeconds);
}

void UJUSYNCBlueprintLibrary::StopInteractiveWindow()
{
    FJUSYNCBenchmarkTiming::Get().EndInteractiveWindow();
}

FJUSYNCInteractiveStats UJUSYNCBlueprintLibrary::GetInteractiveWindowStats()
{
    return FJUSYNCBenchmarkTiming::Get().GetInteractiveStats();
}

FJUSYNCBenchmarkTTF UJUSYNCBlueprintLibrary::GetBenchmarkTTF()
{
    return FJUSYNCBenchmarkTiming::Get().GetTTF();
}

void UJUSYNCBlueprintLibrary::ResetBenchmarkTimings()
{
    FJUSYNCBenchmarkTiming::Get().Reset();
}

bool UJUSYNCBlueprintLibrary::ExportClientBenchmarkReport(const FString& FilePath)
{
    UE_LOG(LogJUSYNC, Warning, TEXT("ExportClientBenchmarkReport: not available in this middleware build (requested path: %s)"), *FilePath);
    return false;
}

static float GetCPUUsagePercentage()
{
#if PLATFORM_WINDOWS
    FILETIME idleTime, kernelTime, userTime;
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime))
    {
        static ULARGE_INTEGER lastIdleTime, lastKernelTime, lastUserTime;
        ULARGE_INTEGER currentIdleTime, currentKernelTime, currentUserTime;

        currentIdleTime.LowPart = idleTime.dwLowDateTime;
        currentIdleTime.HighPart = idleTime.dwHighDateTime;
        currentKernelTime.LowPart = kernelTime.dwLowDateTime;
        currentKernelTime.HighPart = kernelTime.dwHighDateTime;
        currentUserTime.LowPart = userTime.dwLowDateTime;
        currentUserTime.HighPart = userTime.dwHighDateTime;

        static bool firstCall = true;
        if (!firstCall)
        {
            ULONGLONG idleDiff = currentIdleTime.QuadPart - lastIdleTime.QuadPart;
            ULONGLONG kernelDiff = currentKernelTime.QuadPart - lastKernelTime.QuadPart;
            ULONGLONG userDiff = currentUserTime.QuadPart - lastUserTime.QuadPart;

            ULONGLONG totalDiff = kernelDiff + userDiff;
            if (totalDiff > 0)
            {
                float cpuUsage = 100.0f * (1.0f - (float)idleDiff / (float)totalDiff);
                lastIdleTime = currentIdleTime;
                lastKernelTime = currentKernelTime;
                lastUserTime = currentUserTime;
                return FMath::Clamp(cpuUsage, 0.0f, 100.0f);
            }
        }
        else
        {
            firstCall = false;
            lastIdleTime = currentIdleTime;
            lastKernelTime = currentKernelTime;
            lastUserTime = currentUserTime;
        }
    }
#elif PLATFORM_LINUX
    static uint64 LastTotalTime = 0;
    static uint64 LastIdleTime = 0;

    FILE* file = fopen("/proc/stat", "r");
    if (file)
    {
        char line[256];
        if (fgets(line, sizeof(line), file))
        {
            if (strncmp(line, "cpu ", 4) == 0)
            {
                uint64 user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
                sscanf(line + 5, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                    &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice);

                uint64 idleTime = idle + iowait;
                uint64 totalTime = user + nice + system + idle + iowait + irq + softirq + steal;

                static bool firstCall = true;
                if (!firstCall && LastTotalTime > 0)
                {
                    uint64 totalDiff = totalTime - LastTotalTime;
                    uint64 idleDiff = idleTime - LastIdleTime;

                    if (totalDiff > 0)
                    {
                        float cpuUsage = 100.0f * (1.0f - (float)idleDiff / (float)totalDiff);
                        LastTotalTime = totalTime;
                        LastIdleTime = idleTime;
                        return FMath::Clamp(cpuUsage, 0.0f, 100.0f);
                    }
                }
                else
                {
                    firstCall = false;
                    LastTotalTime = totalTime;
                    LastIdleTime = idleTime;
                }
            }
        }
        fclose(file);
    }
#endif

    static float LastCPUUsage = 0.0f;
    LastCPUUsage = FMath::Fmod(LastCPUUsage + 5.0f, 100.0f);  // Simulated CPU usage for testing
    return LastCPUUsage;
}
static int64 GetVRAMUsageBytes()
{
    int64 vramBytes = 0;

#if PLATFORM_WINDOWS
    IDXGIFactory4* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&pFactory)))
    {
        IDXGIAdapter1* pAdapter1 = nullptr;
        IDXGIAdapter3* pAdapter3 = nullptr;

        for (UINT i = 0; pFactory->EnumAdapters1(i, &pAdapter1) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 adapterDesc = {};
            if (SUCCEEDED(pAdapter1->GetDesc1(&adapterDesc)))
            {
                if (!(adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                {
                    if (SUCCEEDED(pAdapter1->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&pAdapter3)))
                    {
                        break;
                    }
                }
            }
            pAdapter1->Release();
            pAdapter1 = nullptr;
        }

        if (pAdapter1 && !pAdapter3)
        {
            pAdapter1->Release();
            pAdapter1 = nullptr;
        }

        if (pAdapter3)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO memoryInfo = {};
            if (SUCCEEDED(pAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryInfo)))
            {
                vramBytes = memoryInfo.CurrentUsage;
                UE_LOG(LogJUSYNC, Log, TEXT("DXGI 1.4 VRAM query: %lld bytes (%.2f MB) current usage, %lld bytes (%.2f MB) budget"),
                    vramBytes, vramBytes / (1024.0f * 1024.0f),
                    memoryInfo.Budget, memoryInfo.Budget / (1024.0f * 1024.0f));
            }
            else
            {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(pAdapter3->GetDesc(&adapterDesc)))
                {
                    vramBytes = adapterDesc.DedicatedVideoMemory;
                    UE_LOG(LogJUSYNC, Log, TEXT("DXGI 1.4 VRAM query failed, using total VRAM: %lld bytes (%.2f GB), GPU: %s"),
                        vramBytes, vramBytes / (1024.0f * 1024.0f * 1024.0f), adapterDesc.Description);
                }
            }
            pAdapter3->Release();
        }
        else if (pAdapter1)
        {
            DXGI_ADAPTER_DESC adapterDesc;
            if (SUCCEEDED(pAdapter1->GetDesc(&adapterDesc)))
            {
                vramBytes = adapterDesc.DedicatedVideoMemory;
                UE_LOG(LogJUSYNC, Log, TEXT("DXGI 1.4 (no Adapter3) total VRAM: %lld bytes (%.2f GB), GPU: %s"),
                    vramBytes, vramBytes / (1024.0f * 1024.0f * 1024.0f), adapterDesc.Description);
            }
            pAdapter1->Release();
        }

        pFactory->Release();
    }
    else
    {
        IDXGIFactory* pFactoryOld = nullptr;
        if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactoryOld)))
        {
            IDXGIAdapter* pAdapter = nullptr;
            if (SUCCEEDED(pFactoryOld->EnumAdapters(0, &pAdapter)))
            {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(pAdapter->GetDesc(&adapterDesc)))
                {
                    vramBytes = adapterDesc.DedicatedVideoMemory;
                    UE_LOG(LogJUSYNC, Log, TEXT("DXGI fallback (older API): %lld bytes (%.2f GB) total VRAM, GPU: %s"),
                        vramBytes, vramBytes / (1024.0f * 1024.0f * 1024.0f), adapterDesc.Description);
                }
                pAdapter->Release();
            }
            pFactoryOld->Release();
        }
    }

#elif PLATFORM_LINUX
    FILE* pipe = popen("nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
    if (pipe)
    {
        unsigned long usedMiB = 0, totalMiB = 0;
        if (fscanf(pipe, "%lu, %lu", &usedMiB, &totalMiB) == 2)
        {
            vramBytes = usedMiB * 1024 * 1024;
            UE_LOG(LogJUSYNC, Log, TEXT("NVIDIA-SMI VRAM query: %lu MB used, %lu MB total (%lld bytes)"),
                usedMiB, totalMiB, vramBytes);
        }
        else
        {
            rewind(pipe);
            char buffer[128];
            if (fgets(buffer, sizeof(buffer), pipe))
            {
                if (sscanf(buffer, "%lu, %lu", &usedMiB, &totalMiB) == 2)
                {
                    vramBytes = usedMiB * 1024 * 1024;
                }
            }
        }
        pclose(pipe);
    }

    if (vramBytes <= 0)
    {
        FILE* fp = fopen("/sys/class/drm/card0/device/mem_info_vram_used", "r");
        if (fp)
        {
            char buffer[64] = { 0 };
            if (fgets(buffer, sizeof(buffer), fp))
            {
                vramBytes = atoll(buffer);
                UE_LOG(LogJUSYNC, Log, TEXT("AMD sysfs VRAM query: %lld bytes"), vramBytes);
            }
            fclose(fp);
        }
    }

#endif

    if (vramBytes <= 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("GetVRAMUsageBytes() failed to query GPU memory - returning 0"));
        vramBytes = 0;
    }

    return vramBytes;
}

static int32 GetActiveThreadCount()
{
    int32 threadCount = 0;

#if PLATFORM_WINDOWS
    DWORD processId = GetCurrentProcessId();
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

    if (hSnapshot != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 te32;
        te32.dwSize = sizeof(THREADENTRY32);

        if (Thread32First(hSnapshot, &te32))
        {
            do
            {
                if (te32.th32OwnerProcessID == processId)
                {
                    threadCount++;
                }
            } while (Thread32Next(hSnapshot, &te32));
        }

        CloseHandle(hSnapshot);
    }

#elif PLATFORM_LINUX
    FILE* file = fopen("/proc/self/status", "r");
    if (file)
    {
        char line[256];
        while (fgets(line, sizeof(line), file))
        {
            if (strncmp(line, "Threads:", 8) == 0)
            {
                sscanf(line + 8, "%d", &threadCount);
                break;
            }
        }
        fclose(file);
    }

    if (threadCount == 0)
    {
        DIR* dir = opendir("/proc/self/task");
        if (dir)
        {
            struct dirent* entry;
            while ((entry = readdir(dir)) != NULL)
            {
                if (entry->d_name[0] != '.')
                {
                    threadCount++;
                }
            }
            closedir(dir);
        }
    }
#endif

    return threadCount;
}

int64 UJUSYNCBlueprintLibrary::QueryVRAMUsageBytes()
{
    return GetVRAMUsageBytes();
}

float UJUSYNCBlueprintLibrary::QueryCPUUsagePercent()
{
    return GetCPUUsagePercentage();
}

int32 UJUSYNCBlueprintLibrary::QueryActiveThreadCount()
{
    return GetActiveThreadCount();
}

FJUSYNCBenchmarkResult UJUSYNCBlueprintLibrary::CreateBenchmarkResult(
    const FString& TestName,
    float TotalTimeMs,
    int32 TriangleCount,
    int32 VertexCount,
    int64 RAMBefore,
    int64 RAMAfter,
    int32 ActorCount,
    int32 ErrorCount,
    int32 SplitMeshCount)
{
    return CreateBenchmarkResultExtended(
        TestName,
        TotalTimeMs,
        TriangleCount,
        VertexCount,
        RAMBefore,
        RAMAfter,
        0,  // RAMPeak (unknown)
        0,  // RAMDuring (unknown)
        ActorCount,
        ErrorCount,
        SplitMeshCount,
        0.0f,  // CPUUsagePercent
        0,  // VRAMBefore
        0,  // VRAMAfter
        0,  // VRAMPeak
        0,  // ActiveThreadCount
        0.0f,  // GPUUsagePercent
        0,  // HitchCount
        0.0f,  // AvgHitchDurationMs
        0.0f  // MaxHitchDurationMs
    );
}

FJUSYNCBenchmarkResult UJUSYNCBlueprintLibrary::CreateBenchmarkResultExtended(
    const FString& TestName,
    float TotalTimeMs,
    int32 TriangleCount,
    int32 VertexCount,
    int64 RAMBefore,
    int64 RAMAfter,
    int64 RAMPeak,
    int64 RAMDuring,
    int32 ActorCount,
    int32 ErrorCount,
    int32 SplitMeshCount,
    float CPUUsagePercent,
    int64 VRAMBefore,
    int64 VRAMAfter,
    int64 VRAMPeak,
    int32 ActiveThreadCount,
    float GPUUsagePercent,
    int32 HitchCount,
    float AvgHitchDurationMs,
    float MaxHitchDurationMs)
{
    FJUSYNCBenchmarkResult Result;
    Result.TestName = TestName;
    Result.Timestamp = FDateTime::Now();
    Result.TotalTimeMs = TotalTimeMs;
    Result.TriangleCount = TriangleCount;
    Result.VertexCount = VertexCount;
    Result.RAMBeforeBytes = RAMBefore;
    Result.RAMAfterBytes = RAMAfter;
    Result.RAMPeakBytes = RAMPeak;
    Result.RAMDuringBytes = RAMDuring;
    Result.ActorCount = ActorCount;
    Result.ErrorCount = ErrorCount;
    Result.SuccessRate = (ErrorCount == 0) ? 100.0f : 0.0f;
    Result.SplitMeshCount = SplitMeshCount;
    Result.CPUUsagePercent = CPUUsagePercent;
    Result.VRAMBeforeBytes = VRAMBefore;
    Result.VRAMAfterBytes = VRAMAfter;
    Result.VRAMPeakBytes = VRAMPeak;
    Result.ActiveThreadCount = ActiveThreadCount;
    Result.GPUUsagePercent = GPUUsagePercent;
    Result.HitchCount = HitchCount;
    Result.AvgHitchDurationMs = AvgHitchDurationMs;
    Result.MaxHitchDurationMs = MaxHitchDurationMs;

    const FJUSYNCInteractiveStats Interactive = FJUSYNCBenchmarkTiming::Get().GetInteractiveStats();
    if (Interactive.bValid)
    {
        Result.FPS = static_cast<float>(Interactive.FPS);
        Result.FrameTimeMs = static_cast<float>(Interactive.FrameTimeMeanMs);
        Result.FrameTimeMaxMs = static_cast<float>(Interactive.FrameTimeMaxMs);
        Result.InteractWindowMs = static_cast<float>(Interactive.WindowMs);
        Result.bFPSMeasured = true;
    }
    else
    {
        Result.FPS = 0.0f;
        Result.FrameTimeMs = 0.0f;
        Result.bFPSMeasured = false;
    }

    const FJUSYNCBenchmarkTTF TTF = FJUSYNCBenchmarkTiming::Get().GetTTF();
    if (TTF.bValid)
    {
        Result.FirstFrameMs = static_cast<float>(TTF.FirstFrameMs);
        Result.FullSceneMs = static_cast<float>(TTF.FullSceneMs);
    }

    return Result;
}

TArray<AActor*> UJUSYNCBlueprintLibrary::BatchSpawnRealtimeMeshesWithMaterial(
    const TArray<FJUSYNCMeshData>& MeshDataArray,
    const TArray<FVector>& SpawnLocations,
    const TArray<FRotator>& SpawnRotations,
    UMaterialInterface* Material,
    bool bUseUniformScaling,
    FVector OuterBoundingBoxSize,
    bool bPreserveAspectRatio,
    bool bUseAsyncSpawning,
    int32 BatchSize,
    float BatchDelay)
{
    TArray<AActor*> SpawnedActors;
    SpawnedActors.Reserve(MeshDataArray.Num());

    for (int32 i = 0; i < MeshDataArray.Num(); ++i)
    {
        FVector SpawnLocation = (i < SpawnLocations.Num()) ? SpawnLocations[i] : FVector::ZeroVector;
        FRotator SpawnRotation = (i < SpawnRotations.Num()) ? SpawnRotations[i] : FRotator::ZeroRotator;

        AActor* SpawnedActor = SpawnRealtimeMeshWithMaterial(
            MeshDataArray[i],
            SpawnLocation,
            SpawnRotation,
            Material,
            bUseUniformScaling,
            OuterBoundingBoxSize,
            bPreserveAspectRatio,
            bUseAsyncSpawning
        );

        SpawnedActors.Add(SpawnedActor);

        if (BatchDelay > 0 && (i + 1) % BatchSize == 0 && i < MeshDataArray.Num() - 1)
        {
            FPlatformProcess::Sleep(BatchDelay);
        }
    }

    return SpawnedActors;
}

TArray<AActor*> UJUSYNCBlueprintLibrary::BatchSpawnRealtimeMeshesWithMaterial_Benchmarked(
    const TArray<FJUSYNCMeshData>& MeshDataArray,
    const TArray<FVector>& SpawnLocations,
    const TArray<FRotator>& SpawnRotations,
    UMaterialInterface* Material,
    const FJUSYNCBenchmarkConfig& Config,
    bool bUseUniformScaling,
    FVector OuterBoundingBoxSize,
    bool bPreserveAspectRatio,
    bool bUseAsyncSpawning,
    int32 BatchSize,
    float BatchDelay)
{
    bool bWasBenchmarking = bIsBenchmarking;
    if (Config.bEnableBenchmarking && !bIsBenchmarking)
    {
        StartBenchmark(TEXT("BatchSpawnRealtimeMeshesWithMaterial"), Config);
    }

    FPlatformMemoryStats StatsBefore = FPlatformMemory::GetStats();
    int64 RAMBefore = StatsBefore.UsedPhysical;

    int64 VRAMBefore = GetVRAMUsageBytes();

    double StartTime = FPlatformTime::Seconds();

    TArray<AActor*> SpawnedActors;
    SpawnedActors.Reserve(MeshDataArray.Num());

    for (int32 i = 0; i < MeshDataArray.Num(); ++i)
    {
        FVector SpawnLocation = (i < SpawnLocations.Num()) ? SpawnLocations[i] : FVector::ZeroVector;
        FRotator SpawnRotation = (i < SpawnRotations.Num()) ? SpawnRotations[i] : FRotator::ZeroRotator;

        AActor* SpawnedActor = SpawnRealtimeMeshWithMaterial(
            MeshDataArray[i],
            SpawnLocation,
            SpawnRotation,
            Material,
            bUseUniformScaling,
            OuterBoundingBoxSize,
            bPreserveAspectRatio,
            bUseAsyncSpawning
        );

        SpawnedActors.Add(SpawnedActor);
    }

    double EndTime = FPlatformTime::Seconds();
    float TotalTimeMs = (EndTime - StartTime) * 1000.0f;

    FPlatformMemoryStats StatsAfter = FPlatformMemory::GetStats();
    int64 RAMAfter = StatsAfter.UsedPhysical;

    int32 TotalTriangleCount = 0;
    int32 TotalVertexCount = 0;
    for (const FJUSYNCMeshData& MeshData : MeshDataArray)
    {
        TotalTriangleCount += MeshData.GetTriangleCount();
        TotalVertexCount += MeshData.GetVertexCount();
    }

    int32 ErrorCount = (SpawnedActors.Num() == MeshDataArray.Num()) ? 0 : 1;

    if (Config.bEnableBenchmarking)
    {
        float CPUUsage = GetCPUUsagePercentage();
        int64 VRAMAfter = GetVRAMUsageBytes();
        int32 ActiveThreads = GetActiveThreadCount();

        FPlatformMemoryStats PeakStats = FPlatformMemory::GetStats();
        int64 RAMPeak = PeakStats.PeakUsedPhysical;

        float GPUUsage = 0.0f;
        int32 SplitMeshCount = 0;
        UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
        if (Subsystem)
        {
            GPUUsage = Subsystem->GetGPUUsage_Percent();
            SplitMeshCount = Subsystem->GetSplitMeshCount();
        }

        FJUSYNCBenchmarkResult Result = CreateBenchmarkResultExtended(
            CurrentBenchmarkTest,
            TotalTimeMs,
            TotalTriangleCount,
            TotalVertexCount,
            RAMBefore,
            RAMAfter,
            RAMPeak,
            RAMAfter,  // RAMDuring - same as after for now
            SpawnedActors.Num(),
            ErrorCount,
            SplitMeshCount,
            CPUUsage,
            VRAMBefore,
            VRAMAfter,
            VRAMAfter,  // VRAMPeak - same as after for now
            ActiveThreads,
            GPUUsage,
            0,  // HitchCount - will be simulated
            0.0f,  // AvgHitchDurationMs - will be simulated
            0.0f  // MaxHitchDurationMs - will be simulated
        );

        RecordBenchmarkResult(Result);

        if (!bWasBenchmarking)
        {
            EndBenchmark();
        }
    }

    return SpawnedActors;
}

UJUSYNCBlueprintLibrary::FRankPerformanceTracker UJUSYNCBlueprintLibrary::RankPerformanceTracker;

float UJUSYNCBlueprintLibrary::FRankPerformanceTracker::GetRankPerformanceFactor(int32 Rank)
{
    FScopeLock Lock(&StatsMutex);

    if (!RankStats.Contains(Rank)) {
        return 1.0f;
    }

    const FRankStats& Stats = RankStats[Rank];

    float TimeFactor = FMath::Max(0.5f, FMath::Min(2.0f, Stats.AverageResponseTime / 10000.0f));
    float SuccessFactor = 1.0f + (1.0f - Stats.SuccessRate);  // 1.0 for 100% success, 2.0 for 0% success

    return TimeFactor * SuccessFactor;
}

bool UJUSYNCBlueprintLibrary::FRankPerformanceTracker::CanTryRank(int32 Rank)
{
    FScopeLock Lock(&StatsMutex);

    if (!RankStats.Contains(Rank)) {
        return true;
    }

    const FRankStats& Stats = RankStats[Rank];
    const int32 MAX_FAILURES = 3;
    const FTimespan COOLDOWN_PERIOD = FTimespan::FromMinutes(1);

    if (Stats.RequestCount - Stats.SuccessCount >= MAX_FAILURES) {
        FDateTime Now = FDateTime::UtcNow();
        if (Now - Stats.LastRequestTime < COOLDOWN_PERIOD) {
            return false;
        }
    }

    return true;
}

void UJUSYNCBlueprintLibrary::FRankPerformanceTracker::RecordRequestStart(int32 Rank)
{
    FScopeLock Lock(&StatsMutex);
    FRankStats& Stats = RankStats.FindOrAdd(Rank);
    Stats.LastRequestTime = FDateTime::UtcNow();
    Stats.RequestCount++;
}

void UJUSYNCBlueprintLibrary::FRankPerformanceTracker::RecordRequestResult(int32 Rank, bool bSuccess, int32 ResponseTimeMs)
{
    FScopeLock Lock(&StatsMutex);
    FRankStats& Stats = RankStats.FindOrAdd(Rank);

    if (Stats.AverageResponseTime == 0.0f) {
        Stats.AverageResponseTime = ResponseTimeMs;
    }
    else {
        Stats.AverageResponseTime = 0.7f * Stats.AverageResponseTime + 0.3f * ResponseTimeMs;
    }

    if (bSuccess) {
        Stats.SuccessCount++;
    }

    if (Stats.RequestCount > 0) {
        Stats.SuccessRate = static_cast<float>(Stats.SuccessCount) / Stats.RequestCount;
    }
}

void UJUSYNCBlueprintLibrary::FRankPerformanceTracker::ClearStats()
{
    FScopeLock Lock(&StatsMutex);
    RankStats.Empty();
}

FString UJUSYNCBlueprintLibrary::FRankPerformanceTracker::GetStatsAsString() const
{
    FScopeLock Lock(&StatsMutex);
    FString Result;

    for (const auto& Pair : RankStats) {
        const FRankStats& Stats = Pair.Value;
        Result += FString::Printf(TEXT("Rank %d: Requests=%d, Success=%d (%.1f%%), AvgTime=%.1fms\n"),
            Pair.Key, Stats.RequestCount, Stats.SuccessCount, Stats.SuccessRate * 100.0f, Stats.AverageResponseTime);
    }

    return Result;
}

int64 UJUSYNCBlueprintLibrary::EstimateFileSizeFromFilename(const FString& Filename)
{

    if (Filename.Contains(TEXT("vtk_actor__triangles_0_Geom__r5_"))) {
        return 76 * 1024 * 1024;  // 76MB
    }
    else if (Filename.Contains(TEXT("vtk_actor__triangles_0_Geom__r10_"))) {
        return 70 * 1024 * 1024;  // 70MB (estimated)
    }
    else if (Filename.Contains(TEXT("vtk_actor__triangles_0_Geom__r15_"))) {
        return 71 * 1024 * 1024;  // 71MB
    }
    else if (Filename.Contains(TEXT(".usda")) ||
             Filename.Contains(TEXT(".usdc")) ||
             Filename.Contains(TEXT(".usd"))) {
        if (Filename.Contains(TEXT("_Geom__"))) {
            return 50 * 1024 * 1024;  // 50MB for geometry files
        }
        return 10 * 1024 * 1024;  // 10MB for other USD files
    }

    return 5 * 1024 * 1024;  // Default 5MB
}

int32 UJUSYNCBlueprintLibrary::CalculateDynamicTimeout(const FString& Filename, int32 TargetRank, bool bIsRetry, int32 RetryCount)
{
    const int32 BASE_TIMEOUT = 5000;  // 5 seconds minimum
    const int32 PER_MB_TIMEOUT = 200;  // 200ms per MB
    const int32 NETWORK_LATENCY_FACTOR = 1000;  // 1 second for network overhead
    const int32 RETRY_PENALTY = 2000;  // +2 seconds per retry

    int64 EstimatedSize = EstimateFileSizeFromFilename(Filename);

    int64 SizeMB = FMath::Max(1LL, EstimatedSize / (1024 * 1024));
    int32 SizeTimeout = SizeMB * PER_MB_TIMEOUT;

    int32 NetworkTimeout = NETWORK_LATENCY_FACTOR;

    int32 RetryTimeout = bIsRetry ? (RetryCount * RETRY_PENALTY) : 0;

    float RankFactor = RankPerformanceTracker.GetRankPerformanceFactor(TargetRank);
    int32 RankAdjustment = static_cast<int32>(RankFactor * 1000.0f);

    int32 TotalTimeout = BASE_TIMEOUT + SizeTimeout + NetworkTimeout + RetryTimeout + RankAdjustment;

    int32 MaxTimeout = bIsRetry ? 300000 : 120000;  // 5 minutes max for retries, 2 minutes normal
    return FMath::Clamp(TotalTimeout, 5000, MaxTimeout);
}

TArray<int32> UJUSYNCBlueprintLibrary::GetFallbackRanks(int32 TargetRank)
{
    TArray<int32> Ranks;

    const int32 FallbackOffsets[] = {1, 2, 3, 4, 5, -1, -2, -3, -4, -5, 0};

    for (int32 Offset : FallbackOffsets)
    {
        int32 Candidate = TargetRank + Offset;
        if (Candidate >= 0 && Candidate != TargetRank && !Ranks.Contains(Candidate))
        {
            Ranks.Add(Candidate);
        }
    }

    return Ranks;
}

void UJUSYNCBlueprintLibrary::ClearRankPerformanceStats()
{
    RankPerformanceTracker.ClearStats();
}

FString UJUSYNCBlueprintLibrary::GetRankPerformanceStats()
{
    return RankPerformanceTracker.GetStatsAsString();
}

void UJUSYNCBlueprintLibrary::RequestFileAsyncDynamic(
    const FString& Filename,
    int32 TargetRank,
    const FOnFileReceived& OnComplete,
    const FOnBrokerError& OnError,
    int32 MaxRetries,
    bool bUseExtractedRank)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        OnError.ExecuteIfBound(TEXT("Subsystem not available"));
        return;
    }

    int32 ExtractedRank = -1;
    if (bUseExtractedRank)
    {
        ExtractedRank = ExtractRankFromFilename(Filename);
        if (ExtractedRank >= 0 && ExtractedRank != TargetRank) {
            UE_LOG(LogJUSYNC, Warning, TEXT("Filename suggests rank %d, but requesting from rank %d. Using extracted rank."),
                ExtractedRank, TargetRank);
            TargetRank = ExtractedRank;
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Rank extraction from filename disabled. Using provided TargetRank: %d"), TargetRank);
    }

    TArray<int32> RanksToTry;
    RanksToTry.Add(TargetRank);
    RanksToTry.Append(GetFallbackRanks(TargetRank));

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, RanksToTry, Filename, MaxRetries, OnComplete, OnError]()
        {
            UJUSYNCSubsystem* SubsystemPtr = WeakSubsystem.Get();
            if (!SubsystemPtr)
            {
                UE_LOG(LogJUSYNC, Error, TEXT("Subsystem destroyed before async operation could complete"));
                AsyncTask(ENamedThreads::GameThread, [Filename, OnError]()
                    {
                        OnError.ExecuteIfBound(FString::Printf(TEXT("Subsystem destroyed while trying to retrieve '%s'"), *Filename));
                    });
                return;
            }

            TArray<uint8> FileData;
            bool bSuccess = false;
            int32 ActualRankUsed = -1;
            int32 TotalAttempts = 0;
            FString FinalError;

            const int32 MAX_TOTAL_TIME_MS = 300000;  // 5 minutes maximum
            FDateTime OperationStartTime = FDateTime::UtcNow();

            for (int32 RankIndex = 0; RankIndex < RanksToTry.Num() && !bSuccess; RankIndex++)
            {
                int32 CurrentRank = RanksToTry[RankIndex];

                if (!RankPerformanceTracker.CanTryRank(CurrentRank)) {
                    UE_LOG(LogJUSYNC, Log, TEXT("Skipping rank %d due to circuit breaker"), CurrentRank);
                    continue;
                }

                for (int32 Retry = 0; Retry < MaxRetries && !bSuccess; Retry++)
                {
                    FTimespan ElapsedTime = FDateTime::UtcNow() - OperationStartTime;
                    if (ElapsedTime.GetTotalMilliseconds() > MAX_TOTAL_TIME_MS)
                    {
                        FinalError = FString::Printf(TEXT("Exceeded maximum total time of %dms"), MAX_TOTAL_TIME_MS);
                        UE_LOG(LogJUSYNC, Error, TEXT("%s for file: %s"), *FinalError, *Filename);
                        break;
                    }

                    TotalAttempts++;

                    bool bIsRetry = (Retry > 0);
                    int32 DynamicTimeout = CalculateDynamicTimeout(Filename, CurrentRank, bIsRetry, Retry);

                    if (bIsRetry) {
                        int32 BackoffMs = FMath::Min(1000 * (1 << (Retry - 1)), 30000);
                        FPlatformProcess::Sleep(BackoffMs / 1000.0f);
                    }

                    RankPerformanceTracker.RecordRequestStart(CurrentRank);
                    FDateTime StartTime = FDateTime::UtcNow();

                    bSuccess = SubsystemPtr->RequestFile(Filename, CurrentRank, DynamicTimeout, FileData);

                    FDateTime EndTime = FDateTime::UtcNow();
                    FTimespan ResponseTime = EndTime - StartTime;
                    int32 ResponseTimeMs = ResponseTime.GetTotalMilliseconds();

                    RankPerformanceTracker.RecordRequestResult(CurrentRank, bSuccess, ResponseTimeMs);

                    if (bSuccess) {
                        ActualRankUsed = CurrentRank;
                        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved '%s' from rank %d in %dms (attempt %d, timeout: %dms)"),
                            *Filename, ActualRankUsed, ResponseTimeMs, TotalAttempts, DynamicTimeout);
                        break;
                    }
                    else {
                        FinalError = FString::Printf(TEXT("Failed attempt %d for '%s' from rank %d (timeout: %dms, actual: %dms)"),
                            TotalAttempts, *Filename, CurrentRank, DynamicTimeout, ResponseTimeMs);
                        UE_LOG(LogJUSYNC, Warning, TEXT("%s"), *FinalError);
                    }
                }
            }

            AsyncTask(ENamedThreads::GameThread, [bSuccess, Filename, FileData, ActualRankUsed, TotalAttempts, FinalError, OnComplete, OnError]()
                {
                    if (bSuccess) {
                        if (OnComplete.IsBound()) {
                            OnComplete.Execute(Filename, FileData);
                        }
                        else {
                            UE_LOG(LogJUSYNC, Warning, TEXT("OnComplete delegate not bound for successful file retrieval: %s"), *Filename);
                        }
                    }
                    else {
                        FString ErrorMsg = FString::Printf(
                            TEXT("Failed to retrieve '%s' after %d attempts (last tried rank: %d). %s"),
                            *Filename, TotalAttempts, ActualRankUsed, *FinalError);
                        if (OnError.IsBound()) {
                            OnError.Execute(ErrorMsg);
                        }
                        else {
                            UE_LOG(LogJUSYNC, Error, TEXT("OnError delegate not bound for failed file retrieval: %s"), *Filename);
                        }
                    }
                });
        });
}
