#include "JUSYNCAsyncActions.h"
#include "JUSYNCBlueprintLibrary.h"
#include "JUSYNCModule.h"
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
    
    // Try to register with game instance, but don't fail if we can't find a world
    // The Activate() method will try again with more comprehensive world detection
    UWorld* World = nullptr;
    
    // Method 1: Use GWorld global
    if (GWorld)
    {
        World = GWorld;
    }
    
    // Method 2: Try engine world contexts
    if (!World && GEngine)
    {
        for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
        {
            if (WorldContext.World() && (WorldContext.WorldType == EWorldType::PIE || WorldContext.WorldType == EWorldType::Game))
            {
                World = WorldContext.World();
                break;
            }
        }
    }
    
    if (World)
    {
        if (UGameInstance* GameInstance = World->GetGameInstance())
        {
            Action->RegisterWithGameInstance(GameInstance);
            UE_LOG(LogJUSYNC, Log, TEXT("AsyncLoadUSDFromBuffer: Registered with game instance"));
        }
        else
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("AsyncLoadUSDFromBuffer: Found world but no game instance"));
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("AsyncLoadUSDFromBuffer: Could not find world context for registration"));
        // Don't fail - Activate() will try again
    }
    
    return Action;
}

UJUSYNCAsyncLoadUSD* UJUSYNCAsyncLoadUSD::AsyncLoadUSDFromDisk(const FString& FilePath)
{
    UJUSYNCAsyncLoadUSD* Action = NewObject<UJUSYNCAsyncLoadUSD>();
    Action->FilePathData = FilePath;
    Action->bIsFromDisk = true;
    
    // Try to register with game instance, but don't fail if we can't find a world
    // The Activate() method will try again with more comprehensive world detection
    UWorld* World = nullptr;
    
    // Method 1: Use GWorld global
    if (GWorld)
    {
        World = GWorld;
    }
    
    // Method 2: Try engine world contexts
    if (!World && GEngine)
    {
        for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
        {
            if (WorldContext.World() && (WorldContext.WorldType == EWorldType::PIE || WorldContext.WorldType == EWorldType::Game))
            {
                World = WorldContext.World();
                break;
            }
        }
    }
    
    if (World)
    {
        if (UGameInstance* GameInstance = World->GetGameInstance())
        {
            Action->RegisterWithGameInstance(GameInstance);
            UE_LOG(LogJUSYNC, Log, TEXT("AsyncLoadUSDFromDisk: Registered with game instance"));
        }
        else
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("AsyncLoadUSDFromDisk: Found world but no game instance"));
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("AsyncLoadUSDFromDisk: Could not find world context for registration"));
        // Don't fail - Activate() will try again
    }
    
    return Action;
}

void UJUSYNCAsyncLoadUSD::Activate()
{
    // Enhanced world detection similar to UJUSYNCAsyncReceiveFiles
    UWorld* World = nullptr;
    
    // Method 1: Use GWorld global (most reliable for PIE)
    if (GWorld)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("AsyncLoadUSD: Got world from GWorld: %s"), *GWorld->GetName());
        World = GWorld;
    }
    
    // Method 2: Try to get world from JUSYNC subsystem
    if (!World)
    {
        if (UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
        {
            if (UGameInstance* GameInstance = Subsystem->GetGameInstance())
            {
                World = GameInstance->GetWorld();
                if (World)
                {
                    UE_LOG(LogJUSYNC, Log, TEXT("AsyncLoadUSD: Got world from subsystem: %s"), *World->GetName());
                }
            }
        }
    }
    
    // Method 3: Iterate through world contexts (comprehensive fallback)
    if (!World && GEngine)
    {
        // First try PIE worlds
        for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
        {
            if (WorldContext.World() && WorldContext.WorldType == EWorldType::PIE)
            {
                World = WorldContext.World();
                UE_LOG(LogJUSYNC, Log, TEXT("AsyncLoadUSD: Got PIE world: %s"), *World->GetName());
                break;
            }
        }
        
        // Then try game worlds
        if (!World)
        {
            for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
            {
                if (WorldContext.World() && WorldContext.WorldType == EWorldType::Game)
                {
                    World = WorldContext.World();
                    UE_LOG(LogJUSYNC, Log, TEXT("AsyncLoadUSD: Got game world: %s"), *World->GetName());
                    break;
                }
            }
        }
    }
    
    // Register with game instance if we found a world
    if (World)
    {
        if (UGameInstance* GameInstance = World->GetGameInstance())
        {
            RegisterWithGameInstance(GameInstance);
            UE_LOG(LogJUSYNC, Log, TEXT("AsyncLoadUSD: Registered with game instance in Activate()"));
        }
        else
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("AsyncLoadUSD: Could not get game instance from world"));
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("AsyncLoadUSD: Could not find any valid world context"));
    }
    
    // Validate inputs first
    if (bIsFromDisk && FilePathData.IsEmpty())
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("AsyncLoadUSD: Empty file path for disk load"));
        OnFailure.Broadcast(TArray<FJUSYNCMeshData>(), false);
        SetReadyToDestroy();
        return;
    }
    
    if (!bIsFromDisk && BufferData.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("AsyncLoadUSD: Empty buffer for memory load"));
        OnFailure.Broadcast(TArray<FJUSYNCMeshData>(), false);
        SetReadyToDestroy();
        return;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("AsyncLoadUSD: Starting async load (from disk: %d)"), bIsFromDisk);
    
    // Perform async loading on background thread with memory safety
    TWeakObjectPtr<UJUSYNCAsyncLoadUSD> WeakThis(this);
    bool bIsFromDiskCopy = bIsFromDisk;
    FString FilePathDataCopy = FilePathData;
    TArray<uint8> BufferDataCopy = BufferData;
    FString FilenameDataCopy = FilenameData;
    
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis, bIsFromDiskCopy, FilePathDataCopy, BufferDataCopy, FilenameDataCopy]()
    {
        TArray<FJUSYNCMeshData> MeshData;
        FString Preview;
        bool bSuccess = false;

        // Broadcast initial progress (check if object still exists)
        AsyncTask(ENamedThreads::GameThread, [WeakThis]()
        {
            if (UJUSYNCAsyncLoadUSD* StrongThis = WeakThis.Get())
            {
                StrongThis->BroadcastProgress(0.1f);
            }
        });

        if (bIsFromDiskCopy)
        {
            // Load from disk using your existing function
            bSuccess = UJUSYNCBlueprintLibrary::LoadUSDFromDisk(FilePathDataCopy, MeshData, Preview);
        }
        else
        {
            // Load from buffer using your existing function
            bSuccess = UJUSYNCBlueprintLibrary::LoadUSDFromBuffer(BufferDataCopy, FilenameDataCopy, MeshData, Preview);
        }

        // Return to game thread for Blueprint callback (check if object still exists)
        AsyncTask(ENamedThreads::GameThread, [WeakThis, bSuccess, MeshData]()
        {
            if (UJUSYNCAsyncLoadUSD* StrongThis = WeakThis.Get())
            {
                StrongThis->BroadcastProgress(1.0f);
                StrongThis->OnLoadComplete(bSuccess, MeshData);
            }
            else
            {
                UE_LOG(LogJUSYNC, Warning, TEXT("AsyncLoadUSD: Object destroyed before completion"));
            }
        });
    });
}

