#include "JUSYNCSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture.h"
#include "TextureResource.h" 
#include "RenderUtils.h"
#include "RealtimeMeshComponent.h"
#include "JUSYNCBlueprintLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "RealtimeMeshSimple.h" 

// Include the C-wrapper header
#ifdef WITH_ANARI_USD_MIDDLEWARE
extern "C" {
#include "AnariUsdMiddleware_C.h"
}
#endif

// Global callback handlers for C interface
static UJUSYNCSubsystem* g_SubsystemInstance = nullptr;

#ifdef WITH_ANARI_USD_MIDDLEWARE

// Enhanced callback functions with detailed debugging
extern "C" void FileReceivedCallback_Static(const CFileData* file_data)
{
    UE_LOG(LogTemp, Warning, TEXT("=== ZMQ CALLBACK TRIGGERED ==="));
    
    if (!file_data)
    {
        UE_LOG(LogTemp, Error, TEXT("FileReceivedCallback_Static: NULL file_data received"));
        return;
    }
    
    UE_LOG(LogTemp, Warning, TEXT("ZMQ File Received:"));
    UE_LOG(LogTemp, Warning, TEXT("  - Filename: %s"), UTF8_TO_TCHAR(file_data->filename));
    UE_LOG(LogTemp, Warning, TEXT("  - File Type: %s"), UTF8_TO_TCHAR(file_data->file_type));
    UE_LOG(LogTemp, Warning, TEXT("  - Data Size: %d bytes"), file_data->data_size);
    UE_LOG(LogTemp, Warning, TEXT("  - Hash: %s"), UTF8_TO_TCHAR(file_data->hash));
    
    if (!g_SubsystemInstance)
    {
        UE_LOG(LogTemp, Error, TEXT("FileReceivedCallback_Static: g_SubsystemInstance is NULL"));
        return;
    }
    
    UE_LOG(LogTemp, Log, TEXT("Creating async task for file processing..."));
    
    // Create a copy of the data for the lambda
    CFileData LocalData = *file_data;

    if (file_data->data && file_data->data_size > 0) {
        LocalData.data = new unsigned char[file_data->data_size];
        std::memcpy(LocalData.data, file_data->data, file_data->data_size);
    }

    
    AsyncTask(ENamedThreads::GameThread, [LocalData]()
    {
        UE_LOG(LogTemp, Warning, TEXT("=== ASYNC TASK EXECUTING ON GAME THREAD ==="));
        
        if (!g_SubsystemInstance)
        {
            UE_LOG(LogTemp, Error, TEXT("Async Task: g_SubsystemInstance is NULL on game thread"));
            return;
        }
        
        UE_LOG(LogTemp, Log, TEXT("Converting C data to UE format..."));
        
        FJUSYNCFileData UEFileData;
        UEFileData.Filename = FString(UTF8_TO_TCHAR(LocalData.filename));
        UEFileData.Hash = FString(UTF8_TO_TCHAR(LocalData.hash));
        UEFileData.FileType = FString(UTF8_TO_TCHAR(LocalData.file_type));
        UEFileData.Data.SetNum(LocalData.data_size);
        FMemory::Memcpy(UEFileData.Data.GetData(), LocalData.data, LocalData.data_size);
        
        UE_LOG(LogTemp, Warning, TEXT("Broadcasting to Blueprint events..."));
        UE_LOG(LogTemp, Warning, TEXT("  - UE Filename: %s"), *UEFileData.Filename);
        UE_LOG(LogTemp, Warning, TEXT("  - UE File Type: %s"), *UEFileData.FileType);
        UE_LOG(LogTemp, Warning, TEXT("  - UE Data Size: %d"), UEFileData.Data.Num());
        
        // Send to Blueprint Library FIRST
        g_SubsystemInstance->HandleFileReceivedForLibrary(UEFileData);
        
        // Broadcast to subsystem events
        g_SubsystemInstance->OnFileReceived.Broadcast(UEFileData);
        
        UE_LOG(LogTemp, Warning, TEXT("=== FILE PROCESSING COMPLETE ==="));

        if (LocalData.data) {
            delete[] LocalData.data;
        }

    });
}

extern "C" void MessageReceivedCallback_Static(const char* message)
{
    UE_LOG(LogTemp, Warning, TEXT("=== ZMQ MESSAGE CALLBACK TRIGGERED ==="));
    
    if (!message)
    {
        UE_LOG(LogTemp, Error, TEXT("MessageReceivedCallback_Static: NULL message received"));
        return;
    }
    
    UE_LOG(LogTemp, Warning, TEXT("ZMQ Message: %s"), UTF8_TO_TCHAR(message));
    
    if (!g_SubsystemInstance)
    {
        UE_LOG(LogTemp, Error, TEXT("MessageReceivedCallback_Static: g_SubsystemInstance is NULL"));
        return;
    }
    
    // Create a copy of the message for the lambda
    FString MessageCopy = FString(UTF8_TO_TCHAR(message));
    
    AsyncTask(ENamedThreads::GameThread, [MessageCopy]()
    {
        if (g_SubsystemInstance)
        {
            UE_LOG(LogTemp, Warning, TEXT("Broadcasting message to Blueprint: %s"), *MessageCopy);
            g_SubsystemInstance->OnMessageReceived.Broadcast(MessageCopy);
            g_SubsystemInstance->HandleMessageReceivedForLibrary(MessageCopy);
        }
    });
}

