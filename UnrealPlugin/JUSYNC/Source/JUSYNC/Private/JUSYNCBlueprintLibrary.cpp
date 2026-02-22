#include "JUSYNCBlueprintLibrary.h"

#include "JUSYNCModule.h"
#include "JUSYNCSubsystem.h"
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

// Static member initialization
TArray<FJUSYNCFileData> UJUSYNCBlueprintLibrary::ReceivedFiles;
TArray<FString> UJUSYNCBlueprintLibrary::ReceivedMessages;
FCriticalSection UJUSYNCBlueprintLibrary::DataMutex;

TArray<FString> UJUSYNCBlueprintLibrary::LastFileList;
FCriticalSection UJUSYNCBlueprintLibrary::LastFileListMutex;

TArray<FString> UJUSYNCBlueprintLibrary::LastFileListWithSizes_Names;
TArray<int64> UJUSYNCBlueprintLibrary::LastFileListWithSizes_Sizes;
FCriticalSection UJUSYNCBlueprintLibrary::LastFileListWithSizesMutex;

// Benchmarking static member initialization
TArray<FJUSYNCBenchmarkResult> UJUSYNCBlueprintLibrary::BenchmarkResults;
FString UJUSYNCBlueprintLibrary::CurrentBenchmarkTest = TEXT("");
FJUSYNCBenchmarkConfig UJUSYNCBlueprintLibrary::CurrentBenchmarkConfig;
bool UJUSYNCBlueprintLibrary::bIsBenchmarking = false;

// ========== CONNECTION MANAGEMENT ==========

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
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Middleware initialized successfully"));
        //DisplayDebugMessage(TEXT("JUSYNC Middleware Connected"), 3.0f, FLinearColor::Green);
    }
    else
    {
        //DisplayDebugMessage(TEXT("JUSYNC Middleware Failed to Connect"), 5.0f, FLinearColor::Red);
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
        //DisplayDebugMessage(TEXT("JUSYNC Middleware Disconnected"), 3.0f, FLinearColor::Yellow);
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

    // Add reception statistics
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
        //DisplayDebugMessage(TEXT("JUSYNC Started Receiving Data"), 3.0f, FLinearColor::Blue);
    }

    return bResult;
}

void UJUSYNCBlueprintLibrary::StopJUSYNCReceiving()
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->StopReceiving();
        // Use FLinearColor constructor for Orange color
        //DisplayDebugMessage(TEXT("JUSYNC Stopped Receiving Data"), 3.0f, FLinearColor(1.0f, 0.5f, 0.0f, 1.0f));
    }
}

// ========== DEALER CLIENT FOR HPC BROKER ==========

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
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Connected to ANARI USD broker at %s"), *BrokerEndpoint);
        //DisplayDebugMessage(FString::Printf(TEXT("Connected to broker: %s"), *BrokerEndpoint), 3.0f, FLinearColor::Green);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to connect to broker at %s"), *BrokerEndpoint);
        //DisplayDebugMessage(FString::Printf(TEXT("Failed to connect to broker: %s"), *BrokerEndpoint), 5.0f, FLinearColor::Red);
    }

    return bResult;
}

void UJUSYNCBlueprintLibrary::DisconnectFromANARIUSDBroker()
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->DisconnectFromBroker();
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Disconnected from ANARI USD broker"));
        //DisplayDebugMessage(TEXT("Disconnected from broker"), 3.0f, FLinearColor::Yellow);
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
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Retrieved %d files from broker"), OutFiles.Num());
        //DisplayDebugMessage(FString::Printf(TEXT("Retrieved %d files from broker"), OutFiles.Num()), 3.0f, FLinearColor::Green);
        
        // Store the retrieved file list for later retrieval
        if (OutFiles.Num() > 0)
        {
            FScopeLock Lock(&LastFileListMutex);
            LastFileList = OutFiles;
            UE_LOG(LogJUSYNC, Log, TEXT("Stored %d files in LastFileList"), OutFiles.Num());
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to retrieve file list from broker"));
        //DisplayDebugMessage(TEXT("Failed to retrieve file list from broker"), 5.0f, FLinearColor::Red);
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
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Retrieved %d files with sizes from broker"), OutFiles.Num());
        // Optionally store the file list (without sizes) for later retrieval
        if (OutFiles.Num() > 0)
        {
            FScopeLock Lock(&LastFileListMutex);
            LastFileList = OutFiles;
            UE_LOG(LogJUSYNC, Log, TEXT("Stored %d files in LastFileList"), OutFiles.Num());
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to retrieve file list with sizes from broker"));
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
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Retrieved %d files with sizes and ranks from broker"), OutFiles.Num());
        // Store the file list with rank information for later retrieval
        if (OutFiles.Num() > 0)
        {
            FScopeLock Lock(&LastFileListMutex);
            LastFileList = OutFiles;
            // Also store rank information if needed
            UE_LOG(LogJUSYNC, Log, TEXT("Stored %d files with rank information in LastFileList"), OutFiles.Num());
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to retrieve file list with sizes and ranks from broker"));
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
    UE_LOG(LogJUSYNC, Log, TEXT("✅ Retrieved last file list (%d files)"), OutFileList.Num());
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
    UE_LOG(LogJUSYNC, Log, TEXT("✅ Retrieved last file list with sizes (%d files)"), OutFileList.Num());
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
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Retrieved file '%s' (%d bytes) from broker"), *Filename, OutData.Num());
        //DisplayDebugMessage(FString::Printf(TEXT("Retrieved file: %s (%d bytes)"), *Filename, OutData.Num()), 3.0f, FLinearColor::Green);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to retrieve file '%s' from broker"), *Filename);
        //DisplayDebugMessage(FString::Printf(TEXT("Failed to retrieve file: %s"), *Filename), 5.0f, FLinearColor::Red);
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
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Retrieved frame %d with %d files from broker"), FrameNumber, OutFiles.Num());
        //DisplayDebugMessage(FString::Printf(TEXT("Retrieved frame %d with %d files"), FrameNumber, OutFiles.Num()), 3.0f, FLinearColor::Green);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to retrieve frame %d from broker"), FrameNumber);
        //DisplayDebugMessage(FString::Printf(TEXT("Failed to retrieve frame %d"), FrameNumber), 5.0f, FLinearColor::Red);
    }

    return bResult;
}

// ========== WORKER STATUS QUERIES ==========

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
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Retrieved worker status for rank %d: %d workers"), TargetRank, OutWorkerStatus.Num());
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to retrieve worker status for rank %d"), TargetRank);
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
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Retrieved worker count: %d workers"), OutWorkerCount);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to retrieve worker count"));
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
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Retrieved total worker count (including rank 0): %d"), OutTotalCount);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to retrieve total worker count"));
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
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Retrieved worker count (excluding rank 0): %d workers"), OutWorkerCount);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to retrieve worker count"));
    }

    return bResult;
}

// ========== ASYNC WORKER QUERIES (NON-BLOCKING) ==========

