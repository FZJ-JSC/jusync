#include "JUSYNCAsyncActions.h"
#include "JUSYNCBlueprintLibrary.h"
#include "JUSYNCSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "RealtimeMeshComponent.h"
#include "Engine/GameInstance.h"
#include "TimerManager.h"  

// ========== USD LOADING ASYNC NODE IMPLEMENTATION ==========
UJUSYNCAsyncLoadUSD* UJUSYNCAsyncLoadUSD::AsyncLoadUSDFromBuffer(
    const TArray<uint8>& Buffer, const FString& Filename)
{
    UJUSYNCAsyncLoadUSD* Action = NewObject<UJUSYNCAsyncLoadUSD>();
    Action->BufferData = Buffer;
    Action->FilenameData = Filename;
    Action->bIsFromDisk = false;
    return Action;
}

UJUSYNCAsyncLoadUSD* UJUSYNCAsyncLoadUSD::AsyncLoadUSDFromDisk(const FString& FilePath)
{
    UJUSYNCAsyncLoadUSD* Action = NewObject<UJUSYNCAsyncLoadUSD>();
    Action->FilePathData = FilePath;
    Action->bIsFromDisk = true;
    return Action;
}

void UJUSYNCAsyncLoadUSD::Activate()
{
    // Validate inputs first
    if (bIsFromDisk && FilePathData.IsEmpty())
    {
        OnFailure.Broadcast(TArray<FJUSYNCMeshData>(), false);
        SetReadyToDestroy();
        return;
    }
    
    if (!bIsFromDisk && BufferData.Num() == 0)
    {
        OnFailure.Broadcast(TArray<FJUSYNCMeshData>(), false);
        SetReadyToDestroy();
        return;
    }

    // Perform async loading on background thread
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]()
    {
        TArray<FJUSYNCMeshData> MeshData;
        FString Preview;
        bool bSuccess = false;

        // Broadcast initial progress
        AsyncTask(ENamedThreads::GameThread, [this]()
        {
            BroadcastProgress(0.1f);
        });

        if (bIsFromDisk)
        {
            // Load from disk using your existing function
            bSuccess = UJUSYNCBlueprintLibrary::LoadUSDFromDisk(FilePathData, MeshData, Preview);
        }
        else
        {
            // Load from buffer using your existing function
            bSuccess = UJUSYNCBlueprintLibrary::LoadUSDFromBuffer(BufferData, FilenameData, MeshData, Preview);
        }

        // Return to game thread for Blueprint callback
        AsyncTask(ENamedThreads::GameThread, [this, bSuccess, MeshData]()
        {
            BroadcastProgress(1.0f);
            OnLoadComplete(bSuccess, MeshData);
        });
    });
}

void UJUSYNCAsyncLoadUSD::OnLoadComplete(bool bSuccess, const TArray<FJUSYNCMeshData>& MeshData)
{
    if (bSuccess)
    {
        OnSuccess.Broadcast(MeshData, true);
    }
    else
    {
        OnFailure.Broadcast(TArray<FJUSYNCMeshData>(), false);
    }
    
    SetReadyToDestroy();
}

void UJUSYNCAsyncLoadUSD::BroadcastProgress(float Progress)
{
    OnProgress.Broadcast(Progress);
}

// ========== FILE RECEPTION ASYNC NODE IMPLEMENTATION ==========
UJUSYNCAsyncReceiveFiles* UJUSYNCAsyncReceiveFiles::AsyncStartReceiving()
{
    UJUSYNCAsyncReceiveFiles* Action = NewObject<UJUSYNCAsyncReceiveFiles>();
    return Action;
}

// Enhanced GetWorld implementation with more fallbacks
UWorld* UJUSYNCAsyncReceiveFiles::GetWorld() const
{
    // Method 1: Use GWorld global (most reliable for PIE)
    if (GWorld)
    {
        UE_LOG(LogTemp, Log, TEXT("JUSYNC Async: Got world from GWorld: %s"), *GWorld->GetName());
        return GWorld;
    }
    
    // Method 2: Try to get world from JUSYNC subsystem
    if (UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
    {
        if (UGameInstance* GameInstance = Subsystem->GetGameInstance())
        {
            UWorld* World = GameInstance->GetWorld();
            if (World)
            {
                UE_LOG(LogTemp, Log, TEXT("JUSYNC Async: Got world from subsystem: %s"), *World->GetName());
                return World;
            }
        }
    }
    
    // Method 3: Iterate through world contexts (comprehensive fallback)
    if (GEngine)
    {
        // First try PIE worlds
        for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
        {
            if (WorldContext.World() && WorldContext.WorldType == EWorldType::PIE)
            {
                UE_LOG(LogTemp, Log, TEXT("JUSYNC Async: Got PIE world: %s"), *WorldContext.World()->GetName());
                return WorldContext.World();
            }
        }
        
        // Then try game worlds
        for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
        {
            if (WorldContext.World() && WorldContext.WorldType == EWorldType::Game)
            {
                UE_LOG(LogTemp, Log, TEXT("JUSYNC Async: Got game world: %s"), *WorldContext.World()->GetName());
                return WorldContext.World();
            }
        }
    }
    
    UE_LOG(LogTemp, Error, TEXT("JUSYNC Async: No valid world context found"));
    return nullptr;
}


void UJUSYNCAsyncReceiveFiles::Activate()
{
    UE_LOG(LogTemp, Log, TEXT("JUSYNC Async: Activate() called"));
    
    // Get world context first
    UWorld* World = GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to get world context for JUSYNC async receiving"));
        SetReadyToDestroy();
        return;
    }
    
    UE_LOG(LogTemp, Log, TEXT("JUSYNC Async: Got world context: %s"), *World->GetName());
    
    // Start receiving using your existing function
    bool bStarted = UJUSYNCBlueprintLibrary::StartJUSYNCReceiving();
    
    if (!bStarted)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to start JUSYNC receiving - check middleware initialization"));
        SetReadyToDestroy();
        return;
    }
    
    UE_LOG(LogTemp, Log, TEXT("JUSYNC receiving started successfully"));
    
    // Set up timer to check for files
    World->GetTimerManager().SetTimer(CheckTimer, this, 
        &UJUSYNCAsyncReceiveFiles::CheckForFiles, 0.1f, true);
        
    UE_LOG(LogTemp, Log, TEXT("JUSYNC Async: Timer setup complete"));
}