// Helper function to convert C mesh data to UE format
// Enhanced ConvertCMeshDataToUE_Helper with FORCED vertex interpolation while preserving all functionality
static FJUSYNCMeshData ConvertCMeshDataToUE_Helper(const CMeshData& CMesh, bool bForceVertexInterpolation = true)
{
    FJUSYNCMeshData UEMesh;

    // 1. Convert names (PRESERVED)
    UEMesh.ElementName = FString(UTF8_TO_TCHAR(CMesh.element_name));
    UEMesh.TypeName = FString(UTF8_TO_TCHAR(CMesh.type_name));

    // 2. Convert points (PRESERVED - flat array → FVector array)
    size_t PointCount = CMesh.points_count / 3;
    UEMesh.Vertices.Reserve(PointCount);
    for (size_t i = 0; i < PointCount; ++i)
    {
        size_t idx = i * 3;
        float x = CMesh.points[idx + 0];
        float y = CMesh.points[idx + 1];
        float z = CMesh.points[idx + 2];
        // Transform from right-handed Z-up (ParaView) to left-handed Z-up (UE)
        UEMesh.Vertices.Add(FVector(x, -y, z));
    }

    // 3. Convert triangle indices (PRESERVED)
    size_t IndexCount = CMesh.indices_count;
    UEMesh.Triangles.Reserve(IndexCount);
    for (size_t i = 0; i < IndexCount; ++i)
    {
        UEMesh.Triangles.Add(static_cast<int32>(CMesh.indices[i]));
    }

    // 4. Convert normals if present (PRESERVED)
    if (CMesh.normals && CMesh.normals_count >= 3)
    {
        size_t NormalCount = CMesh.normals_count / 3;
        UEMesh.Normals.Reserve(NormalCount);
        for (size_t i = 0; i < NormalCount; ++i)
        {
            size_t idx = i * 3;
            float nx = CMesh.normals[idx + 0];
            float ny = CMesh.normals[idx + 1];
            float nz = CMesh.normals[idx + 2];
            UEMesh.Normals.Add(FVector(nx, -ny, nz).GetSafeNormal());
        }
    }

    // 5. Convert UVs if present (PRESERVED)
    if (CMesh.uvs && CMesh.uvs_count >= 2)
    {
        size_t UVCount = CMesh.uvs_count / 2;
        UEMesh.UVs.Reserve(UVCount);
        for (size_t i = 0; i < UVCount; ++i)
        {
            size_t idx = i * 2;
            UEMesh.UVs.Add(FVector2D(CMesh.uvs[idx], CMesh.uvs[idx + 1]));
        }
    }

    // 6. ✅ ENHANCED: Vertex‐colors with FORCED vertex interpolation while preserving all functionality
    if (CMesh.vertex_colors && CMesh.vertex_colors_count >= 4)
    {
        int32 VertexCount = static_cast<int32>(PointCount);
        int32 FaceCount = static_cast<int32>(UEMesh.Triangles.Num() / 3);
        int32 ColorCount = static_cast<int32>(CMesh.vertex_colors_count / 4);

        bool bDetectedVertexInterp = (ColorCount == VertexCount);
        bool bDetectedUniformInterp = (ColorCount == FaceCount);
        
        UE_LOG(LogTemp, Warning, TEXT("🎨 Color conversion: %d colors, %d vertices, %d faces"),
               ColorCount, VertexCount, FaceCount);
        UE_LOG(LogTemp, Warning, TEXT("🎨 Detected: %s | Force Vertex: %s"),
               bDetectedVertexInterp ? TEXT("VERTEX") : (bDetectedUniformInterp ? TEXT("UNIFORM") : TEXT("UNKNOWN")),
               bForceVertexInterpolation ? TEXT("YES") : TEXT("NO"));

        UEMesh.VertexColors.Reserve(VertexCount);

        if (bDetectedVertexInterp)
        {
            // ✅ CASE 1: Already vertex interpolation - direct mapping (PRESERVED)
            UE_LOG(LogTemp, Warning, TEXT("🎨 Using direct VERTEX interpolation"));
            for (int32 i = 0; i < VertexCount; ++i)
            {
                int64 idx = int64(i) * 4;
                uint8 r = uint8(FMath::Clamp(CMesh.vertex_colors[idx + 0] * 255.0f, 0.0f, 255.0f));
                uint8 g = uint8(FMath::Clamp(CMesh.vertex_colors[idx + 1] * 255.0f, 0.0f, 255.0f));
                uint8 b = uint8(FMath::Clamp(CMesh.vertex_colors[idx + 2] * 255.0f, 0.0f, 255.0f));
                uint8 a = uint8(FMath::Clamp(CMesh.vertex_colors[idx + 3] * 255.0f, 0.0f, 255.0f));
                UEMesh.VertexColors.Add(FColor(r, g, b, a));
            }
        }
        else if (bDetectedUniformInterp && bForceVertexInterpolation)
        {
            // ✅ CASE 2: Uniform detected + Force Vertex = Convert uniform to smooth vertex interpolation
            UE_LOG(LogTemp, Warning, TEXT("🎨 CONVERTING uniform to smooth VERTEX interpolation"));
            
            // Initialize vertex color accumulation arrays
            TArray<FLinearColor> AccumulatedColors;
            TArray<int32> ColorCounts;
            AccumulatedColors.SetNumZeroed(VertexCount);
            ColorCounts.SetNumZeroed(VertexCount);
            
            // ✅ ENHANCED: Accumulate colors from all faces that use each vertex (for smooth blending)
            for (int32 FaceIdx = 0; FaceIdx < FaceCount && FaceIdx < ColorCount; ++FaceIdx)
            {
                int32 i0 = UEMesh.Triangles[FaceIdx * 3 + 0];
                int32 i1 = UEMesh.Triangles[FaceIdx * 3 + 1];
                int32 i2 = UEMesh.Triangles[FaceIdx * 3 + 2];
                
                if (i0 < VertexCount && i1 < VertexCount && i2 < VertexCount)
                {
                    int64 cidx = int64(FaceIdx) * 4;
                    
                    // Get face color as linear color for better blending
                    FLinearColor FaceColor(
                        CMesh.vertex_colors[cidx + 0],
                        CMesh.vertex_colors[cidx + 1],
                        CMesh.vertex_colors[cidx + 2],
                        CMesh.vertex_colors[cidx + 3]
                    );
                    
                    // ✅ SMOOTH BLENDING: Accumulate this face color to all three vertices
                    AccumulatedColors[i0] += FaceColor;
                    AccumulatedColors[i1] += FaceColor;
                    AccumulatedColors[i2] += FaceColor;
                    
                    ColorCounts[i0]++;
                    ColorCounts[i1]++;
                    ColorCounts[i2]++;
                }
            }
            
            // ✅ FINALIZE: Average the accumulated colors and convert to FColor
            for (int32 i = 0; i < VertexCount; ++i)
            {
                FLinearColor FinalColor;
                if (ColorCounts[i] > 0)
                {
                    // Average the accumulated colors
                    FinalColor = AccumulatedColors[i] / ColorCounts[i];
                }
                else
                {
                    // Fallback for vertices not used by any face
                    FinalColor = FLinearColor::White;
                }
                
                // Convert to FColor with proper clamping
                uint8 r = uint8(FMath::Clamp(FinalColor.R * 255.0f, 0.0f, 255.0f));
                uint8 g = uint8(FMath::Clamp(FinalColor.G * 255.0f, 0.0f, 255.0f));
                uint8 b = uint8(FMath::Clamp(FinalColor.B * 255.0f, 0.0f, 255.0f));
                uint8 a = uint8(FMath::Clamp(FinalColor.A * 255.0f, 0.0f, 255.0f));
                
                UEMesh.VertexColors.Add(FColor(r, g, b, a));
            }
            
            UE_LOG(LogTemp, Warning, TEXT("✅ Converted uniform to smooth vertex interpolation: %d vertex colors"), 
                   UEMesh.VertexColors.Num());
        }
        else if (bDetectedUniformInterp && !bForceVertexInterpolation)
        {
            // ✅ CASE 3: Keep original uniform behavior (PRESERVED for backwards compatibility)
            UE_LOG(LogTemp, Warning, TEXT("🎨 Using original UNIFORM interpolation (flat shading)"));
            UEMesh.VertexColors.Reserve(FaceCount * 3);
            
            for (int32 f = 0; f < FaceCount; ++f)
            {
                int64 cidx = int64(f) * 4;
                uint8 r = uint8(FMath::Clamp(CMesh.vertex_colors[cidx + 0] * 255.0f, 0.0f, 255.0f));
                uint8 g = uint8(FMath::Clamp(CMesh.vertex_colors[cidx + 1] * 255.0f, 0.0f, 255.0f));
                uint8 b = uint8(FMath::Clamp(CMesh.vertex_colors[cidx + 2] * 255.0f, 0.0f, 255.0f));
                uint8 a = uint8(FMath::Clamp(CMesh.vertex_colors[cidx + 3] * 255.0f, 0.0f, 255.0f));
                FColor faceColor(r, g, b, a);
                
                // Assign to each of the three vertices of face f
                for (int vi = 0; vi < 3; ++vi)
                {
                    UEMesh.VertexColors.Add(faceColor);
                }
            }
        }
        else
        {
            // ✅ CASE 4: Fallback behavior (PRESERVED)
            UE_LOG(LogTemp, Warning, TEXT("🎨 Using fallback vertex interpolation"));
            for (int32 i = 0; i < VertexCount; ++i)
            {
                if (i < ColorCount)
                {
                    int64 idx = int64(i) * 4;
                    uint8 r = uint8(FMath::Clamp(CMesh.vertex_colors[idx + 0] * 255.0f, 0.0f, 255.0f));
                    uint8 g = uint8(FMath::Clamp(CMesh.vertex_colors[idx + 1] * 255.0f, 0.0f, 255.0f));
                    uint8 b = uint8(FMath::Clamp(CMesh.vertex_colors[idx + 2] * 255.0f, 0.0f, 255.0f));
                    uint8 a = uint8(FMath::Clamp(CMesh.vertex_colors[idx + 3] * 255.0f, 0.0f, 255.0f));
                    UEMesh.VertexColors.Add(FColor(r, g, b, a));
                }
                else
                {
                    UEMesh.VertexColors.Add(FColor::White);
                }
            }
        }

        // ✅ PRESERVED: Debug logging for color verification
        UE_LOG(LogTemp, Warning, TEXT("🎨 Final vertex colors: %d"), UEMesh.VertexColors.Num());
        
        // ✅ PRESERVED: DEBUG dump first 20 different colours
        TSet<FColor> Unique;
        for (int32 i = 0; i < UEMesh.VertexColors.Num(); ++i)
        {
            const FColor& C = UEMesh.VertexColors[i];
            if (!Unique.Contains(C))
            {
                Unique.Add(C);
                UE_LOG(LogTemp, Warning, TEXT("USD Color[%d] = (R=%d G=%d B=%d A=%d)"),
                       i, C.R, C.G, C.B, C.A);
                if (Unique.Num() == 20) break;
            }
        }
        UE_LOG(LogTemp, Warning, TEXT("Total unique colours in first scan: %d"), Unique.Num());
    }

    return UEMesh;
}