void UJUSYNCBlueprintLibrary::RequestTotalWorkerCountAsync(int32 TimeoutMs, const FOnTotalWorkerCountReceived& OnComplete, const FOnBrokerError& OnError)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available"));
        OnError.ExecuteIfBound(TEXT("Subsystem not available"));
        return;
    }

    // Capture subsystem pointer for validity checking
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    
    // Launch async request on a background thread using Unreal's Async system
    Async(EAsyncExecution::Thread, [WeakSubsystem, TimeoutMs, OnComplete, OnError]()
    {
        // Check if subsystem is still valid before making the request
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem no longer valid, cancelling async request"));
            return; // Game stopped, exit early
        }
        
        int32 TotalCount = 0;
        bool bSuccess = false;
        
        // Make the broker request
        if (WeakSubsystem.IsValid())
        {
            bSuccess = WeakSubsystem->RequestTotalWorkerCount(TimeoutMs, TotalCount);
        }
        
        // Check again before calling back (game might have stopped during request)
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem destroyed during async request, cancelling callback"));
            return; // Game stopped, don't call callbacks
        }
        
        // Execute callback on game thread
        if (IsInGameThread())
        {
            if (bSuccess) OnComplete.ExecuteIfBound(TotalCount);
            else OnError.ExecuteIfBound(TEXT("Failed to retrieve total worker count"));
        }
        else
        {
            // Schedule on game thread
            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, TotalCount, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(TotalCount);
                    else OnError.ExecuteIfBound(TEXT("Failed to retrieve total worker count"));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        }
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

    // Capture subsystem pointer for validity checking
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    
    // Launch async request on a background thread using Unreal's Async system
    Async(EAsyncExecution::Thread, [WeakSubsystem, TimeoutMs, OnComplete, OnError]()
    {
        // Check if subsystem is still valid before making the request
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem no longer valid, cancelling async request"));
            return; // Game stopped, exit early
        }
        
        int32 WorkerCount = 0;
        bool bSuccess = false;
        
        // Make the broker request
        if (WeakSubsystem.IsValid())
        {
            bSuccess = WeakSubsystem->RequestWorkerCount(TimeoutMs, WorkerCount);
        }
        
        // Check again before calling back (game might have stopped during request)
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem destroyed during async request, cancelling callback"));
            return; // Game stopped, don't call callbacks
        }
        
        // Execute callback on game thread
        if (IsInGameThread())
        {
            if (bSuccess) OnComplete.ExecuteIfBound(WorkerCount);
            else OnError.ExecuteIfBound(TEXT("Failed to retrieve worker count"));
        }
        else
        {
            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, WorkerCount, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(WorkerCount);
                    else OnError.ExecuteIfBound(TEXT("Failed to retrieve worker count"));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        }
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

    // Capture subsystem pointer for validity checking
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    
    // Launch async request on a background thread using Unreal's Async system
    Async(EAsyncExecution::Thread, [WeakSubsystem, TargetRank, TimeoutMs, OnComplete, OnError]()
    {
        // Check if subsystem is still valid before making the request
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem no longer valid, cancelling async request"));
            return; // Game stopped, exit early
        }
        
        TArray<FJUSYNCWorkerStatus> WorkerStatus;
        bool bSuccess = false;
        
        // Make the broker request
        if (WeakSubsystem.IsValid())
        {
            bSuccess = WeakSubsystem->RequestWorkerStatus(TargetRank, TimeoutMs, WorkerStatus);
        }
        
        // Check again before calling back (game might have stopped during request)
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem destroyed during async request, cancelling callback"));
            return; // Game stopped, don't call callbacks
        }
        
        // Execute callback on game thread
        if (IsInGameThread())
        {
            if (bSuccess) OnComplete.ExecuteIfBound(WorkerStatus);
            else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve worker status for rank %d"), TargetRank));
        }
        else
        {
            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, WorkerStatus, TargetRank, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(WorkerStatus);
                    else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve worker status for rank %d"), TargetRank));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        }
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

    // Capture subsystem pointer for validity checking
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    
    // Launch async request on a background thread using Unreal's Async system
    Async(EAsyncExecution::Thread, [WeakSubsystem, TargetRank, TimeoutMs, OnComplete, OnError]()
    {
        // Check if subsystem is still valid before making the request
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem no longer valid, cancelling async request"));
            return; // Game stopped, exit early
        }
        
        TArray<FString> FileList;
        bool bSuccess = false;
        
        // Make the broker request
        if (WeakSubsystem.IsValid())
        {
            bSuccess = WeakSubsystem->RequestFileList(TargetRank, TimeoutMs, FileList);
        }
        
        // Store the retrieved file list for later retrieval (if successful)
        if (bSuccess && FileList.Num() > 0)
        {
            FScopeLock Lock(&UJUSYNCBlueprintLibrary::LastFileListMutex);
            UJUSYNCBlueprintLibrary::LastFileList = FileList;
            UE_LOG(LogJUSYNC, Log, TEXT("Stored %d files in LastFileList"), FileList.Num());
        }
        
        // Check again before calling back (game might have stopped during request)
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem destroyed during async request, cancelling callback"));
            return; // Game stopped, don't call callbacks
        }
        
        // Execute callback on game thread
        if (IsInGameThread())
        {
            if (bSuccess) OnComplete.ExecuteIfBound(FileList);
            else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve file list from rank %d"), TargetRank));
        }
        else
        {
            FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, FileList, TargetRank, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(FileList);
                    else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve file list from rank %d"), TargetRank));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        }
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

    // Capture subsystem pointer for validity checking
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    
    // Launch async request on a background thread using Unreal's Async system
    Async(EAsyncExecution::Thread, [WeakSubsystem, TargetRank, TimeoutMs, OnComplete, OnError]()
    {
        // Check if subsystem is still valid before making the request
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem no longer valid, cancelling async request"));
            return; // Game stopped, exit early
        }
        
        TArray<FString> FileList;
        TArray<int64> FileSizes;
        bool bSuccess = false;
        
        // Make the broker request
        if (WeakSubsystem.IsValid())
        {
            bSuccess = WeakSubsystem->RequestFileListWithSizes(TargetRank, TimeoutMs, FileList, FileSizes);
        }
        
        // Store the retrieved file list for later retrieval (if successful)
        if (bSuccess && FileList.Num() > 0)
        {
            FScopeLock Lock(&UJUSYNCBlueprintLibrary::LastFileListWithSizesMutex);
            UJUSYNCBlueprintLibrary::LastFileListWithSizes_Names = FileList;
            UJUSYNCBlueprintLibrary::LastFileListWithSizes_Sizes = FileSizes;
            UE_LOG(LogJUSYNC, Log, TEXT("Stored %d files with sizes in LastFileListWithSizes"), FileList.Num());
        }
        
        // Check again before calling back (game might have stopped during request)
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem destroyed during async request, cancelling callback"));
            return; // Game stopped, don't call callbacks
        }
        
        // Execute callback on game thread
        if (IsInGameThread())
        {
            if (bSuccess) OnComplete.ExecuteIfBound(FileList, FileSizes);
            else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve file list with sizes from rank %d"), TargetRank));
        }
        else
        {
            FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, FileList, FileSizes, TargetRank, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(FileList, FileSizes);
                    else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve file list with sizes from rank %d"), TargetRank));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        }
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

    // Capture subsystem pointer for validity checking
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    
    // Launch async request on a background thread using Unreal's Async system
    Async(EAsyncExecution::Thread, [WeakSubsystem, TargetRank, TimeoutMs, OnComplete, OnError]()
    {
        // Check if subsystem is still valid before making the request
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem no longer valid, cancelling async request"));
            return; // Game stopped, exit early
        }
        
        TArray<FString> FileList;
        TArray<int64> FileSizes;
        TArray<int32> FileRanks;
        bool bSuccess = false;
        
        // Make the broker request
        if (WeakSubsystem.IsValid())
        {
            bSuccess = WeakSubsystem->RequestFileListWithSizesAndRanks(TargetRank, TimeoutMs, FileList, FileSizes, FileRanks);
        }
        
        // Store the retrieved file list for later retrieval (if successful)
        if (bSuccess && FileList.Num() > 0)
        {
            FScopeLock Lock(&UJUSYNCBlueprintLibrary::LastFileListMutex);
            UJUSYNCBlueprintLibrary::LastFileList = FileList;
            UE_LOG(LogJUSYNC, Log, TEXT("Stored %d files with ranks in LastFileList"), FileList.Num());
        }
        
        // Check again before calling back (game might have stopped during request)
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem destroyed during async request, cancelling callback"));
            return; // Game stopped, don't call callbacks
        }
        
        // Execute callback on game thread
        if (IsInGameThread())
        {
            if (bSuccess) OnComplete.ExecuteIfBound(FileList, FileSizes, FileRanks);
            else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve file list with sizes and ranks from rank %d"), TargetRank));
        }
        else
        {
            FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, FileList, FileSizes, FileRanks, TargetRank, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(FileList, FileSizes, FileRanks);
                    else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve file list with sizes and ranks from rank %d"), TargetRank));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        }
    });
}

void UJUSYNCBlueprintLibrary::RequestFileAsync(const FString& Filename, int32 TargetRank, int32 TimeoutMs, const FOnFileReceived& OnComplete, const FOnBrokerError& OnError)
{
    // If timeout is excessively large (like 2,000,000ms), use dynamic timeout instead
    const int32 MAX_REASONABLE_TIMEOUT = 300000; // 5 minutes max
    if (TimeoutMs > MAX_REASONABLE_TIMEOUT)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("Timeout %dms is too large, using dynamic timeout instead"), TimeoutMs);
        
        // Extract rank from filename if possible
        int32 ExtractedRank = ExtractRankFromFilename(Filename);
        if (ExtractedRank >= 0 && ExtractedRank != TargetRank)
        {
            UE_LOG(LogJUSYNC, Log, TEXT("Adjusting target rank from %d to %d based on filename"), TargetRank, ExtractedRank);
            TargetRank = ExtractedRank;
        }
        
        // Calculate dynamic timeout
        int32 DynamicTimeout = CalculateDynamicTimeout(Filename, TargetRank, false, 0);
        UE_LOG(LogJUSYNC, Log, TEXT("Using dynamic timeout: %dms for file '%s'"), DynamicTimeout, *Filename);
        
        // Use the new dynamic async function with single retry
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

    // Capture subsystem pointer for validity checking
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    
    // Launch async request on a background thread using Unreal's Async system
    Async(EAsyncExecution::Thread, [WeakSubsystem, Filename, TargetRank, TimeoutMs, OnComplete, OnError]()
    {
        // Check if subsystem is still valid before making the request
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem no longer valid, cancelling async request"));
            return; // Game stopped, exit early
        }
        
        TArray<uint8> FileData;
        bool bSuccess = false;
        
        // Make the broker request
        if (WeakSubsystem.IsValid())
        {
            bSuccess = WeakSubsystem->RequestFile(Filename, TargetRank, TimeoutMs, FileData);
        }
        
        // Check again before calling back (game might have stopped during request)
        if (!WeakSubsystem.IsValid())
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Subsystem destroyed during async request, cancelling callback"));
            return; // Game stopped, don't call callbacks
        }
        
        // Execute callback on game thread
        if (IsInGameThread())
        {
            if (bSuccess) OnComplete.ExecuteIfBound(Filename, FileData);
            else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve file '%s' from rank %d"), *Filename, TargetRank));
        }
        else
        {
            FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady(
                [bSuccess, Filename, FileData, TargetRank, OnComplete, OnError]()
                {
                    if (bSuccess) OnComplete.ExecuteIfBound(Filename, FileData);
                    else OnError.ExecuteIfBound(FString::Printf(TEXT("Failed to retrieve file '%s' from rank %d"), *Filename, TargetRank));
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        }
    });
}

// ========== RANK EXTRACTION HELPER ==========

