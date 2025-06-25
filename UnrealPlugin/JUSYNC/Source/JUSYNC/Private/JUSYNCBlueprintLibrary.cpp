#include "JUSYNCBlueprintLibrary.h"
#include "JUSYNCSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "RealtimeMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"  

// Static member initialization
TArray<FJUSYNCFileData> UJUSYNCBlueprintLibrary::ReceivedFiles;
TArray<FString> UJUSYNCBlueprintLibrary::ReceivedMessages;
FCriticalSection UJUSYNCBlueprintLibrary::DataMutex;

// ========== CONNECTION MANAGEMENT ==========

bool UJUSYNCBlueprintLibrary::InitializeJUSYNCMiddleware(const FString& Endpoint)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->InitializeMiddleware(Endpoint);
    if (bResult)
    {
        UE_LOG(LogTemp, Log, TEXT("JUSYNC Middleware initialized successfully"));
        DisplayDebugMessage(TEXT("JUSYNC Middleware Connected"), 3.0f, FLinearColor::Green);
    }
    else
    {
        DisplayDebugMessage(TEXT("JUSYNC Middleware Failed to Connect"), 5.0f, FLinearColor::Red);
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
        DisplayDebugMessage(TEXT("JUSYNC Middleware Disconnected"), 3.0f, FLinearColor::Yellow);
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
        DisplayDebugMessage(TEXT("JUSYNC Started Receiving Data"), 3.0f, FLinearColor::Blue);
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
        DisplayDebugMessage(TEXT("JUSYNC Stopped Receiving Data"), 3.0f, FLinearColor(1.0f, 0.5f, 0.0f, 1.0f));
    }
}

// ========== USD PROCESSING WITH PREVIEW ==========

bool UJUSYNCBlueprintLibrary::LoadUSDFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCMeshData>& OutMeshData, FString& OutPreview)
{
    if (!ValidateBufferSize(Buffer, TEXT("LoadUSDFromBuffer")))
    {
        return false;
    }

    // Generate preview first
    OutPreview = GetUSDAPreview(Buffer, 15);

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Subsystem not available for USD loading"));
        return false;
    }

    bool bResult = Subsystem->LoadUSDFromBuffer(Buffer, Filename, OutMeshData);

    if (bResult)
    {
        FString Message = FString::Printf(TEXT("Loaded USD: %s (%d meshes)"), *Filename, OutMeshData.Num());
        DisplayDebugMessage(Message, 5.0f, FLinearColor::Green);
        UE_LOG(LogTemp, Log, TEXT("USD Preview:\n%s"), *OutPreview);
    }
    else
    {
        DisplayDebugMessage(TEXT("Failed to load USD file"), 5.0f, FLinearColor::Red);
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

    // Check file extension
    FString Extension = FPaths::GetExtension(Filename).ToLower();
    if (Extension != TEXT("usd") && Extension != TEXT("usda") &&
        Extension != TEXT("usdc") && Extension != TEXT("usdz"))
    {
        return false;
    }

    // Check content for USD markers
    FString Content = ExtractUSDAPreview(Buffer, 5);
    return Content.Contains(TEXT("#usda")) ||
           Content.Contains(TEXT("PXR-USDC")) ||
           Content.Contains(TEXT("def ")) ||
           Content.Contains(TEXT("over "));
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
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Subsystem not available for texture creation"));
        return EmptyTexture;
    }

    FJUSYNCTextureData Result = Subsystem->CreateTextureFromBuffer(Buffer);

    if (Result.IsValid())
    {
        FString Message = FString::Printf(TEXT("Created Texture: %dx%d (%d channels)"),
                                        Result.Width, Result.Height, Result.Channels);
        // Use FLinearColor constructor for Cyan color
        DisplayDebugMessage(Message, 3.0f, FLinearColor(0.0f, 1.0f, 1.0f, 1.0f));
    }
    else
    {
        DisplayDebugMessage(TEXT("Failed to create texture from buffer"), 3.0f, FLinearColor::Red);
    }

    return Result;
}