#endif

void UJUSYNCSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    
    // Set global instance for callbacks
    g_SubsystemInstance = this;
    
    UE_LOG(LogTemp, Warning, TEXT("=== JUSYNC SUBSYSTEM INITIALIZED ==="));
    UE_LOG(LogTemp, Warning, TEXT("Global instance set: %p"), g_SubsystemInstance);
    UE_LOG(LogTemp, Log, TEXT("JUSYNCSubsystem initialized with C-wrapper interface"));
}

void UJUSYNCSubsystem::Deinitialize()
{
    UE_LOG(LogTemp, Warning, TEXT("=== JUSYNC SUBSYSTEM DEINITIALIZING ==="));
    
    ShutdownMiddleware();
    
    // Clear global instance
    g_SubsystemInstance = nullptr;
    
    Super::Deinitialize();
    UE_LOG(LogTemp, Log, TEXT("JUSYNCSubsystem deinitialized"));
}

bool UJUSYNCSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    return true;
}

bool UJUSYNCSubsystem::InitializeMiddleware(const FString& Endpoint)
{
    FScopeLock Lock(&MiddlewareMutex);
    
    UE_LOG(LogTemp, Warning, TEXT("=== INITIALIZING JUSYNC MIDDLEWARE ==="));
    UE_LOG(LogTemp, Warning, TEXT("Requested Endpoint: %s"), *Endpoint);
    UE_LOG(LogTemp, Warning, TEXT("Subsystem instance: %p"), this);
    UE_LOG(LogTemp, Warning, TEXT("Global instance: %p"), g_SubsystemInstance);
    
    // Ensure global instance is set
    if (!g_SubsystemInstance)
    {
        g_SubsystemInstance = this;
        UE_LOG(LogTemp, Warning, TEXT("Set global instance: %p"), g_SubsystemInstance);
    }
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    // Convert FString to C string
    FTCHARToUTF8 EndpointConverter(*Endpoint);
    const char* EndpointCStr = Endpoint.IsEmpty() ? "tcp://*:5556" : EndpointConverter.Get();
    
    UE_LOG(LogTemp, Warning, TEXT("Using C endpoint: %s"), UTF8_TO_TCHAR(EndpointCStr));
    
    // Register callbacks BEFORE initialization
    UE_LOG(LogTemp, Warning, TEXT("Registering ZMQ callbacks..."));
    RegisterUpdateCallback_C(FileReceivedCallback_Static);
    RegisterMessageCallback_C(MessageReceivedCallback_Static);
    UE_LOG(LogTemp, Warning, TEXT("✅ Callbacks registered"));
    
    // Initialize middleware using C interface
    UE_LOG(LogTemp, Warning, TEXT("Calling InitializeMiddleware_C..."));
    int Result = InitializeMiddleware_C(EndpointCStr);
    
    UE_LOG(LogTemp, Warning, TEXT("InitializeMiddleware_C returned: %d"), Result);
    
    bIsInitialized.store(Result == 1);
    
    if (Result == 1)
    {
        UE_LOG(LogTemp, Warning, TEXT("✅ ROUTER socket bound to: %s"), UTF8_TO_TCHAR(EndpointCStr));
        UE_LOG(LogTemp, Warning, TEXT("✅ Waiting for DEALER connections..."));
        
        // Test connection status
        int ConnectionStatus = IsConnected_C();
        UE_LOG(LogTemp, Warning, TEXT("Connection status check: %d"), ConnectionStatus);
        
        // Get status info
        const char* StatusInfo = GetStatusInfo_C();
        UE_LOG(LogTemp, Warning, TEXT("Middleware status: %s"), UTF8_TO_TCHAR(StatusInfo));
        
        UE_LOG(LogTemp, Log, TEXT("JUSYNC Middleware initialized successfully on %s"), *Endpoint);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("❌ Failed to initialize JUSYNC Middleware (Result: %d)"), Result);
    }
    
    return Result == 1;