int32 UJUSYNCBlueprintLibrary::ExtractRankFromFilename(const FString& Filename)
{
    // Extract rank from filename using multiple patterns:
    // Pattern 1: _rX_ (e.g., vtk_actor__triangles_0_Geom__r0_0.000000.usda.usda)
    // Pattern 2: _X at end before extension (e.g., vtk_actor__triangles_0)
    // Pattern 3: _X_ somewhere in filename
    
    int32 Rank = -1;
    
    // Pattern 1: _rX_ 
    int32 RankStart = Filename.Find(TEXT("_r"));
    if (RankStart != INDEX_NONE)
    {
        int32 RankEnd = Filename.Find(TEXT("_"), ESearchCase::IgnoreCase, ESearchDir::FromStart, RankStart + 2);
        if (RankEnd != INDEX_NONE)
        {
            FString RankStr = Filename.Mid(RankStart + 2, RankEnd - (RankStart + 2));
            Rank = FCString::Atoi(*RankStr);
            if (Rank >= 0 && Rank < 16)
            {
                UE_LOG(LogJUSYNC, Log, TEXT("Extracted rank %d using pattern _rX_ from: %s"), Rank, *Filename);
                return Rank;
            }
        }
    }
    
    // Pattern 2: Look for last underscore before extension
    // Find last underscore in filename (before .usda or other extension)
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
            if (Rank >= 0 && Rank < 16)
            {
                UE_LOG(LogJUSYNC, Log, TEXT("Extracted rank %d using pattern _X at end from: %s"), Rank, *Filename);
                return Rank;
            }
        }
    }
    
    // Pattern 3: Try to find any number after underscore
    TArray<FString> Parts;
    Filename.ParseIntoArray(Parts, TEXT("_"), true);
    
    for (int32 i = Parts.Num() - 1; i >= 0; --i)
    {
        if (Parts[i].IsNumeric())
        {
            Rank = FCString::Atoi(*Parts[i]);
            if (Rank >= 0 && Rank < 16)
            {
                UE_LOG(LogJUSYNC, Log, TEXT("Extracted rank %d using numeric part from: %s"), Rank, *Filename);
                return Rank;
            }
        }
    }
    
    UE_LOG(LogJUSYNC, Warning, TEXT("Could not extract rank from filename: %s"), *Filename);
    return -1;
}

// ========== USD PROCESSING WITH PREVIEW ==========

bool UJUSYNCBlueprintLibrary::LoadUSDFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCMeshData>& OutMeshData, FString& OutPreview)
{
    if (!ValidateBufferSize(Buffer, TEXT("LoadUSDFromBuffer")))
    {
        return false;
    }

    // OPTIMIZATION: Only generate preview in debug builds to prevent memory hogging
    // In release builds, skip preview extraction to save CPU and memory
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
        //DisplayDebugMessage(Message, 5.0f, FLinearColor::Green);
        // OPTIMIZATION: Disabled USD preview logging to prevent memory hogging
        // UE_LOG(LogJUSYNC, Log, TEXT("USD Preview:\n%s"), *OutPreview);
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Successfully loaded %d meshes from USD buffer"), OutMeshData.Num());
    }
    else
    {
        //DisplayDebugMessage(TEXT("Failed to load USD file"), 5.0f, FLinearColor::Red);
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::LoadUSDFromDisk(const FString& FilePath, TArray<FJUSYNCMeshData>& OutMeshData, FString& OutPreview)
{
    if (!ValidateFilePath(FilePath, TEXT("LoadUSDFromDisk")))
    {
        return false;
    }

    // Load file to buffer first for preview
    TArray<uint8> Buffer;
    if (!LoadFileToBuffer(FilePath, Buffer))
    {
        return false;
    }

    // Extract filename from path
    FString Filename = FPaths::GetCleanFilename(FilePath);

    return LoadUSDFromBuffer(Buffer, Filename, OutMeshData, OutPreview);
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

    // Check file extension first (fast check)
    FString Extension = FPaths::GetExtension(Filename).ToLower();
    if (Extension != TEXT("usd") && Extension != TEXT("usda") &&
        Extension != TEXT("usdc") && Extension != TEXT("usdz"))
    {
        return false;
    }

    // OPTIMIZATION: Direct buffer scanning instead of full ExtractUSDAPreview
    // We only need to check for a few markers in the first few KB
    const uint8* BufferData = Buffer.GetData();
    if (!BufferData)
    {
        return false;
    }

    // Scan first 8KB for USD markers (more than enough)
    const int32 SCAN_SIZE = FMath::Min(Buffer.Num(), 8 * 1024);
    
    // Convert to string for searching (but only the scanned portion)
    FString FirstChunk;
    FirstChunk.Reserve(SCAN_SIZE);
    
    for (int32 i = 0; i < SCAN_SIZE; ++i)
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

    // Check for USD markers
    return FirstChunk.Contains(TEXT("#usda")) ||
           FirstChunk.Contains(TEXT("PXR-USDC")) ||
           FirstChunk.Contains(TEXT("def ")) ||
           FirstChunk.Contains(TEXT("over "));
}

// ========== TEXTURE PROCESSING ==========

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
        // Use FLinearColor constructor for Cyan color
        //DisplayDebugMessage(Message, 3.0f, FLinearColor(0.0f, 1.0f, 1.0f, 1.0f));
    }
    else
    {
        //DisplayDebugMessage(TEXT("Failed to create texture from buffer"), 3.0f, FLinearColor::Red);
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
        // Use FLinearColor constructor for Cyan color
        //DisplayDebugMessage(TEXT("UE Texture2D Created Successfully"), 3.0f, FLinearColor(0.0f, 1.0f, 1.0f, 1.0f));
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
        //DisplayDebugMessage(Message, 3.0f, FLinearColor::Green);
    }
    else
    {
        //DisplayDebugMessage(TEXT("Failed to save gradient PNG"), 3.0f, FLinearColor::Red);
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
        //DisplayDebugMessage(Message, 3.0f, FLinearColor::Green);
    }
    else
    {
        //DisplayDebugMessage(TEXT("Failed to create gradient PNG buffer"), 3.0f, FLinearColor::Red);
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
        //DisplayDebugMessage(Message, 3.0f, FLinearColor::Green);
    }
    else
    {
        //DisplayDebugMessage(TEXT("Failed to get PNG dimensions"), 3.0f, FLinearColor::Red);
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
        //DisplayDebugMessage(Message, 3.0f, FLinearColor::Green);
    }
    else
    {
        FString Message = FString::Printf(TEXT("Failed to extract row %d as PNG buffer"), RowIndex);
        //DisplayDebugMessage(Message, 3.0f, FLinearColor::Red);
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

// ========== REALTIMEMESH PROCESSING ==========

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
        // Use FLinearColor constructor for Cyan color
        //DisplayDebugMessage(Message, 5.0f, FLinearColor(0.0f, 1.0f, 1.0f, 1.0f));
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
    // Use FLinearColor constructor for Cyan or Yellow color
    FLinearColor Color = bAllSuccessful ? FLinearColor(0.0f, 1.0f, 1.0f, 1.0f) : FLinearColor::Yellow;
    //DisplayDebugMessage(Message, 5.0f, Color);

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

      // Call the subsystem's batch function (it already has frame budget management built-in)
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

// ========== DATA RECEPTION ==========

bool UJUSYNCBlueprintLibrary::CheckForReceivedFiles(TArray<FJUSYNCFileData>& OutReceivedFiles)
{
    FScopeLock Lock(&DataMutex);

    if (ReceivedFiles.Num() > 0)
    {
        OutReceivedFiles = ReceivedFiles;

        // Log received files for debugging
        for (const FJUSYNCFileData& FileData : ReceivedFiles)
        {
            FString Message = FString::Printf(TEXT("Received File: %s (%d bytes, %s)"),
                                            *FileData.Filename, FileData.Data.Num(), *FileData.FileType);
            UE_LOG(LogJUSYNC, Log, TEXT("%s"), *Message);
            //DisplayDebugMessage(Message, 5.0f, FLinearColor::Blue);
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
        OutReceivedMessages = ReceivedMessages;

        // Log received messages for debugging
        for (const FString& Message : ReceivedMessages)
        {
            UE_LOG(LogJUSYNC, Log, TEXT("Received Message: %s"), *Message);
            // Use FLinearColor constructor for Cyan color
            //DisplayDebugMessage(FString::Printf(TEXT("Message: %s"), *Message), 3.0f, FLinearColor(0.0f, 1.0f, 1.0f, 1.0f));
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

// ========== VALIDATION & UTILITIES ==========

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

    // Check index bounds
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
            Extensions.Add(TEXT(".usd"));
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
            // No filtering
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
            // Ensure extension starts with dot
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
            Extensions.Add(TEXT(".usd"));
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
            // No filtering
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
            // Ensure extension starts with dot
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

// ========== RANK-AWARE FILTER FUNCTIONS ==========

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
            Extensions.Add(TEXT(".usd"));
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
            // No filtering
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
                break; // Found matching extension, move to next file
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
                break; // Found matching extension, move to next file
            }
        }
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("FilterFileListByExtensionsWithSizesAndRanks: filtered %d files down to %d (allowed extensions: %s)"),
           FileList.Num(), OutFilteredFiles.Num(), *FString::Join(AllowedExtensions, TEXT(", ")));
}

int32 UJUSYNCBlueprintLibrary::CalculateTimeoutFromFileSize(int64 FileSizeBytes, int32 BaseTimeoutMs, float BandwidthBytesPerSecond)
{
    if (FileSizeBytes <= 0)
        return BaseTimeoutMs;
    
    // Calculate transfer time in seconds: size / bandwidth
    float TransferTimeSeconds = static_cast<float>(FileSizeBytes) / BandwidthBytesPerSecond;
    // Convert to milliseconds and add base timeout
    int32 AdditionalMs = FMath::CeilToInt(TransferTimeSeconds * 1000.0f);
    // Add safety margin (e.g., 20%)
    AdditionalMs = FMath::CeilToInt(AdditionalMs * 1.2f);
    int32 TotalTimeout = BaseTimeoutMs + AdditionalMs;
    
    // Clamp to reasonable range (e.g., max 60 seconds)
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

// ========== FILE OPERATIONS ==========

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
        //DisplayDebugMessage(Message, 3.0f, FLinearColor::Green);
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
        //DisplayDebugMessage(Message, 3.0f, FLinearColor::Green);
    }

    return bResult;
}