void UJUSYNCAsyncReceiveFiles::CheckForFiles()
{
    // Use your existing function to check for received files
    TArray<FJUSYNCFileData> ReceivedFiles;
    if (UJUSYNCBlueprintLibrary::CheckForReceivedFiles(ReceivedFiles))
    {
        UE_LOG(LogTemp, Log, TEXT("JUSYNC Async: Found %d files"), ReceivedFiles.Num());
        for (const FJUSYNCFileData& FileData : ReceivedFiles)
        {
            OnFileReceived.Broadcast(FileData, true);
        }
        // Clear received data using your existing function
        UJUSYNCBlueprintLibrary::ClearReceivedData();
    }
}

// ========== TEXTURE PROCESSING ASYNC NODE IMPLEMENTATION ==========
UJUSYNCAsyncCreateTexture* UJUSYNCAsyncCreateTexture::AsyncCreateTextureFromBuffer(const TArray<uint8>& Buffer)
{
    UJUSYNCAsyncCreateTexture* Action = NewObject<UJUSYNCAsyncCreateTexture>();
    Action->BufferData = Buffer;
    return Action;
}

void UJUSYNCAsyncCreateTexture::Activate()
{
    if (BufferData.Num() == 0)
    {
        OnFailure.Broadcast(FJUSYNCTextureData(), false);
        SetReadyToDestroy();
        return;
    }

    // Perform async texture creation on background thread
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]()
    {
        // Use your existing function
        FJUSYNCTextureData TextureData = UJUSYNCBlueprintLibrary::CreateTextureFromBuffer(BufferData);
        bool bSuccess = TextureData.IsValid();

        // Return to game thread for Blueprint callback
        AsyncTask(ENamedThreads::GameThread, [this, bSuccess, TextureData]()
        {
            OnTextureComplete(bSuccess, TextureData);
        });
    });
}

void UJUSYNCAsyncCreateTexture::OnTextureComplete(bool bSuccess, const FJUSYNCTextureData& TextureData)
{
    if (bSuccess)
    {
        OnSuccess.Broadcast(TextureData, true);
    }
    else
    {
        OnFailure.Broadcast(FJUSYNCTextureData(), false);
    }
    
    SetReadyToDestroy();
}

// ========== REALTIMEMESH CREATION ASYNC NODE IMPLEMENTATION ==========
UJUSYNCAsyncCreateMesh* UJUSYNCAsyncCreateMesh::AsyncCreateRealtimeMeshFromJUSYNC(
    const FJUSYNCMeshData& MeshData, URealtimeMeshComponent* RealtimeMeshComponent)
{
    UJUSYNCAsyncCreateMesh* Action = NewObject<UJUSYNCAsyncCreateMesh>();
    Action->MeshDataCopy = MeshData;
    Action->MeshComponentPtr = RealtimeMeshComponent;
    return Action;
}

void UJUSYNCAsyncCreateMesh::Activate()
{
    if (!MeshDataCopy.IsValid() || !MeshComponentPtr.IsValid())
    {
        OnFailure.Broadcast(nullptr, false);
        SetReadyToDestroy();
        return;
    }

    // Perform async mesh creation on background thread
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]()
    {
        // Use your existing function
        bool bSuccess = UJUSYNCBlueprintLibrary::CreateRealtimeMeshFromJUSYNC(
            MeshDataCopy, MeshComponentPtr.Get());

        // Return to game thread for Blueprint callback
        AsyncTask(ENamedThreads::GameThread, [this, bSuccess]()
        {
            OnMeshComplete(bSuccess, MeshComponentPtr.Get());
        });
    });
}

void UJUSYNCAsyncCreateMesh::OnMeshComplete(bool bSuccess, URealtimeMeshComponent* Component)
{
    if (bSuccess && Component)
    {
        OnSuccess.Broadcast(Component, true);
    }
    else
    {
        OnFailure.Broadcast(nullptr, false);
    }
    
    SetReadyToDestroy();
}