#else
    UE_LOG(LogTemp, Error, TEXT("JUSYNC compiled without middleware support"));
    return false;
#endif
}

void UJUSYNCSubsystem::ShutdownMiddleware()
{
    FScopeLock Lock(&MiddlewareMutex);
    
    UE_LOG(LogTemp, Warning, TEXT("=== SHUTTING DOWN JUSYNC MIDDLEWARE ==="));
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    ShutdownMiddleware_C();
    bIsInitialized.store(false);
    UE_LOG(LogTemp, Log, TEXT("JUSYNC Middleware shutdown"));
#endif
}

bool UJUSYNCSubsystem::IsMiddlewareConnected() const
{
    FScopeLock Lock(&MiddlewareMutex);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    bool bConnected = (IsConnected_C() == 1) && bIsInitialized.load();
    
    // Periodic connection status logging
    static int32 StatusCheckCount = 0;
    StatusCheckCount++;
    if (StatusCheckCount % 1000 == 0) // Log every 1000 calls
    {
        UE_LOG(LogTemp, Log, TEXT("Connection status: %s (Check #%d)"), 
               bConnected ? TEXT("CONNECTED") : TEXT("DISCONNECTED"), StatusCheckCount);
    }
    
    return bConnected;
#else
    return false;
#endif
}

FString UJUSYNCSubsystem::GetStatusInfo() const
{
    FScopeLock Lock(&MiddlewareMutex);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    const char* StatusCStr = GetStatusInfo_C();
    FString Status = FString(UTF8_TO_TCHAR(StatusCStr));
    
    UE_LOG(LogTemp, Log, TEXT("Status info requested: %s"), *Status);
    return Status;
#else
    return TEXT("Middleware not available");
#endif
}

bool UJUSYNCSubsystem::StartReceiving()
{
    FScopeLock Lock(&MiddlewareMutex);
    
    UE_LOG(LogTemp, Warning, TEXT("=== STARTING JUSYNC RECEIVING ==="));
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogTemp, Error, TEXT("❌ Cannot start receiving - middleware not initialized"));
        return false;
    }
    
    UE_LOG(LogTemp, Warning, TEXT("Middleware is initialized, calling StartReceiving_C..."));
    
    int Result = StartReceiving_C();
    
    UE_LOG(LogTemp, Warning, TEXT("StartReceiving_C returned: %d"), Result);
    
    if (Result == 1)
    {
        UE_LOG(LogTemp, Warning, TEXT("✅ JUSYNC Started Receiving Data"));
        UE_LOG(LogTemp, Warning, TEXT("✅ ROUTER is now listening for DEALER messages"));
        
        // Additional status checks
        int ConnectionStatus = IsConnected_C();
        UE_LOG(LogTemp, Warning, TEXT("Post-start connection status: %d"), ConnectionStatus);
        
        const char* StatusInfo = GetStatusInfo_C();
        UE_LOG(LogTemp, Warning, TEXT("Post-start middleware status: %s"), UTF8_TO_TCHAR(StatusInfo));
        
        UE_LOG(LogTemp, Log, TEXT("JUSYNC Start receiving: Success"));
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("❌ Failed to start receiving (Result: %d)"), Result);
        UE_LOG(LogTemp, Log, TEXT("JUSYNC Start receiving: Failed"));
    }
    
    return Result == 1;
#endif
    return false;
}

void UJUSYNCSubsystem::StopReceiving()
{
    FScopeLock Lock(&MiddlewareMutex);
    
    UE_LOG(LogTemp, Warning, TEXT("=== STOPPING JUSYNC RECEIVING ==="));
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    StopReceiving_C();
    UE_LOG(LogTemp, Log, TEXT("JUSYNC Stopped receiving"));
#endif
}