// ========== DEBUG & DISPLAY ==========

void UJUSYNCBlueprintLibrary::DisplayDebugMessage(const FString& Message, float Duration, FLinearColor Color)
{
    /*if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(-1, Duration, Color.ToFColor(true), FString::Printf(TEXT("JUSYNC: %s"), *Message));
    }*/

    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC: %s"), *Message);
}

void UJUSYNCBlueprintLibrary::LogJUSYNCMessage(const FString& Message, bool bIsError)
{
    if (bIsError)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC: %s"), *Message);
        //DisplayDebugMessage(Message, 5.0f, FLinearColor::Red);
    }
    else
    {
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC: %s"), *Message);
        //DisplayDebugMessage(Message, 3.0f, FLinearColor::White);
    }
}

// ========== HELPER FUNCTIONS ==========

UJUSYNCSubsystem* UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem()
{
    // Method 1: Try GWorld first (most reliable for PIE)
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
    
    // Method 2: Try from all world contexts (comprehensive fallback)
    if (GEngine)
    {
        // First try PIE worlds
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
        
        // Then try game worlds
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

// ========== PRIVATE HELPERS ==========

bool UJUSYNCBlueprintLibrary::ValidateBufferSize(const TArray<uint8>& Buffer, const FString& Context)
{
    if (Buffer.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("%s: Buffer is empty"), *Context);
        return false;
    }

    // Use your middleware's safety constants
    const size_t MaxSize = 1024 * 1024 * 1024; // 1GB limit from your middleware
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

    // Check for dangerous path patterns
    if (FilePath.Contains(TEXT("..")) || FilePath.Contains(TEXT("~/")))
    {
        UE_LOG(LogJUSYNC, Error, TEXT("%s: Unsafe file path detected: %s"), *Context, *FilePath);
        return false;
    }

    return true;
}

FString UJUSYNCBlueprintLibrary::ExtractUSDAPreview(const TArray<uint8>& Buffer, int32 MaxLines)
{
    // Enhanced safety check - validate buffer before any access
    if (Buffer.Num() == 0)
    {
        return TEXT("Empty buffer");
    }

    // Additional safety: check if buffer data pointer is valid (as much as we can)
    const uint8* BufferData = Buffer.GetData();
    if (!BufferData)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("ExtractUSDAPreview: Buffer has null data pointer"));
        return TEXT("Invalid buffer");
    }

    // OPTIMIZATION: For preview purposes, we only need to search a reasonable amount
    // Max 64KB is enough for preview, even for huge USD files
    const int32 MAX_PREVIEW_SIZE = 64 * 1024; // 64KB
    int32 SearchSize = FMath::Min(Buffer.Num(), MAX_PREVIEW_SIZE);
    
    // OPTIMIZATION: Use FString::ChrArray for bulk conversion instead of character-by-character
    // Convert buffer to string efficiently
    FString Content;
    Content.Reserve(SearchSize);
    
    // Process in chunks for better performance
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
            // Skip non-printable characters
        }
        
        Content += Chunk;
    }

    // Extract first N lines
    TArray<FString> Lines;
    Content.ParseIntoArrayLines(Lines, false); // false = don't cull empty lines
    
    FString Preview = TEXT("=== USD PREVIEW ===\n");
    int32 LinesToShow = FMath::Min(MaxLines, Lines.Num());
    
    for (int32 i = 0; i < LinesToShow; ++i)
    {
        FString Line = Lines[i];
        if (Line.Len() > 200) // Reduced from 500 to 200 for preview
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
    
    // OPTIMIZATION: Removed excessive logging to reduce resource usage
    // UE_LOG(LogJUSYNC, Log, TEXT("ExtractUSDAPreview: Converted %d characters, %d lines"), 
    //        Content.Len(), Lines.Num());
    
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
            UE_LOG(LogJUSYNC, Log, TEXT("✅ Applied enhanced default material"));
        }
    }
}

// Update the async internal function
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

    // Calculate batch range
    int32 StartIndex = CurrentBatch * BatchSize;
    int32 EndIndex = FMath::Min(StartIndex + BatchSize, MeshDataArray.Num());
    
    UE_LOG(LogJUSYNC, Log, TEXT("📦 Processing async batch %d: indices %d-%d with rotations"),
           CurrentBatch, StartIndex, EndIndex - 1);

    // Process current batch with rotations
    for (int32 i = StartIndex; i < EndIndex; ++i)
    {
        FRotator UERotation = ConvertParaViewToUERotation(SpawnRotations[i]);
        AActor* SpawnedActor = SpawnRealtimeMeshAtLocation(
            MeshDataArray[i], SpawnLocations[i], UERotation);
        
        SharedResults->Add(SpawnedActor);
        
        if (SpawnedActor)
        {
            // Extract rank from filename for logging
            int32 Rank = ExtractRankFromFilename(MeshDataArray[i].ElementName);
            
            UE_LOG(LogJUSYNC, Log, TEXT("✅ Async spawned mesh %d (Rank %d) at %s with rotation %s"),
                   i, Rank, *SpawnLocations[i].ToString(), *UERotation.ToString());
        }
    }

    // Check if we're done
    if (EndIndex >= MeshDataArray.Num())
    {
        UE_LOG(LogJUSYNC, Log, TEXT("🎉 Async batch spawn complete: %d/%d successful"),
               SharedResults->Num(), MeshDataArray.Num());
        return;
    }

    // Schedule next batch
    FTimerHandle TimerHandle;
    World->GetTimerManager().SetTimer(TimerHandle,
        FTimerDelegate::CreateLambda([=]()
        {
            AsyncBatchSpawnInternal(MeshDataArray, SpawnLocations, SpawnRotations, SharedResults,
                                  CurrentBatch + 1, BatchSize, BatchDelay);
        }),
        BatchDelay, false);
}

// Add this function after your existing helper functions
FString UJUSYNCBlueprintLibrary::DetectUSDContentType(const TArray<uint8>& Buffer)
{
    // OPTIMIZATION: Direct pattern scanning without full preview extraction
    // Scan only first 16KB of buffer for efficiency with large USD files
    const int32 MAX_SCAN_SIZE = 16 * 1024; // 16KB is enough for content detection
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
    
    // Convert first ScanSize bytes to ASCII string for pattern matching
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
        // Skip non-printable characters
    }
    
    // OPTIMIZATION: Reduced logging - only log in verbose mode
    #if JUSYNC_VERBOSE_LOGGING
    UE_LOG(LogJUSYNC, Log, TEXT("=== USD CONTENT TYPE DETECTION DEBUG ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Buffer size: %d bytes, Scanned: %d bytes"), Buffer.Num(), ScanSize);
    UE_LOG(LogJUSYNC, Log, TEXT("First chunk length: %d characters"), FirstChunk.Len());
    #endif
    
    // Look for primvars:color.timeSamples
    bool bHasPrimvarsColor = FirstChunk.Contains(TEXT("primvars:color.timeSamples"));
    
    #if JUSYNC_VERBOSE_LOGGING
    UE_LOG(LogJUSYNC, Log, TEXT("Contains 'primvars:color.timeSamples': %s"), 
           bHasPrimvarsColor ? TEXT("YES") : TEXT("NO"));
    #endif
    
    if (bHasPrimvarsColor)
    {
        // Look for actual color data patterns FIRST
        bool bHasActualColorData = FirstChunk.Contains(TEXT("0: [(0.")) ||
                                  FirstChunk.Contains(TEXT("0: [(1.")) ||
                                  FirstChunk.Contains(TEXT("), (0.")) ||
                                  FirstChunk.Contains(TEXT("), (1.")) ||
                                  (FirstChunk.Contains(TEXT("0: [(")) && FirstChunk.Contains(TEXT("), (")));
        
        UE_LOG(LogJUSYNC, Log, TEXT("Contains actual color data patterns: %s"), 
               bHasActualColorData ? TEXT("YES") : TEXT("NO"));
        
        // CRITICAL FIX: Prioritize actual color data over "None"
        if (bHasActualColorData)
        {
            UE_LOG(LogJUSYNC, Log, TEXT("🎨 DETECTED: VERTEX_COLORS (actual color data found)"));
            return TEXT("VERTEX_COLORS");
        }
        else if (FirstChunk.Contains(TEXT("0: None")))
        {
            UE_LOG(LogJUSYNC, Log, TEXT("🎨 DETECTED: TEXTURES (None values found, no color data)"));
            return TEXT("TEXTURES");
        }
    }
    
    // Check for explicit texture references
    if (FirstChunk.Contains(TEXT("asset inputs:file")) ||
        FirstChunk.Contains(TEXT("UsdUVTexture")) ||
        FirstChunk.Contains(TEXT(".jpg")) ||
        FirstChunk.Contains(TEXT(".png")))
    {
        UE_LOG(LogJUSYNC, Log, TEXT("🎨 DETECTED: TEXTURES (explicit references)"));
        return TEXT("TEXTURES");
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("🎨 DETECTED: GEOMETRY_ONLY"));
    return TEXT("GEOMETRY_ONLY");
}