UTexture2D* UJUSYNCBlueprintLibrary::CreateUETextureFromJUSYNC(const FJUSYNCTextureData& TextureData)
{
    if (!TextureData.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("Invalid texture data provided"));
        return nullptr;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Subsystem not available"));
        return nullptr;
    }

    UTexture2D* Result = Subsystem->CreateUETextureFromJUSYNC(TextureData);

    if (Result)
    {
        // Use FLinearColor constructor for Cyan color
        DisplayDebugMessage(TEXT("UE Texture2D Created Successfully"), 3.0f, FLinearColor(0.0f, 1.0f, 1.0f, 1.0f));
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
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->WriteGradientLineAsPNG(Buffer, OutputPath);

    if (bResult)
    {
        FString Message = FString::Printf(TEXT("Gradient PNG saved: %s"), *OutputPath);
        DisplayDebugMessage(Message, 3.0f, FLinearColor::Green);
    }
    else
    {
        DisplayDebugMessage(TEXT("Failed to save gradient PNG"), 3.0f, FLinearColor::Red);
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
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->GetGradientLineAsPNGBuffer(Buffer, OutPNGBuffer);

    if (bResult)
    {
        FString Message = FString::Printf(TEXT("Gradient PNG buffer created: %d bytes"), OutPNGBuffer.Num());
        DisplayDebugMessage(Message, 3.0f, FLinearColor::Green);
    }
    else
    {
        DisplayDebugMessage(TEXT("Failed to create gradient PNG buffer"), 3.0f, FLinearColor::Red);
    }

    return bResult;
}

// ========== REALTIMEMESH PROCESSING ==========

bool UJUSYNCBlueprintLibrary::CreateRealtimeMeshFromJUSYNC(const FJUSYNCMeshData& MeshData, URealtimeMeshComponent* RealtimeMeshComponent)
{
    if (!MeshData.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("Invalid mesh data provided"));
        return false;
    }

    if (!RealtimeMeshComponent)
    {
        UE_LOG(LogTemp, Error, TEXT("RealtimeMeshComponent is null"));
        return false;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Subsystem not available"));
        return false;
    }

    bool bResult = Subsystem->CreateRealtimeMeshFromJUSYNC(MeshData, RealtimeMeshComponent);

    if (bResult)
    {
        FString Message = FString::Printf(TEXT("RealtimeMesh Created: %s (%d verts, %d tris)"),
                                        *MeshData.ElementName, MeshData.GetVertexCount(), MeshData.GetTriangleCount());
        // Use FLinearColor constructor for Cyan color
        DisplayDebugMessage(Message, 5.0f, FLinearColor(0.0f, 1.0f, 1.0f, 1.0f));
    }

    return bResult;
}

bool UJUSYNCBlueprintLibrary::BatchCreateRealtimeMeshesFromJUSYNC(const TArray<FJUSYNCMeshData>& MeshDataArray, const TArray<URealtimeMeshComponent*>& MeshComponents)
{
    if (MeshDataArray.Num() != MeshComponents.Num())
    {
        UE_LOG(LogTemp, Error, TEXT("Mesh data array and component array size mismatch"));
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
            UE_LOG(LogTemp, Warning, TEXT("Failed to create RealtimeMesh %d: %s"), i, *MeshDataArray[i].ElementName);
            bAllSuccessful = false;
        }
    }

    FString Message = FString::Printf(TEXT("Batch RealtimeMesh Creation: %d/%d successful"), SuccessCount, MeshDataArray.Num());
    // Use FLinearColor constructor for Cyan or Yellow color
    FLinearColor Color = bAllSuccessful ? FLinearColor(0.0f, 1.0f, 1.0f, 1.0f) : FLinearColor::Yellow;
    DisplayDebugMessage(Message, 5.0f, Color);

    return bAllSuccessful;
}