void UJUSYNCSubsystem::HandleFileReceivedForLibrary(const FJUSYNCFileData& FileData)
{
    UE_LOG(LogTemp, Warning, TEXT("=== ADDING FILE TO BLUEPRINT LIBRARY ==="));
    UE_LOG(LogTemp, Warning, TEXT("File: %s (%d bytes, %s)"), 
           *FileData.Filename, FileData.Data.Num(), *FileData.FileType);
    
    FScopeLock Lock(&UJUSYNCBlueprintLibrary::DataMutex);
    
    int32 PreviousCount = UJUSYNCBlueprintLibrary::ReceivedFiles.Num();
    UJUSYNCBlueprintLibrary::ReceivedFiles.Add(FileData);
    int32 NewCount = UJUSYNCBlueprintLibrary::ReceivedFiles.Num();
    
    UE_LOG(LogTemp, Warning, TEXT("Blueprint Library file count: %d -> %d"), PreviousCount, NewCount);
    UE_LOG(LogTemp, Warning, TEXT("✅ File added to Blueprint Library: %s"), *FileData.Filename);
}

void UJUSYNCSubsystem::HandleMessageReceivedForLibrary(const FString& Message)
{
    UE_LOG(LogTemp, Warning, TEXT("=== ADDING MESSAGE TO BLUEPRINT LIBRARY ==="));
    UE_LOG(LogTemp, Warning, TEXT("Message: %s"), *Message);
    
    FScopeLock Lock(&UJUSYNCBlueprintLibrary::DataMutex);
    
    int32 PreviousCount = UJUSYNCBlueprintLibrary::ReceivedMessages.Num();
    UJUSYNCBlueprintLibrary::ReceivedMessages.Add(Message);
    int32 NewCount = UJUSYNCBlueprintLibrary::ReceivedMessages.Num();
    
    UE_LOG(LogTemp, Warning, TEXT("Blueprint Library message count: %d -> %d"), PreviousCount, NewCount);
    UE_LOG(LogTemp, Log, TEXT("Message received for Blueprint Library: %s"), *Message);
}

bool UJUSYNCSubsystem::LoadUSDFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCMeshData>& OutMeshData)
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Middleware not initialized"));
        return false;
    }
    
    // Convert FString to C string
    FTCHARToUTF8 FilenameConverter(*Filename);
    const char* FilenameCStr = FilenameConverter.Get();
    
    CMeshData* CMeshes = nullptr;
    size_t MeshCount = 0;
    
    // Call C interface
    int Result = LoadUSDBuffer_C(Buffer.GetData(), Buffer.Num(), FilenameCStr, &CMeshes, &MeshCount);
    
    if (Result == 1 && CMeshes && MeshCount > 0)
    {
        OutMeshData.Empty();
        OutMeshData.Reserve(MeshCount);
        
        // Convert C mesh data to UE format
        for (size_t i = 0; i < MeshCount; ++i)
        {
            FJUSYNCMeshData UEMeshData = ConvertCMeshDataToUE_Helper(CMeshes[i]);
            OutMeshData.Add(UEMeshData);
        }
        
        // Free C memory
        FreeMeshData_C(CMeshes, MeshCount);
        
        UE_LOG(LogTemp, Log, TEXT("Successfully loaded %d meshes from USD buffer"), OutMeshData.Num());
        return true;
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to load USD from buffer"));
        if (CMeshes)
        {
            FreeMeshData_C(CMeshes, MeshCount);
        }
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::LoadUSDFromDisk(const FString& FilePath, TArray<FJUSYNCMeshData>& OutMeshData)
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Middleware not initialized"));
        return false;
    }
    
    // Convert FString to C string
    FTCHARToUTF8 FilePathConverter(*FilePath);
    const char* FilePathCStr = FilePathConverter.Get();
    
    CMeshData* CMeshes = nullptr;
    size_t MeshCount = 0;
    
    // Call C interface
    int Result = LoadUSDFromDisk_C(FilePathCStr, &CMeshes, &MeshCount);
    
    if (Result == 1 && CMeshes && MeshCount > 0)
    {
        OutMeshData.Empty();
        OutMeshData.Reserve(MeshCount);
        
        // Convert C mesh data to UE format
        for (size_t i = 0; i < MeshCount; ++i)
        {
            FJUSYNCMeshData UEMeshData = ConvertCMeshDataToUE_Helper(CMeshes[i]);
            OutMeshData.Add(UEMeshData);
        }
        
        // Free C memory
        FreeMeshData_C(CMeshes, MeshCount);
        
        UE_LOG(LogTemp, Log, TEXT("Successfully loaded %d meshes from USD file"), OutMeshData.Num());
        return true;
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to load USD from disk"));
        if (CMeshes)
        {
            FreeMeshData_C(CMeshes, MeshCount);
        }
    }
#endif
    return false;
}

FJUSYNCTextureData UJUSYNCSubsystem::CreateTextureFromBuffer(const TArray<uint8>& Buffer)
{
    FJUSYNCTextureData Result;
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Middleware not initialized"));
        return Result;
    }
    
    // Call C interface
    CTextureData CTexture = CreateTextureFromBuffer_C(Buffer.GetData(), Buffer.Num());
    
    if (CTexture.data && CTexture.data_size > 0)
    {
        Result.Width = CTexture.width;
        Result.Height = CTexture.height;
        Result.Channels = CTexture.channels;
        Result.Data.SetNum(CTexture.data_size);
        FMemory::Memcpy(Result.Data.GetData(), CTexture.data, CTexture.data_size);
        
        // Free C memory
        FreeTextureData_C(&CTexture);
        
        UE_LOG(LogTemp, Log, TEXT("Created texture: %dx%d (%d channels)"), Result.Width, Result.Height, Result.Channels);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to create texture from buffer"));
    }
#endif
    
    return Result;
}