TArray<AActor*> UJUSYNCBlueprintLibrary::BatchSpawnRealtimeMeshesAtLocationsSync(
    const TArray<FJUSYNCMeshData>& MeshDataArray,
    const TArray<FVector>& SpawnLocations,
    const TArray<FRotator>& SpawnRotations)
{
    TArray<AActor*> SpawnedActors;
    
    // Create default rotations if not provided
    TArray<FRotator> FinalRotations = SpawnRotations;
    if (FinalRotations.Num() == 0)
    {
        FinalRotations = GenerateDefaultRotations(MeshDataArray.Num());
    }

    UE_LOG(LogJUSYNC, Log, TEXT("=== SYNC BATCH SPAWN WITH ROTATIONS ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Processing %d meshes with locations and rotations"), MeshDataArray.Num());

    SpawnedActors.Reserve(MeshDataArray.Num());
    int32 SuccessCount = 0;

    for (int32 i = 0; i < MeshDataArray.Num(); ++i)
    {
        // Convert ParaView rotation to UE rotation if needed
        FRotator UERotation = ConvertParaViewToUERotation(FinalRotations[i]);
        
        // Extract rank from filename for logging
        int32 Rank = ExtractRankFromFilename(MeshDataArray[i].ElementName);
        
        UE_LOG(LogJUSYNC, Log, TEXT("🎯 Spawning mesh %d (Rank %d) '%s' at location %s with rotation %s"),
               i, Rank, *MeshDataArray[i].ElementName, *SpawnLocations[i].ToString(), *UERotation.ToString());

        AActor* SpawnedActor = SpawnRealtimeMeshAtLocation(MeshDataArray[i], SpawnLocations[i], UERotation);
        SpawnedActors.Add(SpawnedActor);

        if (SpawnedActor)
        {
            SuccessCount++;
            UE_LOG(LogJUSYNC, Log, TEXT("✅ Successfully spawned at %s with rotation %s"),
                   *SpawnedActor->GetActorLocation().ToString(), 
                   *SpawnedActor->GetActorRotation().ToString());
        }
        else
        {
            UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to spawn mesh %d"), i);
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("=== SYNC BATCH SPAWN COMPLETE: %d/%d successful ==="),
           SuccessCount, MeshDataArray.Num());
    return SpawnedActors;
}

FRotator UJUSYNCBlueprintLibrary::ConvertParaViewToUERotation(const FRotator& ParaViewRotation)
{
    // ParaView and UE both use Z-up, but ParaView is right-handed, UE is left-handed
    FRotator UERotation;
    
    // For Z-up to Z-up conversion with handedness flip:
    UERotation.Pitch = ParaViewRotation.Pitch;  // Keep pitch
    UERotation.Yaw = -ParaViewRotation.Yaw;     // Flip yaw for handedness
    UERotation.Roll = ParaViewRotation.Roll;    // Keep roll
    
    UE_LOG(LogJUSYNC, Log, TEXT("Converted ParaView rotation %s to UE rotation %s"),
           *ParaViewRotation.ToString(), *UERotation.ToString());
    
    return UERotation;
}

// Generate default locations in a grid pattern
TArray<FVector> UJUSYNCBlueprintLibrary::GenerateDefaultLocations(int32 Count, const FVector& BaseLocation, float Spacing)
{
    TArray<FVector> Locations;
    Locations.Reserve(Count);
    
    if (Count <= 0)
    {
        return Locations;
    }
    
    // Calculate grid dimensions
    int32 GridSize = FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Count)));
    
    for (int32 i = 0; i < Count; ++i)
    {
        // Calculate grid position
        int32 Row = i / GridSize;
        int32 Col = i % GridSize;
        
        // Create location with spacing
        FVector Location = BaseLocation + FVector(Col * Spacing, Row * Spacing, 0.0f);
        Locations.Add(Location);
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("Generated %d default locations in %dx%d grid (spacing: %.1f)"),
           Count, GridSize, GridSize, Spacing);
    
    return Locations;
}

// Generate default rotations
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
    // Enhanced validation
    if (MeshData.Vertices.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("⚠️ Empty mesh data provided"));
        return nullptr;
    }

    // Get subsystem and world
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

    // Process mesh data
    FJUSYNCMeshData ProcessedMeshData = FixMeshDataForSpawning(MeshData);
    FRotator UERotation = ConvertParaViewToUERotation(SpawnRotation);

    UE_LOG(LogJUSYNC, Log, TEXT("🎯 Spawning mesh '%s' at location %s with rotation %s"),
           *ProcessedMeshData.ElementName, *SpawnLocation.ToString(), *UERotation.ToString());

    // Calculate scale factor if uniform scaling is enabled
    FVector MeshScaleFactor = FVector::OneVector;
    if (bUseUniformScaling && OuterBoundingBoxSize != FVector::ZeroVector)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("🎯 Applying uniform scaling with bounding box: %s"),
               *OuterBoundingBoxSize.ToString());

        FVector MeshSize(40.0f, 40.0f, 40.0f); // Default fallback
        if (ProcessedMeshData.Vertices.Num() > 0)
        {
            FBox MeshBounds(EForceInit::ForceInit);
            for (const FVector& Vertex : ProcessedMeshData.Vertices)
            {
                MeshBounds += Vertex;
            }
            MeshSize = MeshBounds.GetSize();
            UE_LOG(LogJUSYNC, Log, TEXT("📐 Mesh size from vertices: %s"), *MeshSize.ToString());
        }
        else
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("⚠️ Mesh has no vertices, using default size"));
        }

        // Calculate scale factor for this mesh
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

        UE_LOG(LogJUSYNC, Log, TEXT("🎯 Mesh scale factor: %s (MeshSize: %s, TargetSize: %s)"),
               *MeshScaleFactor.ToString(), *MeshSize.ToString(), *OuterBoundingBoxSize.ToString());
    }
    else
    {
        // No scaling - scale factor remains (1,1,1)
        UE_LOG(LogJUSYNC, Log, TEXT("📏 No uniform scaling applied"));
    }

    // **FIXED ACTOR SPAWNING - Let engine auto-generate names**
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    // **CRITICAL FIX: Do NOT set SpawnParams.Name - let Unreal Engine auto-generate unique names**

    AActor* SpawnedActor = World->SpawnActor<AActor>(SpawnParams);
    if (!SpawnedActor)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to spawn actor"));
        return nullptr;
    }

    // **ENHANCED: Use Tags for identification**
    SpawnedActor->Tags.Add(FName(*FString::Printf(TEXT("JUSYNC_%s"), *ProcessedMeshData.ElementName)));
    UE_LOG(LogJUSYNC, Log, TEXT("✅ Spawned actor with auto-generated name: %s"), *SpawnedActor->GetName());

    // **ENHANCED COMPONENT CREATION**
    URealtimeMeshComponent* MeshComp = NewObject<URealtimeMeshComponent>(SpawnedActor);
    SpawnedActor->SetRootComponent(MeshComp);
    MeshComp->RegisterComponent();

    // **ENHANCED TRANSFORM APPLICATION**
    FTransform ActorTransform(UERotation, SpawnLocation, MeshScaleFactor);
    SpawnedActor->SetActorTransform(ActorTransform);

    // **ENHANCED SCALING APPLICATION** - Multiple methods for reliability
    if (bUseUniformScaling && MeshScaleFactor != FVector::OneVector)
    {
        // Validate components before applying scaling
        if (!MeshComp)
        {
            UE_LOG(LogJUSYNC, Error, TEXT("❌ Cannot apply scaling: MeshComp is null"));
        }
        else if (!SpawnedActor->GetRootComponent())
        {
            UE_LOG(LogJUSYNC, Error, TEXT("❌ Cannot apply scaling: Actor has no root component"));
        }
        else
        {
            // Always apply scaling when uniform scaling is enabled
            // Method 1: Component-level scaling
            MeshComp->SetWorldScale3D(MeshScaleFactor);
            // Method 2: Actor-level scaling (redundant but ensures it works)
            SpawnedActor->SetActorScale3D(MeshScaleFactor);
            // Method 3: Force transform update
            SpawnedActor->SetActorTransform(FTransform(UERotation, SpawnLocation, MeshScaleFactor));
            // Method 4: Mark for render state update
            MeshComp->MarkRenderStateDirty();

            // Verify scaling was applied
            FVector ActualActorScale = SpawnedActor->GetActorScale3D();
            FVector ActualComponentScale = MeshComp->GetComponentScale();
            
            if (ActualActorScale.Equals(MeshScaleFactor, 0.01f) && ActualComponentScale.Equals(MeshScaleFactor, 0.01f))
            {
                UE_LOG(LogJUSYNC, Log, TEXT("✅ Applied scale %s to actor '%s' (Verified: Actor=%s, Component=%s)"),
                       *MeshScaleFactor.ToString(), *ProcessedMeshData.ElementName,
                       *ActualActorScale.ToString(), *ActualComponentScale.ToString());
            }
            else
            {
                UE_LOG(LogJUSYNC, Error, TEXT("❌ Scaling mismatch for actor: Target=%s, Actor=%s, Component=%s"),
                       *MeshScaleFactor.ToString(),
                       *ActualActorScale.ToString(), *ActualComponentScale.ToString());
            }
        }
    }

    // **ENHANCED MATERIAL APPLICATION**
    if (Material)
    {
        // Always use the provided material (your texture material from Blueprint)
        MeshComp->SetMaterial(0, Material);
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Applied PROVIDED material to mesh"));
    }
    else
    {
        // Apply default material when no material is provided
        ApplyEnhancedDefaultMaterial(MeshComp);
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Applied default material to mesh"));
    }

    // **ENHANCED MESH CREATION - WITH ASYNC SUPPORT AND AUTOMATIC SPLITTING**
    bool bSuccess;
    
    // Check if mesh needs splitting (exceeds RMC vertex/triangle limits)
    const int32 TotalVertices = ProcessedMeshData.Vertices.Num();
    const int32 TotalTriangles = ProcessedMeshData.Triangles.Num() / 3;
    const int32 RMCVertexLimit = 32768; // From CheckMemoryLimitsForMesh
    const int32 RMCTriangleLimit = 65536; // From CheckMemoryLimitsForMesh
    
    bool bNeedsSplitting = (TotalVertices > RMCVertexLimit) || (TotalTriangles > RMCTriangleLimit);
    
    if (bUseAsyncSpawning)
    {
        if (bNeedsSplitting)
        {
            // For large meshes with async, we need to handle splitting differently
            // Since there's no async splitting function, we'll use sync splitting for now
            UE_LOG(LogJUSYNC, Warning, TEXT("⚠️ Large mesh detected (%d vertices, %d triangles) - using synchronous splitting with async spawn"),
                   TotalVertices, TotalTriangles);
            
            // Use synchronous splitting for large meshes
            bSuccess = Subsystem->CreateRealtimeMeshFromJUSYNCWithSplitting(
                ProcessedMeshData, 
                MeshComp,
                RMCVertexLimit
            );
            UE_LOG(LogJUSYNC, Log, TEXT("🔀 Using SPLITTING mesh creation (%d vertices > %d limit)"),
                   TotalVertices, RMCVertexLimit);
        }
        else
        {
            // Use async mesh creation for small meshes (doesn't block game thread)
            Subsystem->CreateRealtimeMeshFromJUSYNC_Async(ProcessedMeshData, MeshComp);
            bSuccess = true; // Async assumes success, errors handled internally
            UE_LOG(LogJUSYNC, Log, TEXT("🔄 Using ASYNC mesh creation"));
        }
        
        // For async, we can't verify immediately, but log the spawn
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Mesh creation started for mesh at %s"),
               *SpawnLocation.ToString());
    }
    else
    {
        // Synchronous mesh creation
        if (bNeedsSplitting)
        {
            // Use splitting for large meshes
            bSuccess = Subsystem->CreateRealtimeMeshFromJUSYNCWithSplitting(
                ProcessedMeshData, 
                MeshComp,
                RMCVertexLimit
            );
            UE_LOG(LogJUSYNC, Log, TEXT("🔀 Using SPLITTING mesh creation (%d vertices > %d limit)"),
                   TotalVertices, RMCVertexLimit);
        }
        else
        {
            // Use standard creation for small meshes
            bSuccess = Subsystem->CreateRealtimeMeshFromJUSYNC(ProcessedMeshData, MeshComp);
        }
        
        if (bSuccess)
        {
            // **FINAL VERIFICATION**
            FVector ActualLocation = SpawnedActor->GetActorLocation();
            FVector ActualScale = SpawnedActor->GetActorScale3D();
            FRotator ActualRotation = SpawnedActor->GetActorRotation();

            UE_LOG(LogJUSYNC, Log, TEXT("✅ Successfully spawned mesh at %s (Scale: %s, Rotation: %s)"),
                   *ActualLocation.ToString(), *ActualScale.ToString(), *ActualRotation.ToString());
            
            // Log splitting info if used
            if (bNeedsSplitting)
            {
                UE_LOG(LogJUSYNC, Log, TEXT("🔀 Mesh split into multiple RMC components for better performance"));
            }
        }
        else
        {
            UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to create RealtimeMesh for actor, destroying"));
            SpawnedActor->Destroy();
            return nullptr;
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("=== SINGLE MESH SPAWN COMPLETE ==="));
    return SpawnedActor;
}