void UJUSYNCAsyncLoadUSD::OnLoadComplete(bool bSuccess, const TArray<FJUSYNCMeshData>& MeshData)
{
    UE_LOG(LogJUSYNC, Log, TEXT("AsyncLoadUSD::OnLoadComplete: bSuccess=%d, MeshCount=%d, Object=%p"), 
           bSuccess, MeshData.Num(), this);
    
    if (bSuccess)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("AsyncLoadUSD: Broadcasting OnSuccess to Blueprint (MeshCount=%d)"), MeshData.Num());
        OnSuccess.Broadcast(MeshData, true);
    }
    else
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("AsyncLoadUSD: Broadcasting OnFailure"));
        OnFailure.Broadcast(TArray<FJUSYNCMeshData>(), false);
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("AsyncLoadUSD: Setting ready to destroy"));
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
    
    // CRITICAL: Register with game instance to prevent garbage collection
    if (UWorld* World = GEngine->GetWorldFromContextObject(Action, EGetWorldErrorMode::LogAndReturnNull))
    {
        Action->RegisterWithGameInstance(World->GetGameInstance());
    }
    else
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("AsyncStartReceiving: Could not get world context for registration"));
    }
    
    return Action;
}

// Enhanced GetWorld implementation with more fallbacks
UWorld* UJUSYNCAsyncReceiveFiles::GetWorld() const
{
    // Method 1: Use GWorld global (most reliable for PIE)
    if (GWorld)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Async: Got world from GWorld: %s"), *GWorld->GetName());
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
                UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Async: Got world from subsystem: %s"), *World->GetName());
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
                UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Async: Got PIE world: %s"), *WorldContext.World()->GetName());
                return WorldContext.World();
            }
        }
        
        // Then try game worlds
        for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
        {
            if (WorldContext.World() && WorldContext.WorldType == EWorldType::Game)
            {
                UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Async: Got game world: %s"), *WorldContext.World()->GetName());
                return WorldContext.World();
            }
        }
    }
    
    UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Async: No valid world context found"));
    return nullptr;
}


void UJUSYNCAsyncReceiveFiles::Activate()
{
    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Async: Activate() called"));
    
    // Get world context first
    UWorld* World = GetWorld();
    if (!World)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to get world context for JUSYNC async receiving"));
        SetReadyToDestroy();
        return;
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Async: Got world context: %s"), *World->GetName());
    
    // Start receiving using your existing function
    bool bStarted = UJUSYNCBlueprintLibrary::StartJUSYNCReceiving();
    
    if (!bStarted)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to start JUSYNC receiving - check middleware initialization"));
        SetReadyToDestroy();
        return;
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC receiving started successfully"));
    
    // Set up timer to check for files
    World->GetTimerManager().SetTimer(CheckTimer, this, 
        &UJUSYNCAsyncReceiveFiles::CheckForFiles, 0.1f, true);
        
    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Async: Timer setup complete"));
}


void UJUSYNCAsyncReceiveFiles::CheckForFiles()
{
    // Use your existing function to check for received files
    TArray<FJUSYNCFileData> ReceivedFiles;
    if (UJUSYNCBlueprintLibrary::CheckForReceivedFiles(ReceivedFiles))
    {
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Async: Found %d files"), ReceivedFiles.Num());
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
    
    // CRITICAL: Register with game instance to prevent garbage collection
    if (UWorld* World = GEngine->GetWorldFromContextObject(Action, EGetWorldErrorMode::LogAndReturnNull))
    {
        Action->RegisterWithGameInstance(World->GetGameInstance());
    }
    else
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("AsyncCreateTextureFromBuffer: Could not get world context for registration"));
    }
    
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
    
    // CRITICAL: Register with game instance to prevent garbage collection
    if (UWorld* World = GEngine->GetWorldFromContextObject(Action, EGetWorldErrorMode::LogAndReturnNull))
    {
        Action->RegisterWithGameInstance(World->GetGameInstance());
    }
    else
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("AsyncCreateRealtimeMeshFromJUSYNC: Could not get world context for registration"));
    }
    
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