bool UJUSYNCSubsystem::WriteGradientLineAsPNG(const TArray<uint8>& Buffer, const FString& OutputPath)
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Middleware not initialized"));
        return false;
    }
    
    // Convert FString to C string
    FTCHARToUTF8 OutputPathConverter(*OutputPath);
    const char* OutputPathCStr = OutputPathConverter.Get();
    
    // Call C interface
    int Result = WriteGradientLineAsPNG_C(Buffer.GetData(), Buffer.Num(), OutputPathCStr);
    
    if (Result == 1)
    {
        UE_LOG(LogTemp, Log, TEXT("Gradient PNG saved: %s"), *OutputPath);
        return true;
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to save gradient PNG"));
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::GetGradientLineAsPNGBuffer(const TArray<uint8>& Buffer, TArray<uint8>& OutPNGBuffer)
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Middleware not initialized"));
        return false;
    }
    
    unsigned char* PNGData = nullptr;
    size_t PNGSize = 0;
    
    // Call C interface
    int Result = GetGradientLineAsPNGBuffer_C(Buffer.GetData(), Buffer.Num(), &PNGData, &PNGSize);
    
    if (Result == 1 && PNGData && PNGSize > 0)
    {
        OutPNGBuffer.SetNum(PNGSize);
        FMemory::Memcpy(OutPNGBuffer.GetData(), PNGData, PNGSize);
        
        // Free C memory
        FreeBuffer_C(PNGData);
        
        UE_LOG(LogTemp, Log, TEXT("Gradient PNG buffer created: %d bytes"), OutPNGBuffer.Num());
        return true;
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to create gradient PNG buffer"));
        if (PNGData)
        {
            FreeBuffer_C(PNGData);
        }
    }
#endif
    return false;
}

void RecalculateNormals(FJUSYNCMeshData& MeshData)
{
    if (MeshData.Vertices.Num() == 0 || MeshData.Triangles.Num() == 0)
        return;

    // Initialize normals array
    MeshData.Normals.SetNum(MeshData.Vertices.Num());
    for (int32 i = 0; i < MeshData.Normals.Num(); ++i)
    {
        MeshData.Normals[i] = FVector::ZeroVector;
    }

    // Calculate face normals with correct orientation for counter-clockwise winding
    for (int32 i = 0; i < MeshData.Triangles.Num(); i += 3)
    {
        int32 i0 = MeshData.Triangles[i];
        int32 i1 = MeshData.Triangles[i + 1];
        int32 i2 = MeshData.Triangles[i + 2];
        
        if (i0 < MeshData.Vertices.Num() && i1 < MeshData.Vertices.Num() && i2 < MeshData.Vertices.Num())
        {
            FVector v0 = MeshData.Vertices[i0];
            FVector v1 = MeshData.Vertices[i1];
            FVector v2 = MeshData.Vertices[i2];
            
            // ✅ FIXED: Calculate normal for counter-clockwise winding (v2-v0 x v1-v0)
            FVector FaceNormal = FVector::CrossProduct(v2 - v0, v1 - v0).GetSafeNormal();
            
            // Ensure normal points outward (you may need to flip this if still wrong)
            MeshData.Normals[i0] += FaceNormal;
            MeshData.Normals[i1] += FaceNormal;
            MeshData.Normals[i2] += FaceNormal;
        }
    }

    // Normalize accumulated normals
    for (int32 i = 0; i < MeshData.Normals.Num(); ++i)
    {
        MeshData.Normals[i] = MeshData.Normals[i].GetSafeNormal();
        if (MeshData.Normals[i].IsNearlyZero())
        {
            MeshData.Normals[i] = FVector::UpVector; // Fallback normal
        }
    }
    
    UE_LOG(LogTemp, Warning, TEXT("Recalculated normals with correct CCW winding"));
}