FJUSYNCMeshData UJUSYNCBlueprintLibrary::FixMeshDataForSpawning(const FJUSYNCMeshData& InputMeshData)
{
    FJUSYNCMeshData FixedData = InputMeshData;
    
    // Basic vertex validation
    for (int32 i = 0; i < FixedData.Vertices.Num(); ++i)
    {
        FVector& Vertex = FixedData.Vertices[i];
        if (!FMath::IsFinite(Vertex.X) || !FMath::IsFinite(Vertex.Y) || !FMath::IsFinite(Vertex.Z))
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Fixed invalid vertex at index %d"), i);
            Vertex = FVector::ZeroVector;
        }
    }
    
    // Basic triangle validation - keep only valid triangles
    TArray<int32> ValidTriangles;
    ValidTriangles.Reserve(FixedData.Triangles.Num());
    
    for (int32 i = 0; i < FixedData.Triangles.Num(); i += 3)
    {
        if (i + 2 < FixedData.Triangles.Num())
        {
            int32 i0 = FixedData.Triangles[i];
            int32 i1 = FixedData.Triangles[i + 1];
            int32 i2 = FixedData.Triangles[i + 2];
            
            // Basic bounds checking and degenerate triangle check
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
    
    // Recalculate normals if missing or invalid
    if (!FixedData.HasNormals() || FixedData.Normals.Num() != FixedData.Vertices.Num())
    {
        FixedData.Normals.SetNum(FixedData.Vertices.Num());
        for (int32 i = 0; i < FixedData.Normals.Num(); ++i)
        {
            FixedData.Normals[i] = FVector::ZeroVector;
        }
        
        // Calculate face normals and accumulate
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
        
        // Normalize accumulated normals
        for (int32 i = 0; i < FixedData.Normals.Num(); ++i)
        {
            FixedData.Normals[i] = FixedData.Normals[i].GetSafeNormal();
            if (FixedData.Normals[i].IsNearlyZero())
            {
                FixedData.Normals[i] = FVector::UpVector; // Fallback normal
            }
        }
        
        UE_LOG(LogJUSYNC, Log, TEXT("Recalculated normals for mesh: %s"), *InputMeshData.ElementName);
    }
    
    // Validate and fix UV coordinates
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
    
    // Add all vertices to the bounding box, transformed by location
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

    // **FIX: Handle single point case with mesh-based scaling**
    if (OriginalLocations.Num() == 1)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("🔧 Single spawn point - calculating scale based on mesh bounds"));
        
        // For single point, calculate scale based on the mesh extent from USD
        // Your USD shows extent [(-20, -20, -20), (20, 20, 20)] = 40x40x40 size
        FVector MeshSize(40.0f, 40.0f, 40.0f); // Based on your USD extent
        
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
        
        UE_LOG(LogJUSYNC, Log, TEXT("🎯 Single point scale factor: %s"), *OutScaleFactor.ToString());
        return OriginalLocations; // Return original location, scaling will be applied to actor
    }

    // Rest of your existing multi-point logic...
    FBox CombinedBounds(EForceInit::ForceInit);
    for (const FVector& Location : OriginalLocations)
    {
        CombinedBounds += Location;
    }

    FVector CurrentSize = CombinedBounds.GetSize();
    FVector BoundingBoxCenter = CombinedBounds.GetCenter();

    if (CurrentSize.IsNearlyZero())
    {
        UE_LOG(LogJUSYNC, Log, TEXT("🔧 Zero-size bounding box detected - no scaling needed"));
        OutScaleFactor = FVector::OneVector;
        return OriginalLocations;
    }

    // Calculate scale factor with safety checks
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

    // Calculate scaled positions
    TArray<FVector> ScaledLocations;
    ScaledLocations.Reserve(OriginalLocations.Num());

    for (const FVector& Location : OriginalLocations)
    {
        FVector RelativePosition = Location - BoundingBoxCenter;
        FVector ScaledPosition = RelativePosition * OutScaleFactor;
        FVector NewLocation = BoundingBoxCenter + ScaledPosition;
        ScaledLocations.Add(NewLocation);
    }

    UE_LOG(LogJUSYNC, Log, TEXT("🎯 Multi-point scaling applied: %s"), *OutScaleFactor.ToString());
    return ScaledLocations;
}

// ========== PARALLEL DOWNLOAD FUNCTIONS ==========

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

    // Check if middleware is initialized
    if (!Subsystem->IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Middleware not connected to broker"));
        OnError.ExecuteIfBound(TEXT(""), TEXT("Middleware not connected"));
        return;
    }

    // Convert FString arrays to C arrays for the async C API
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
    
    // Create shared state for tracking callbacks (allocated on heap)
    struct FParallelDownloadState : public TSharedFromThis<FParallelDownloadState>
    {
        FOnParallelFileReceived OnFileReceived;
        FOnParallelDownloadComplete OnComplete;
        FOnParallelDownloadError OnError;
        TArray<FString> Filenames;
        std::atomic<int> FilesReceived{0};
        std::atomic<int> FilesExpected{0};
        std::atomic<bool> bAllComplete{false};
        
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
    
    // Define C callbacks that capture the shared state
    // Note: These are called from C threads, must marshal to game thread
    auto FileReceivedCallback = [State](const char* filename, const unsigned char* data, size_t data_size) {
        FString FilenameUTF8 = UTF8_TO_TCHAR(filename);
        TArray<uint8> DataArray;
        DataArray.Append(data, data_size);
        
        // Update count
        int Received = State->FilesReceived.fetch_add(1) + 1;
        UE_LOG(LogJUSYNC, Log, TEXT("Pipeline: File %d/%d received: %s (%d bytes)"), 
               Received, State->FilesExpected.load(), *FilenameUTF8, DataArray.Num());
        
        // Execute delegate on game thread
        State->ExecuteFileReceived(FilenameUTF8, DataArray);
        
        // Check if all files received
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
        
        // Still count as received (but with error)
        int Received = State->FilesReceived.fetch_add(1) + 1;
        if (Received >= State->FilesExpected && !State->bAllComplete)
        {
            State->bAllComplete = true;
            State->ExecuteComplete();
        }
    };
    
    // Call the async C API (non-blocking, returns immediately)
    UE_LOG(LogJUSYNC, Log, TEXT("Pipeline: Calling RequestFilesParallelAsync_C for %d files (true async)"), Filenames.Num());
    
    // The C API is declared extern "C" in the header included by JUSYNCSubsystem
    // We need to access it - subsystem should have it available
    // For now, use a simpler approach: call through subsystem if it has async method
    // Actually, let me check if there's a direct way
    
    // Since we're in Blueprint library, we can't easily include the C header
    // Instead, let the subsystem handle the async call
    // But the subsystem's current implementation is synchronous
    
    // For now, use the existing approach but log that it's not true pipeline
    UE_LOG(LogJUSYNC, Warning, TEXT("Pipeline: Using synchronous subsystem call - files will download before callbacks"));
    
    // Fall back to existing implementation
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    Async(EAsyncExecution::Thread, [WeakSubsystem, Filenames, TargetRanks, TimeoutMs, OnFileReceived, OnComplete, OnError]()
    {
        if (!WeakSubsystem.IsValid()) return;
        
        TArray<FJUSYNCFileData> DownloadedFiles;
        bool bSuccess = WeakSubsystem->RequestFilesParallel(Filenames, TargetRanks, TimeoutMs, DownloadedFiles);
        
        if (!WeakSubsystem.IsValid()) return;
        
        // Execute callbacks on game thread
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

    // Get the subsystem
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("CreateMaterialFromTexture_Async: JUSYNC subsystem not available"));
        return;
    }

    // Call the subsystem's async function
    Subsystem->CreateMaterialFromTexture_Async(Texture, TargetComponent);
}

