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
    
    UE_LOG(LogTemp, Warning, TEXT("📦 Processing async batch %d: indices %d-%d with rotations"),
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
            UE_LOG(LogTemp, Log, TEXT("✅ Async spawned mesh %d at %s with rotation %s"),
                   i, *SpawnLocations[i].ToString(), *UERotation.ToString());
        }
    }

    // Check if we're done
    if (EndIndex >= MeshDataArray.Num())
    {
        UE_LOG(LogTemp, Warning, TEXT("🎉 Async batch spawn complete: %d/%d successful"),
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

    UE_LOG(LogTemp, Warning, TEXT("=== SYNC BATCH SPAWN WITH ROTATIONS ==="));
    UE_LOG(LogTemp, Warning, TEXT("Processing %d meshes with locations and rotations"), MeshDataArray.Num());

    SpawnedActors.Reserve(MeshDataArray.Num());
    int32 SuccessCount = 0;

    for (int32 i = 0; i < MeshDataArray.Num(); ++i)
    {
        // Convert ParaView rotation to UE rotation if needed
        FRotator UERotation = ConvertParaViewToUERotation(FinalRotations[i]);
        
        UE_LOG(LogTemp, Warning, TEXT("🎯 Spawning mesh %d '%s' at location %s with rotation %s"),
               i, *MeshDataArray[i].ElementName, *SpawnLocations[i].ToString(), *UERotation.ToString());

        AActor* SpawnedActor = SpawnRealtimeMeshAtLocation(MeshDataArray[i], SpawnLocations[i], UERotation);
        SpawnedActors.Add(SpawnedActor);

        if (SpawnedActor)
        {
            SuccessCount++;
            UE_LOG(LogTemp, Warning, TEXT("✅ Successfully spawned at %s with rotation %s"),
                   *SpawnedActor->GetActorLocation().ToString(), 
                   *SpawnedActor->GetActorRotation().ToString());
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

FRotator UJUSYNCBlueprintLibrary::ConvertParaViewToUERotation(const FRotator& ParaViewRotation)
{
    // ParaView typically uses different axis conventions than UE
    // This conversion assumes ParaView uses Z-up while UE uses Z-up but different handedness
    FRotator UERotation;
    
    // Common conversion for ParaView to UE coordinate systems:
    // ParaView: X-right, Y-forward, Z-up (right-handed)
    // UE: X-forward, Y-right, Z-up (left-handed)
    
    UERotation.Pitch = -ParaViewRotation.Pitch;  // Flip pitch for handedness
    UERotation.Yaw = ParaViewRotation.Yaw + 90.0f;  // Rotate 90 degrees for axis alignment
    UERotation.Roll = ParaViewRotation.Roll;
    
    UE_LOG(LogTemp, Log, TEXT("Converted ParaView rotation %s to UE rotation %s"),
           *ParaViewRotation.ToString(), *UERotation.ToString());
    
    return UERotation;
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

TArray<AActor*> UJUSYNCBlueprintLibrary::BatchSpawnRealtimeMeshesWithMaterial(
    const TArray<FJUSYNCMeshData>& MeshDataArray,
    const TArray<FVector>& SpawnLocations,
    const TArray<FRotator>& SpawnRotations,
    UMaterialInterface* Material,
    bool bUseAsyncSpawning,
    int32 BatchSize,
    float BatchDelay)
{
    // Validation
    if (MeshDataArray.Num() != SpawnLocations.Num())
    {
        UE_LOG(LogTemp, Error, TEXT("❌ Array size mismatch! Meshes: %d, Locations: %d"),
            MeshDataArray.Num(), SpawnLocations.Num());
        return TArray<AActor*>();
    }

    // Create default rotations if not provided
    TArray<FRotator> FinalRotations = SpawnRotations;
    if (FinalRotations.Num() == 0)
    {
        FinalRotations = GenerateDefaultRotations(MeshDataArray.Num());
    }
    else if (FinalRotations.Num() != MeshDataArray.Num())
    {
        UE_LOG(LogTemp, Error, TEXT("❌ Rotation array size mismatch!"));
        return TArray<AActor*>();
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Subsystem not available"));
        return TArray<AActor*>();
    }

    UWorld* World = Subsystem->GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("No valid world context"));
        return TArray<AActor*>();
    }

    TArray<AActor*> SpawnedActors;
    SpawnedActors.Reserve(MeshDataArray.Num());

    int32 SuccessCount = 0;
    for (int32 i = 0; i < MeshDataArray.Num(); ++i)
    {
        // ✅ FIXED: Process mesh data to fix normals BEFORE spawning
        FJUSYNCMeshData ProcessedMeshData = FixMeshDataForSpawning(MeshDataArray[i]);
        
        FRotator UERotation = ConvertParaViewToUERotation(FinalRotations[i]);
        
        // Spawn actor
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
        AActor* SpawnedActor = World->SpawnActor<AActor>(SpawnParams);
        
        if (!SpawnedActor)
        {
            SpawnedActors.Add(nullptr);
            continue;
        }

        // Create RealtimeMeshComponent
        URealtimeMeshComponent* MeshComp = NewObject<URealtimeMeshComponent>(SpawnedActor);
        SpawnedActor->SetRootComponent(MeshComp);
        MeshComp->RegisterComponent();
        
        // Set transform
        SpawnedActor->SetActorLocation(SpawnLocations[i]);
        SpawnedActor->SetActorRotation(UERotation);

        // ✅ FIXED: Apply material BEFORE creating mesh
        if (Material)
        {
            MeshComp->SetMaterial(0, Material);
        }
        else
        {
            // Create a proper default double-sided material
            UMaterial* DefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
            if (DefaultMaterial)
            {
                DefaultMaterial->TwoSided = true;
                MeshComp->SetMaterial(0, DefaultMaterial);
            }
        }

        // ✅ FIXED: Create mesh with processed data
        bool bSuccess = Subsystem->CreateRealtimeMeshFromJUSYNC(ProcessedMeshData, MeshComp);
        if (bSuccess)
        {
            SuccessCount++;
            SpawnedActors.Add(SpawnedActor);
        }
        else
        {
            SpawnedActor->Destroy();
            SpawnedActors.Add(nullptr);
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("=== BATCH SPAWN COMPLETE: %d/%d successful ==="),
        SuccessCount, MeshDataArray.Num());

    return SpawnedActors;
}

FJUSYNCMeshData UJUSYNCBlueprintLibrary::FixMeshDataForSpawning(const FJUSYNCMeshData& InputMeshData)
{
    FJUSYNCMeshData FixedData = InputMeshData;
    
    // ✅ FIXED: Recalculate normals if missing or invalid
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
        
        UE_LOG(LogTemp, Warning, TEXT("Recalculated normals for spawned mesh: %s"), *InputMeshData.ElementName);
    }
    
    // ✅ FIXED: Validate and fix UV coordinates
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