bool UJUSYNCSubsystem::CreateRealtimeMeshFromJUSYNC(
    const FJUSYNCMeshData& InMeshData,
    URealtimeMeshComponent* RealtimeMeshComponent)
{
    if (!RealtimeMeshComponent || !InMeshData.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("❌ Invalid input to CreateRealtimeMeshFromJUSYNC"));
        return false;
    }

    // Use the mesh data as-is (already processed by ConvertCMeshDataToUE_Helper with forced vertex interpolation)
    const FJUSYNCMeshData& MeshData = InMeshData;
    
    UE_LOG(LogTemp, Warning, TEXT("🎨 === SMOOTH VERTEX INTERPOLATION MESH CREATION ==="));
    UE_LOG(LogTemp, Warning, TEXT("Mesh: %d vertices, %d triangles, %d colors"),
           MeshData.Vertices.Num(), MeshData.Triangles.Num() / 3, MeshData.VertexColors.Num());

    // Calculate final counts (already processed by helper function)
    const int32 FinalVertexCount = MeshData.Vertices.Num();
    const int32 FinalTriCount = MeshData.Triangles.Num() / 3;

    // Initialize RealtimeMesh builder
    URealtimeMeshSimple* RealtimeMesh = RealtimeMeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
    if (!RealtimeMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("❌ Failed to initialize RealtimeMesh"));
        return false;
    }

    RealtimeMesh->SetupMaterialSlot(0, TEXT("PrimaryMaterial"));

    // Only apply vertex color material if no material is already set
    if (!RealtimeMeshComponent->GetMaterial(0))
    {
        UMaterial* VertexColorMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Game/Materials/M_VertexColor"));
        if (VertexColorMaterial)
        {
            RealtimeMeshComponent->SetMaterial(0, VertexColorMaterial);
            UE_LOG(LogTemp, Warning, TEXT("✅ Applied M_VertexColor material as fallback"));
        }
        else
        {
            // Fallback to enhanced default material
            UMaterial* DefaultMat = UMaterial::GetDefaultMaterial(MD_Surface);
            if (DefaultMat)
            {
                auto* DynMat = UMaterialInstanceDynamic::Create(DefaultMat, RealtimeMeshComponent);
                DynMat->SetScalarParameterValue(TEXT("UseVertexColor"), 1.0f);
                RealtimeMeshComponent->SetMaterial(0, DynMat);
                UE_LOG(LogTemp, Warning, TEXT("✅ Applied enhanced default material as fallback"));
            }
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("✅ Using provided material (preserving texture material from Blueprint)"));
    }


    RealtimeMesh::FRealtimeMeshStreamSet Streams;
    auto Builder = RealtimeMesh::TRealtimeMeshBuilderLocal<uint32>(Streams);
    Builder.EnableTangents();
    Builder.EnableTexCoords();
    Builder.EnableColors();
    Builder.EnablePolyGroups();

    // Add vertices and attributes - SIMPLIFIED since vertex interpolation is already handled
    for (int32 i = 0; i < FinalVertexCount; ++i)
    {
        Builder.AddVertex(FVector3f(MeshData.Vertices[i]));

        // Normals
        FVector3f N = FVector3f(MeshData.Normals.IsValidIndex(i) ? MeshData.Normals[i] : FVector::UpVector);
        Builder.SetNormal(i, N);

        // UVs
        if (MeshData.HasUVs() && MeshData.UVs.IsValidIndex(i))
        {
            Builder.SetTexCoord(i, 0, FVector2DHalf(FVector2f(MeshData.UVs[i])));
        }
        else
        {
            Builder.SetTexCoord(i, 0, FVector2DHalf(FVector2f::ZeroVector));
        }

        // Colors - now using smooth vertex interpolation (already processed)
        if (MeshData.HasVertexColors() && MeshData.VertexColors.IsValidIndex(i))
        {
            FColor VertexColor = MeshData.VertexColors[i];
            Builder.SetColor(i, VertexColor);
            
            // Debug first few vertices
            if (i < 6)
            {
                UE_LOG(LogTemp, Warning, TEXT("🎨 Vertex %d: Smooth Color=(%d,%d,%d,%d)"), 
                       i, VertexColor.R, VertexColor.G, VertexColor.B, VertexColor.A);
            }
        }
        else
        {
            Builder.SetColor(i, FColor::White);
        }
    }

    // Add triangles
    for (int32 Face = 0; Face < FinalTriCount; ++Face)
    {
        int32 i0 = MeshData.Triangles[Face*3 + 0];
        int32 i1 = MeshData.Triangles[Face*3 + 1];
        int32 i2 = MeshData.Triangles[Face*3 + 2];
        
        if (i0 < FinalVertexCount && i1 < FinalVertexCount && i2 < FinalVertexCount)
        {
            Builder.AddTriangle(i0, i1, i2);
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("❌ Invalid triangle %d: [%d,%d,%d] vs %d vertices"), 
                   Face, i0, i1, i2, FinalVertexCount);
        }
    }

    // Finalize the mesh section
    const FRealtimeMeshSectionGroupKey GroupKey = FRealtimeMeshSectionGroupKey::Create(0, TEXT("USDGroup"));
    const FRealtimeMeshSectionKey SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);
    RealtimeMesh->CreateSectionGroup(GroupKey, Streams);
    FRealtimeMeshSectionConfig SectionConfig(0);
    SectionConfig.bIsVisible = true;
    SectionConfig.bCastsShadow = true;
    RealtimeMesh->UpdateSectionConfig(SectionKey, SectionConfig);

    RealtimeMeshComponent->MarkRenderStateDirty();
    
    UE_LOG(LogTemp, Warning, TEXT("🎨 === SMOOTH VERTEX INTERPOLATION MESH CREATION COMPLETE ==="));
    UE_LOG(LogTemp, Log, TEXT("✅ CreateRealtimeMeshFromJUSYNC: Smooth mesh created '%s' (%d verts, %d tris)"),
           *MeshData.ElementName, FinalVertexCount, FinalTriCount);

    return true;
}



bool UJUSYNCSubsystem::BatchCreateRealtimeMeshesFromJUSYNC(const TArray<FJUSYNCMeshData>& MeshDataArray, const TArray<URealtimeMeshComponent*>& MeshComponents)
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
    
    UE_LOG(LogTemp, Log, TEXT("Batch RealtimeMesh Creation: %d/%d successful"), SuccessCount, MeshDataArray.Num());
    return bAllSuccessful;
}


FJUSYNCRealtimeMeshData UJUSYNCSubsystem::ConvertToRealtimeMeshFormat(const FJUSYNCMeshData& StandardMesh)
{
    FJUSYNCRealtimeMeshData RealtimeMesh;
    RealtimeMesh.ElementName = StandardMesh.ElementName;

    // Convert flat arrays to structured vertices
    RealtimeMesh.Vertices.Reserve(StandardMesh.GetVertexCount());

    for (int32 i = 0; i < StandardMesh.GetVertexCount(); ++i)
    {
        FJUSYNCRealtimeMeshVertex Vertex;
        Vertex.Position = StandardMesh.Vertices[i];

        if (i < StandardMesh.Normals.Num())
        {
            Vertex.Normal = StandardMesh.Normals[i];
        }
        else
        {
            Vertex.Normal = FVector::UpVector;
        }

        if (i < StandardMesh.UVs.Num())
        {
            Vertex.UV = StandardMesh.UVs[i];
        }
        else
        {
            Vertex.UV = FVector2D::ZeroVector;
        }

        // ✅ NEW: Handle vertex colors
        if (i < StandardMesh.VertexColors.Num())
        {
            Vertex.Color = StandardMesh.VertexColors[i];
        }
        else
        {
            Vertex.Color = FColor::White;
        }

        RealtimeMesh.Vertices.Add(Vertex);
    }

    RealtimeMesh.Triangles = StandardMesh.Triangles;
    return RealtimeMesh;
}


UTexture2D* UJUSYNCSubsystem::CreateUETextureFromJUSYNC(const FJUSYNCTextureData& TextureData)
{
    if (!TextureData.IsValid())
    {
        return nullptr;
    }
    
    UTexture2D* NewTexture = UTexture2D::CreateTransient(TextureData.Width, TextureData.Height, PF_R8G8B8A8);
    if (!NewTexture)
    {
        return nullptr;
    }
    
    if (NewTexture->GetPlatformData() && NewTexture->GetPlatformData()->Mips.Num() > 0)
    {
        FTexture2DMipMap& Mip = NewTexture->GetPlatformData()->Mips[0];
        void* TextureData_Ptr = Mip.BulkData.Lock(LOCK_READ_WRITE);
        
        if (TextureData_Ptr)
        {
            FMemory::Memcpy(TextureData_Ptr, TextureData.Data.GetData(), TextureData.Data.Num());
            Mip.BulkData.Unlock();
            NewTexture->UpdateResource();
        }
    }
    
    return NewTexture;
}

// ========== REALTIMEMESH SPAWNING IMPLEMENTATION ==========