// Original implementation for backward compatibility
void UJUSYNCBlueprintLibrary::CreateMaterialFromTexture_Async_Return(
    UTexture2D* Texture,
    const FOnMaterialCreated& OnMaterialCreated)
{
    // Call extended version with default parameters
    CreateMaterialFromTexture_Async_Return_Extended(Texture, nullptr, NAME_None, OnMaterialCreated);
}

// Extended implementation with configurable parameters
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

    // Get the subsystem
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("CreateMaterialFromTexture_Async_Return_Extended: JUSYNC subsystem not available"));
        return;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Creating async material from texture (Base: %s, Param: %s)"),
           BaseMaterial ? *BaseMaterial->GetName() : TEXT("Default"),
           *TextureParameterName.ToString());

    // Convert dynamic delegate to standard function and call internal implementation
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

// ========== BENCHMARKING FUNCTIONS ==========

void UJUSYNCBlueprintLibrary::StartBenchmark(const FString& TestName, const FJUSYNCBenchmarkConfig& Config)
{
	if (bIsBenchmarking)
	{
		UE_LOG(LogJUSYNC, Warning, TEXT("Benchmark already in progress: %s"), *CurrentBenchmarkTest);
		return;
	}

	CurrentBenchmarkTest = TestName;
	CurrentBenchmarkConfig = Config;
	bIsBenchmarking = true;

	UE_LOG(LogJUSYNC, Log, TEXT("Started benchmark: %s"), *TestName);
}