FJUSYNCRealtimeMeshData UJUSYNCBlueprintLibrary::ConvertToRealtimeMeshFormat(const FJUSYNCMeshData& StandardMesh)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Subsystem not available"));
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
            UE_LOG(LogTemp, Log, TEXT("%s"), *Message);
            DisplayDebugMessage(Message, 5.0f, FLinearColor::Blue);
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
            UE_LOG(LogTemp, Log, TEXT("Received Message: %s"), *Message);
            // Use FLinearColor constructor for Cyan color
            DisplayDebugMessage(FString::Printf(TEXT("Message: %s"), *Message), 3.0f, FLinearColor(0.0f, 1.0f, 1.0f, 1.0f));
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
    UE_LOG(LogTemp, Log, TEXT("JUSYNC received data cleared"));
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
        DisplayDebugMessage(Message, 3.0f, FLinearColor::Green);
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
        DisplayDebugMessage(Message, 3.0f, FLinearColor::Green);
    }

    return bResult;
}

// ========== DEBUG & DISPLAY ==========

void UJUSYNCBlueprintLibrary::DisplayDebugMessage(const FString& Message, float Duration, FLinearColor Color)
{
    if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(-1, Duration, Color.ToFColor(true), FString::Printf(TEXT("JUSYNC: %s"), *Message));
    }

    UE_LOG(LogTemp, Log, TEXT("JUSYNC: %s"), *Message);
}

void UJUSYNCBlueprintLibrary::LogJUSYNCMessage(const FString& Message, bool bIsError)
{
    if (bIsError)
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC: %s"), *Message);
        DisplayDebugMessage(Message, 5.0f, FLinearColor::Red);
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("JUSYNC: %s"), *Message);
        DisplayDebugMessage(Message, 3.0f, FLinearColor::White);
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
    
    UE_LOG(LogTemp, Warning, TEXT("JUSYNC Subsystem not found in any world context"));
    return nullptr;
}

// ========== PRIVATE HELPERS ==========

bool UJUSYNCBlueprintLibrary::ValidateBufferSize(const TArray<uint8>& Buffer, const FString& Context)
{
    if (Buffer.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("%s: Buffer is empty"), *Context);
        return false;
    }

    // Use your middleware's safety constants
    const size_t MaxSize = 1024 * 1024 * 1024; // 1GB limit from your middleware
    if (Buffer.Num() > MaxSize)
    {
        UE_LOG(LogTemp, Error, TEXT("%s: Buffer too large (%d bytes, max: %llu)"), *Context, Buffer.Num(), MaxSize);
        return false;
    }

    return true;
}