AActor* UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(const FJUSYNCMeshData& MeshData, const FVector& SpawnLocation, const FRotator& SpawnRotation)
{
    if (!MeshData.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("Invalid mesh data for spawning"));
        return nullptr;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Subsystem not available for spawning"));
        return nullptr;
    }

    UWorld* World = Subsystem->GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("No valid world context for spawning"));
        return nullptr;
    }

    // ✅ CORRECTED: Spawn actor first at origin
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    
    AActor* SpawnedActor = World->SpawnActor<AActor>(SpawnParams);
    if (!SpawnedActor)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to spawn actor"));
        return nullptr;
    }

    // ✅ CORRECTED: Create and set root component FIRST
    URealtimeMeshComponent* MeshComp = NewObject<URealtimeMeshComponent>(SpawnedActor);
    SpawnedActor->SetRootComponent(MeshComp);
    MeshComp->RegisterComponent();
    
    // ✅ CORRECTED: Now set the location AFTER root component is set
    SpawnedActor->SetActorLocation(SpawnLocation);
    SpawnedActor->SetActorRotation(SpawnRotation);

    // Create the mesh using your existing function
    bool bSuccess = CreateRealtimeMeshFromJUSYNC(MeshData, MeshComp);
    
    if (bSuccess)
    {
        // ✅ CORRECTED: Verify the actual location after setting
        FVector ActualLocation = SpawnedActor->GetActorLocation();
        FString Message = FString::Printf(TEXT("✅ RealtimeMesh spawned: %s at %s"), 
                                        *MeshData.ElementName, *ActualLocation.ToString());
        //DisplayDebugMessage(Message, 5.0f, FLinearColor::Green);
        UE_LOG(LogTemp, Warning, TEXT("%s"), *Message);
        return SpawnedActor;
    }
    else
    {
        SpawnedActor->Destroy();
        UE_LOG(LogTemp, Error, TEXT("Failed to create RealtimeMesh, destroying actor"));
        return nullptr;
    }
}


AActor* UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtActor(const FJUSYNCMeshData& MeshData, AActor* TargetActor)
{
    if (!TargetActor)
    {
        UE_LOG(LogTemp, Error, TEXT("Target actor is null"));
        return nullptr;
    }

    FVector SpawnLocation = TargetActor->GetActorLocation();
    FRotator SpawnRotation = TargetActor->GetActorRotation();
    
    return SpawnRealtimeMeshAtLocation(MeshData, SpawnLocation, SpawnRotation);
}

TArray<AActor*> UJUSYNCBlueprintLibrary::BatchSpawnRealtimeMeshesAtLocations(
    const TArray<FJUSYNCMeshData>& MeshDataArray,
    const TArray<FVector>& SpawnLocations,
    const TArray<FRotator>& SpawnRotations,
    bool bUseAsyncSpawning,
    int32 BatchSize,
    float BatchDelay)
{
    // Enhanced validation with rotation support
    UE_LOG(LogTemp, Warning, TEXT("=== BATCH SPAWN DEBUG ==="));
    UE_LOG(LogTemp, Warning, TEXT("MeshDataArray.Num(): %d"), MeshDataArray.Num());
    UE_LOG(LogTemp, Warning, TEXT("SpawnLocations.Num(): %d"), SpawnLocations.Num());
    UE_LOG(LogTemp, Warning, TEXT("SpawnRotations.Num(): %d"), SpawnRotations.Num());
    UE_LOG(LogTemp, Warning, TEXT("Async Mode: %s"), bUseAsyncSpawning ? TEXT("YES") : TEXT("NO"));

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
        UE_LOG(LogTemp, Warning, TEXT("Generated %d default rotations"), FinalRotations.Num());
    }
    else if (FinalRotations.Num() != MeshDataArray.Num())
    {
        UE_LOG(LogTemp, Error, TEXT("❌ Rotation array size mismatch! Expected: %d, Got: %d"), 
               MeshDataArray.Num(), FinalRotations.Num());
        return TArray<AActor*>();
    }

    // If not async, use synchronous method
    if (!bUseAsyncSpawning)
    {
        return BatchSpawnRealtimeMeshesAtLocationsSync(MeshDataArray, SpawnLocations, FinalRotations);
    }

    // Async spawning logic
    UE_LOG(LogTemp, Warning, TEXT("🚀 Starting ASYNC batch spawn with rotations"));
    TSharedPtr<TArray<AActor*>> SharedSpawnedActors = MakeShared<TArray<AActor*>>();
    SharedSpawnedActors->Reserve(MeshDataArray.Num());

    AsyncBatchSpawnInternal(MeshDataArray, SpawnLocations, FinalRotations, SharedSpawnedActors,
                           0, BatchSize, BatchDelay);

    return TArray<AActor*>();
}


TArray<FVector> UJUSYNCBlueprintLibrary::GetSpawnPointLocations(const FString& TagFilter)
{
    TArray<FVector> SpawnLocations;
    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("❌ No JUSYNC Subsystem"));
        return SpawnLocations;
    }

    UWorld* World = Subsystem->GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("❌ No World context"));
        return SpawnLocations;
    }

    // 🔍 DEBUG: Log search parameters
    UE_LOG(LogTemp, Warning, TEXT("=== SEARCHING FOR SPAWN POINTS ==="));
    UE_LOG(LogTemp, Warning, TEXT("Tag Filter: '%s'"), *TagFilter);

    TArray<AActor*> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(World, FName(*TagFilter), FoundActors);
    
    UE_LOG(LogTemp, Warning, TEXT("Found %d actors with tag '%s'"), FoundActors.Num(), *TagFilter);

    SpawnLocations.Reserve(FoundActors.Num());
    for (int32 i = 0; i < FoundActors.Num(); ++i)
    {
        AActor* Actor = FoundActors[i];
        if (Actor)
        {
            FVector Location = Actor->GetActorLocation();
            SpawnLocations.Add(Location);
            UE_LOG(LogTemp, Warning, TEXT("SpawnPoint[%d]: %s at %s"), 
                   i, *Actor->GetName(), *Location.ToString());
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("=== TOTAL SPAWN POINTS: %d ==="), SpawnLocations.Num());
    return SpawnLocations;
}