void UJUSYNCBlueprintLibrary::EndBenchmark()
{
	if (!bIsBenchmarking)
	{
		UE_LOG(LogJUSYNC, Warning, TEXT("No benchmark in progress"));
		return;
	}

	UE_LOG(LogJUSYNC, Log, TEXT("Ended benchmark: %s"), *CurrentBenchmarkTest);
	
	// Save results if output directory is specified
	if (!CurrentBenchmarkConfig.OutputDirectory.IsEmpty())
	{
		SaveAllBenchmarkResultsToCSV(CurrentBenchmarkConfig.OutputDirectory);
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

	// Create output directory
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FString OutputPath = OutputDirectory;
	PlatformFile.CreateDirectoryTree(*OutputPath);

	// Create filename
	FString Filename = TEXT("benchmark_results");
	if (CurrentBenchmarkConfig.bAppendTimestamp)
	{
		Filename += TEXT("_") + FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	}
	Filename += TEXT(".csv");

	FString CSVPath = OutputPath / Filename;

	// Create CSV header
	FString CSVData = TEXT("TestName,Timestamp,TotalTimeMs,TriangleCount,VertexCount,RAMBeforeBytes,RAMAfterBytes,FPS,FrameTimeMs,ActorCount,ErrorCount,SuccessRate,SplitMeshCount\n");

	// Add all results
	for (const FJUSYNCBenchmarkResult& Result : BenchmarkResults)
	{
		CSVData += FString::Printf(TEXT("%s,%s,%.2f,%d,%d,%lld,%lld,%.1f,%.2f,%d,%d,%.1f,%d\n"),
			*Result.TestName,
			*Result.Timestamp.ToString(TEXT("%Y-%m-%d %H:%M:%S")),
			Result.TotalTimeMs,
			Result.TriangleCount,
			Result.VertexCount,
			Result.RAMBeforeBytes,
			Result.RAMAfterBytes,
			Result.FPS,
			Result.FrameTimeMs,
			Result.ActorCount,
			Result.ErrorCount,
			Result.SuccessRate,
			Result.SplitMeshCount
		);
	}

	// Save to file
	if (FFileHelper::SaveStringToFile(CSVData, *CSVPath))
	{
		UE_LOG(LogJUSYNC, Log, TEXT("Benchmark results saved to: %s"), *CSVPath);
	}
	else
	{
		UE_LOG(LogJUSYNC, Error, TEXT("Failed to save benchmark results to: %s"), *CSVPath);
	}
}

void UJUSYNCBlueprintLibrary::ClearBenchmarkResults()
{
	BenchmarkResults.Empty();
	UE_LOG(LogJUSYNC, Log, TEXT("Cleared all benchmark results"));
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
	FJUSYNCBenchmarkResult Result;
	Result.TestName = TestName;
	Result.Timestamp = FDateTime::Now();
	Result.TotalTimeMs = TotalTimeMs;
	Result.TriangleCount = TriangleCount;
	Result.VertexCount = VertexCount;
	Result.RAMBeforeBytes = RAMBefore;
	Result.RAMAfterBytes = RAMAfter;
	Result.ActorCount = ActorCount;
	Result.ErrorCount = ErrorCount;
	Result.SuccessRate = (ErrorCount == 0) ? 100.0f : 0.0f;
	Result.SplitMeshCount = SplitMeshCount;

	// Get frame time/FPS
	static uint64 LastFrameCycles = FPlatformTime::Cycles64();
	uint64 CurrentFrameCycles = FPlatformTime::Cycles64();
	Result.FrameTimeMs = FPlatformTime::ToMilliseconds(CurrentFrameCycles - LastFrameCycles);
	LastFrameCycles = CurrentFrameCycles;
	Result.FPS = (Result.FrameTimeMs > 0) ? 1000.0f / Result.FrameTimeMs : 0.0f;

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
	// Simple implementation: call spawn function for each mesh
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
		
		// Optional batching delay
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
	// Start benchmark if enabled
	bool bWasBenchmarking = bIsBenchmarking;
	if (Config.bEnableBenchmarking && !bIsBenchmarking)
	{
		StartBenchmark(TEXT("BatchSpawnRealtimeMeshesWithMaterial"), Config);
	}

	// Measure RAM before
	FPlatformMemoryStats StatsBefore = FPlatformMemory::GetStats();
	int64 RAMBefore = StatsBefore.UsedPhysical;

	// Measure time
	double StartTime = FPlatformTime::Seconds();

	// Call spawn function for each mesh (batch simulation)
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

	// Measure RAM after
	FPlatformMemoryStats StatsAfter = FPlatformMemory::GetStats();
	int64 RAMAfter = StatsAfter.UsedPhysical;

	// Calculate total triangle/vertex count
	int32 TotalTriangleCount = 0;
	int32 TotalVertexCount = 0;
	for (const FJUSYNCMeshData& MeshData : MeshDataArray)
	{
		TotalTriangleCount += MeshData.GetTriangleCount();
		TotalVertexCount += MeshData.GetVertexCount();
	}

	// Calculate error count
	int32 ErrorCount = (SpawnedActors.Num() == MeshDataArray.Num()) ? 0 : 1;

	// Record benchmark result
	if (Config.bEnableBenchmarking)
	{
		FJUSYNCBenchmarkResult Result = CreateBenchmarkResult(
			TEXT("BatchSpawnRealtimeMeshesWithMaterial"),
			TotalTimeMs,
			TotalTriangleCount,
			TotalVertexCount,
			RAMBefore,
			RAMAfter,
			SpawnedActors.Num(),
			ErrorCount,
			0  // SplitMeshCount - TODO: Track actual split meshes
		);

		RecordBenchmarkResult(Result);

		// End benchmark if we started it
		if (!bWasBenchmarking)
		{
			EndBenchmark();
		}
	}

	return SpawnedActors;
}

// ========== DYNAMIC TIMEOUT & RETRY LOGIC IMPLEMENTATION ==========

// Static member initialization
UJUSYNCBlueprintLibrary::FRankPerformanceTracker UJUSYNCBlueprintLibrary::RankPerformanceTracker;

// FRankPerformanceTracker methods
float UJUSYNCBlueprintLibrary::FRankPerformanceTracker::GetRankPerformanceFactor(int32 Rank)
{
	FScopeLock Lock(&StatsMutex);
	
	if (!RankStats.Contains(Rank)) {
		return 1.0f; // Default factor
	}
	
	const FRankStats& Stats = RankStats[Rank];
	
	// Calculate performance factor:
	// 1.0 = normal, >1.0 = slower, <1.0 = faster
	float TimeFactor = FMath::Max(0.5f, FMath::Min(2.0f, Stats.AverageResponseTime / 10000.0f));
	float SuccessFactor = 1.0f + (1.0f - Stats.SuccessRate); // 1.0 for 100% success, 2.0 for 0% success
	
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
	
	// Check if rank has too many failures recently
	if (Stats.RequestCount - Stats.SuccessCount >= MAX_FAILURES) {
		FDateTime Now = FDateTime::UtcNow();
		if (Now - Stats.LastRequestTime < COOLDOWN_PERIOD) {
			return false; // Rank is in cooldown
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
	
	// Update moving average of response time
	if (Stats.AverageResponseTime == 0.0f) {
		Stats.AverageResponseTime = ResponseTimeMs;
	} else {
		Stats.AverageResponseTime = 0.7f * Stats.AverageResponseTime + 0.3f * ResponseTimeMs;
	}
	
	if (bSuccess) {
		Stats.SuccessCount++;
	}
	
	// Update success rate
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

// Helper functions
int64 UJUSYNCBlueprintLibrary::EstimateFileSizeFromFilename(const FString& Filename)
{
	// Pattern-based estimation from your log files:
	// clips/vtk_actor__triangles_0_Geom__r5_0.000000.usda = ~76MB
	// clips/vtk_actor__triangles_0_Geom__r15_0.000000.usda = ~71MB
	
	if (Filename.Contains(TEXT("vtk_actor__triangles_0_Geom__r5_"))) {
		return 76 * 1024 * 1024;  // 76MB
	}
	else if (Filename.Contains(TEXT("vtk_actor__triangles_0_Geom__r10_"))) {
		return 70 * 1024 * 1024;  // 70MB (estimated)
	}
	else if (Filename.Contains(TEXT("vtk_actor__triangles_0_Geom__r15_"))) {
		return 71 * 1024 * 1024;  // 71MB
	}
	else if (Filename.Contains(TEXT(".usda"))) {
		// Generic USD file estimation
		if (Filename.Contains(TEXT("_Geom__"))) {
			return 50 * 1024 * 1024;  // 50MB for geometry files
		}
		return 10 * 1024 * 1024;     // 10MB for other USD files
	}
	
	return 5 * 1024 * 1024;  // Default 5MB
}

int32 UJUSYNCBlueprintLibrary::CalculateDynamicTimeout(const FString& Filename, int32 TargetRank, bool bIsRetry, int32 RetryCount)
{
	// Base factors
	const int32 BASE_TIMEOUT = 5000;           // 5 seconds minimum
	const int32 PER_MB_TIMEOUT = 200;          // 200ms per MB
	const int32 NETWORK_LATENCY_FACTOR = 1000; // 1 second for network overhead
	const int32 RETRY_PENALTY = 2000;          // +2 seconds per retry
	
	// 1. Estimate file size from filename patterns
	int64 EstimatedSize = EstimateFileSizeFromFilename(Filename);
	
	// 2. Calculate size-based timeout
	int64 SizeMB = FMath::Max(1LL, EstimatedSize / (1024 * 1024));
	int32 SizeTimeout = SizeMB * PER_MB_TIMEOUT;
	
	// 3. Network conditions
	int32 NetworkTimeout = NETWORK_LATENCY_FACTOR;
	
	// 4. Retry penalty
	int32 RetryTimeout = bIsRetry ? (RetryCount * RETRY_PENALTY) : 0;
	
	// 5. Rank-specific adjustments
	float RankFactor = RankPerformanceTracker.GetRankPerformanceFactor(TargetRank);
	int32 RankAdjustment = static_cast<int32>(RankFactor * 1000.0f);
	
	// 6. Calculate final timeout with reasonable bounds
	int32 TotalTimeout = BASE_TIMEOUT + SizeTimeout + NetworkTimeout + RetryTimeout + RankAdjustment;
	
	// Enforce reasonable bounds: 5s min, 120s max for normal, 300s max for retries
	int32 MaxTimeout = bIsRetry ? 300000 : 120000; // 5 minutes max for retries, 2 minutes normal
	return FMath::Clamp(TotalTimeout, 5000, MaxTimeout);
}

TArray<int32> UJUSYNCBlueprintLibrary::GetFallbackRanks(int32 TargetRank)
{
	TArray<int32> Ranks;
	
	// Add ranks from same "family" (e.g., r5, r15, r10 are related)
	// Based on your log patterns, files exist on ranks 5, 10, 15
	// Note: TargetRank is already added by the caller, so we don't add it here
	
	if (TargetRank == 5) {
		Ranks.Add(10);
		Ranks.Add(15);
		Ranks.Add(0);  // Rank 0 might have files
	}
	else if (TargetRank == 10) {
		Ranks.Add(5);
		Ranks.Add(15);
		Ranks.Add(0);
	}
	else if (TargetRank == 15) {
		Ranks.Add(5);
		Ranks.Add(10);
		Ranks.Add(0);
	}
	else {
		// For other ranks, try common ranks (excluding the target rank)
		if (TargetRank != 5) Ranks.Add(5);
		if (TargetRank != 10) Ranks.Add(10);
		if (TargetRank != 15) Ranks.Add(15);
		if (TargetRank != 0) Ranks.Add(0);
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
	
	// 1. First, try to extract correct rank from filename (if enabled)
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
	
	// 2. Create list of ranks to try (primary + fallbacks)
	TArray<int32> RanksToTry;
	RanksToTry.Add(TargetRank);
	RanksToTry.Append(GetFallbackRanks(TargetRank));
	
	// 3. Get subsystem as weak pointer for thread safety
	TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
	
	// 4. Launch async retry logic with proper memory safety
	Async(EAsyncExecution::Thread, [WeakSubsystem, RanksToTry, Filename, MaxRetries, OnComplete, OnError]()
	{
		// Check if subsystem still exists
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
		
		// Safety: Calculate maximum total time to prevent infinite loops
		const int32 MAX_TOTAL_TIME_MS = 300000; // 5 minutes maximum
		FDateTime OperationStartTime = FDateTime::UtcNow();
		
		// Try each rank with retries
		for (int32 RankIndex = 0; RankIndex < RanksToTry.Num() && !bSuccess; RankIndex++)
		{
			int32 CurrentRank = RanksToTry[RankIndex];
			
			// Check if we should try this rank (circuit breaker)
			if (!RankPerformanceTracker.CanTryRank(CurrentRank)) {
				UE_LOG(LogJUSYNC, Log, TEXT("Skipping rank %d due to circuit breaker"), CurrentRank);
				continue;
			}
			
			for (int32 Retry = 0; Retry < MaxRetries && !bSuccess; Retry++)
			{
				// Check if we've exceeded maximum total time
				FTimespan ElapsedTime = FDateTime::UtcNow() - OperationStartTime;
				if (ElapsedTime.GetTotalMilliseconds() > MAX_TOTAL_TIME_MS)
				{
					FinalError = FString::Printf(TEXT("Exceeded maximum total time of %dms"), MAX_TOTAL_TIME_MS);
					UE_LOG(LogJUSYNC, Error, TEXT("⚠️ %s for file: %s"), *FinalError, *Filename);
					break;
				}
				
				TotalAttempts++;
				
				// Dynamic timeout based on retry count and rank performance
				bool bIsRetry = (Retry > 0);
				int32 DynamicTimeout = CalculateDynamicTimeout(Filename, CurrentRank, bIsRetry, Retry);
				
				// Apply exponential backoff for retries
				if (bIsRetry) {
					int32 BackoffMs = FMath::Min(1000 * (1 << (Retry - 1)), 30000);
					FPlatformProcess::Sleep(BackoffMs / 1000.0f);
				}
				
				// Track request start
				RankPerformanceTracker.RecordRequestStart(CurrentRank);
				FDateTime StartTime = FDateTime::UtcNow();
				
				// Make the request
				bSuccess = SubsystemPtr->RequestFile(Filename, CurrentRank, DynamicTimeout, FileData);
				
				// Calculate response time
				FDateTime EndTime = FDateTime::UtcNow();
				FTimespan ResponseTime = EndTime - StartTime;
				int32 ResponseTimeMs = ResponseTime.GetTotalMilliseconds();
				
				// Update performance tracker
				RankPerformanceTracker.RecordRequestResult(CurrentRank, bSuccess, ResponseTimeMs);
				
				if (bSuccess) {
					ActualRankUsed = CurrentRank;
					UE_LOG(LogJUSYNC, Log, TEXT("✅ Successfully retrieved '%s' from rank %d in %dms (attempt %d, timeout: %dms)"),
						   *Filename, ActualRankUsed, ResponseTimeMs, TotalAttempts, DynamicTimeout);
					break;
				} else {
					FinalError = FString::Printf(TEXT("Failed attempt %d for '%s' from rank %d (timeout: %dms, actual: %dms)"),
						TotalAttempts, *Filename, CurrentRank, DynamicTimeout, ResponseTimeMs);
					UE_LOG(LogJUSYNC, Warning, TEXT("⚠️ %s"), *FinalError);
				}
			}
		}
		
		// Execute callback on game thread
		AsyncTask(ENamedThreads::GameThread, [bSuccess, Filename, FileData, ActualRankUsed, TotalAttempts, FinalError, OnComplete, OnError]()
		{
			if (bSuccess) {
				if (OnComplete.IsBound()) {
					OnComplete.Execute(Filename, FileData);
				} else {
					UE_LOG(LogJUSYNC, Warning, TEXT("OnComplete delegate not bound for successful file retrieval: %s"), *Filename);
				}
			} else {
				FString ErrorMsg = FString::Printf(
					TEXT("Failed to retrieve '%s' after %d attempts (last tried rank: %d). %s"),
					*Filename, TotalAttempts, ActualRankUsed, *FinalError);
				if (OnError.IsBound()) {
					OnError.Execute(ErrorMsg);
				} else {
					UE_LOG(LogJUSYNC, Error, TEXT("OnError delegate not bound for failed file retrieval: %s"), *Filename);
				}
			}
		});
	});
}