bool UJUSYNCBlueprintLibrary::ValidateFilePath(const FString& FilePath, const FString& Context)
{
    if (FilePath.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("%s: File path is empty"), *Context);
        return false;
    }

    if (FilePath.Len() > 1000)
    {
        UE_LOG(LogTemp, Error, TEXT("%s: File path too long"), *Context);
        return false;
    }

    // Check for dangerous path patterns
    if (FilePath.Contains(TEXT("..")) || FilePath.Contains(TEXT("~/")))
    {
        UE_LOG(LogTemp, Error, TEXT("%s: Unsafe file path detected: %s"), *Context, *FilePath);
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

    // Convert buffer to string safely
    FString Content;
    const int32 PreviewSize = FMath::Min(Buffer.Num(), 4096); // Limit preview to first 4KB

    // Convert bytes to string with safety checks
    for (int32 i = 0; i < PreviewSize; ++i)
    {
        char Char = static_cast<char>(Buffer[i]);
        if (Char >= 32 && Char <= 126) // Printable ASCII
        {
            Content.AppendChar(Char);
        }
        else if (Char == '\n' || Char == '\r' || Char == '\t')
        {
            Content.AppendChar(Char);
        }
        else
        {
            Content.AppendChar('?'); // Replace non-printable with ?
        }
    }

    // Extract first N lines
    TArray<FString> Lines;
    Content.ParseIntoArrayLines(Lines);

    FString Preview = TEXT("=== USD PREVIEW ===\n");
    int32 LinesToShow = FMath::Min(MaxLines, Lines.Num());

    for (int32 i = 0; i < LinesToShow; ++i)
    {
        // Truncate extremely long lines
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

void UJUSYNCBlueprintLibrary::AsyncBatchSpawnInternal(
    const TArray<FJUSYNCMeshData>& MeshDataArray,
    const TArray<FVector>& SpawnLocations,
    TSharedPtr<TArray<AActor*>> SharedResults,
    int32 CurrentBatch,
    int32 BatchSize,
    float BatchDelay)
{
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("No subsystem for async spawn"));
        return;
    }

    UWorld* World = Subsystem->GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("No world for async spawn"));
        return;
    }

    // Calculate batch range
    int32 StartIndex = CurrentBatch * BatchSize;
    int32 EndIndex = FMath::Min(StartIndex + BatchSize, MeshDataArray.Num());
    
    UE_LOG(LogTemp, Warning, TEXT("📦 Processing async batch %d: indices %d-%d"), 
        CurrentBatch, StartIndex, EndIndex - 1);

    // Process current batch
    for (int32 i = StartIndex; i < EndIndex; ++i)
    {
        AActor* SpawnedActor = SpawnRealtimeMeshAtLocation(
            MeshDataArray[i], SpawnLocations[i]);
        SharedResults->Add(SpawnedActor);
        
        if (SpawnedActor)
        {
            UE_LOG(LogTemp, Log, TEXT("✅ Async spawned mesh %d at %s"), 
                i, *SpawnLocations[i].ToString());
        }
    }

    // Check if we're done
    if (EndIndex >= MeshDataArray.Num())
    {
        // All batches complete
        UE_LOG(LogTemp, Warning, TEXT("🎉 Async batch spawn complete: %d/%d successful"), 
            SharedResults->Num(), MeshDataArray.Num());
        
        // Broadcast completion (you can add delegate broadcasting here)
        return;
    }

    // Schedule next batch
    FTimerHandle TimerHandle;
    World->GetTimerManager().SetTimer(TimerHandle, 
        FTimerDelegate::CreateLambda([=]()
        {
            AsyncBatchSpawnInternal(MeshDataArray, SpawnLocations, SharedResults, 
                                  CurrentBatch + 1, BatchSize, BatchDelay);
        }), 
        BatchDelay, false);
}

TArray<AActor*> UJUSYNCBlueprintLibrary::BatchSpawnRealtimeMeshesAtLocationsSync(
    const TArray<FJUSYNCMeshData>& MeshDataArray,
    const TArray<FVector>& SpawnLocations)
{
    TArray<AActor*> SpawnedActors;
    
    // Debug logging
    UE_LOG(LogTemp, Warning, TEXT("=== SYNC BATCH SPAWN DEBUG ==="));
    UE_LOG(LogTemp, Warning, TEXT("MeshDataArray.Num(): %d"), MeshDataArray.Num());
    UE_LOG(LogTemp, Warning, TEXT("SpawnLocations.Num(): %d"), SpawnLocations.Num());

    if (MeshDataArray.Num() != SpawnLocations.Num())
    {
        UE_LOG(LogTemp, Error, TEXT("❌ Array size mismatch! Meshes: %d, Locations: %d"),
            MeshDataArray.Num(), SpawnLocations.Num());
        return SpawnedActors;
    }

    SpawnedActors.Reserve(MeshDataArray.Num());
    int32 SuccessCount = 0;
    
    for (int32 i = 0; i < MeshDataArray.Num(); ++i)
    {
        UE_LOG(LogTemp, Warning, TEXT("🎯 Spawning mesh %d '%s' at location %s"),
            i, *MeshDataArray[i].ElementName, *SpawnLocations[i].ToString());
            
        AActor* SpawnedActor = SpawnRealtimeMeshAtLocation(MeshDataArray[i], SpawnLocations[i]);
        SpawnedActors.Add(SpawnedActor);
        
        if (SpawnedActor)
        {
            SuccessCount++;
            UE_LOG(LogTemp, Warning, TEXT("✅ Successfully spawned at %s"),
                *SpawnedActor->GetActorLocation().ToString());
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("❌ Failed to spawn mesh %d"), i);
        }
    }
    
    UE_LOG(LogTemp, Warning, TEXT("=== SYNC BATCH SPAWN COMPLETE: %d/%d successful ==="),
        SuccessCount, MeshDataArray.Num());
    
    return SpawnedActors;
}



