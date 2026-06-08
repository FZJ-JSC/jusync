#include "JUSYNCSubsystem.h"
#include "JUSYNCBlueprintLibrary.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture.h"
#include "TextureResource.h" 
#include "RenderUtils.h"
#include "RealtimeMeshComponent.h"
#include "JUSYNCModule.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "RealtimeMeshSimple.h" 
#include "UObject/UObjectGlobals.h"  // For MakeUniqueObjectName
#include "Misc/DateTime.h"  // For FDateTime
#include "HAL/PlatformTime.h"  // For FPlatformTime
#include "HAL/PlatformProcess.h"  // For FPlatformProcess::Sleep
#include "HAL/PlatformMisc.h"     // For FPlatformMisc::NumberOfCoresIncludingHyperthreads
#include "Async/ParallelFor.h"    // For ParallelFor
#include "GameFramework/Actor.h"   // For SpawnActor
#include "JUSYNCPointCloudSpawner.h"
#ifdef WITH_ANARI_USD_MIDDLEWARE
#include "LidarPointCloud.h"        // For ULidarPointCloud
#include "LidarPointCloudComponent.h"
#include "LidarPointCloudActor.h"
#endif
#include <atomic>  // For std::atomic

// Include the C-wrapper header
#ifdef WITH_ANARI_USD_MIDDLEWARE
extern "C" {
#include "AnariUsdMiddleware_C.h"
}
#include <cstdint>  // For uint32_t
#endif

// Global callback handlers for C interface
// Use atomic for thread-safe access from ZMQ callback threads
static std::atomic<UJUSYNCSubsystem*> g_SubsystemInstance = nullptr;

#ifdef WITH_ANARI_USD_MIDDLEWARE

// Thread-safe set for tracking processed files to avoid duplicates
static TSet<FString> ProcessedFiles;
static FCriticalSection ProcessedFilesCriticalSection;

// Enhanced callback functions with detailed debugging
extern "C" void FileReceivedCallback_Static(const CFileData* file_data)
{
    UE_LOG(LogJUSYNC, Log, TEXT("=== ZMQ CALLBACK TRIGGERED ==="));
    
    if (!file_data)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("FileReceivedCallback_Static: NULL file_data received"));
        return;
    }
    
    FString Filename = UTF8_TO_TCHAR(file_data->filename);
    
    // Check for duplicate files (broadcast sends same file from multiple ranks)
    {
        FScopeLock Lock(&ProcessedFilesCriticalSection);
        if (ProcessedFiles.Contains(Filename))
        {
            UE_LOG(LogJUSYNC, Log, TEXT("Skipping duplicate file: %s (already processed)"), *Filename);
            return;
        }
        ProcessedFiles.Add(Filename);
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("ZMQ File Received:"));
    UE_LOG(LogJUSYNC, Log, TEXT("  - Filename: %s"), *Filename);
    UE_LOG(LogJUSYNC, Log, TEXT("  - File Type: %s"), UTF8_TO_TCHAR(file_data->file_type));
    UE_LOG(LogJUSYNC, Log, TEXT("  - Data Size: %d bytes"), file_data->data_size);
    UE_LOG(LogJUSYNC, Log, TEXT("  - Hash: %s"), UTF8_TO_TCHAR(file_data->hash));
    
    UJUSYNCSubsystem* Subsystem = g_SubsystemInstance.load();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("FileReceivedCallback_Static: g_SubsystemInstance is NULL"));
        return;
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("Creating async task for file processing..."));
    
    // Create a DEEP copy of the data for the lambda to avoid use-after-free
    // We need to copy all C strings and binary data since the original may be freed
    // Create a custom structure to hold the copied data
    struct LocalFileData
    {
        char* filename = nullptr;
        char* hash = nullptr;
        char* file_type = nullptr;
        unsigned char* data = nullptr;
        size_t data_size = 0;
        
        ~LocalFileData()
        {
            if (filename) delete[] filename;
            if (hash) delete[] hash;
            if (file_type) delete[] file_type;
            if (data) delete[] data;
        }
    };
    
    LocalFileData LocalData;
    
    // Copy filename (C string)
    if (file_data->filename && file_data->filename[0] != '\0') {
        size_t filename_len = strlen(file_data->filename) + 1;
        LocalData.filename = new char[filename_len];
        strcpy_s(LocalData.filename, filename_len, file_data->filename);
    }
    
    // Copy hash (C string)
    if (file_data->hash && file_data->hash[0] != '\0') {
        size_t hash_len = strlen(file_data->hash) + 1;
        LocalData.hash = new char[hash_len];
        strcpy_s(LocalData.hash, hash_len, file_data->hash);
    }
    
    // Copy file_type (C string)
    if (file_data->file_type && file_data->file_type[0] != '\0') {
        size_t file_type_len = strlen(file_data->file_type) + 1;
        LocalData.file_type = new char[file_type_len];
        strcpy_s(LocalData.file_type, file_type_len, file_data->file_type);
    }
    
    // Copy binary data
    LocalData.data_size = file_data->data_size;
    if (file_data->data && file_data->data_size > 0) {
        LocalData.data = new unsigned char[file_data->data_size];
        std::memcpy(LocalData.data, file_data->data, file_data->data_size);
    }
    
    
    AsyncTask(ENamedThreads::GameThread, [LocalData]()
    {
        UE_LOG(LogJUSYNC, Log, TEXT("=== ASYNC TASK EXECUTING ON GAME THREAD ==="));
        
        UJUSYNCSubsystem* Subsystem = g_SubsystemInstance.load();
        if (!Subsystem)
        {
            UE_LOG(LogJUSYNC, Error, TEXT("Async Task: g_SubsystemInstance is NULL on game thread"));
            return;
        }
        
        UE_LOG(LogJUSYNC, Log, TEXT("Converting C data to UE format..."));
        
        FJUSYNCFileData UEFileData;
        UEFileData.Filename = LocalData.filename ? FString(UTF8_TO_TCHAR(LocalData.filename)) : TEXT("");
        UEFileData.Hash = LocalData.hash ? FString(UTF8_TO_TCHAR(LocalData.hash)) : TEXT("");
        UEFileData.FileType = LocalData.file_type ? FString(UTF8_TO_TCHAR(LocalData.file_type)) : TEXT("");
        
        if (LocalData.data && LocalData.data_size > 0) {
            UEFileData.Data.SetNum(LocalData.data_size);
            FMemory::Memcpy(UEFileData.Data.GetData(), LocalData.data, LocalData.data_size);
        }
        
        UE_LOG(LogJUSYNC, Log, TEXT("Broadcasting to Blueprint events..."));
        UE_LOG(LogJUSYNC, Log, TEXT("  - UE Filename: %s"), *UEFileData.Filename);
        UE_LOG(LogJUSYNC, Log, TEXT("  - UE File Type: %s"), *UEFileData.FileType);
        UE_LOG(LogJUSYNC, Log, TEXT("  - UE Data Size: %d"), UEFileData.Data.Num());
        
        // Send to Blueprint Library FIRST
        Subsystem->HandleFileReceivedForLibrary(UEFileData);
        
        // Broadcast to subsystem events
        Subsystem->OnFileReceived.Broadcast(UEFileData);
        
        UE_LOG(LogJUSYNC, Log, TEXT("=== FILE PROCESSING COMPLETE ==="));
    });
}

extern "C" void MessageReceivedCallback_Static(const char* message)
{
    UE_LOG(LogJUSYNC, Log, TEXT("=== ZMQ MESSAGE CALLBACK TRIGGERED ==="));
    
    if (!message)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("MessageReceivedCallback_Static: NULL message received"));
        return;
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("ZMQ Message: %s"), UTF8_TO_TCHAR(message));
    
    // Create a copy of the message for the lambda (deep copy to avoid use-after-free)
    FString MessageCopy = FString(UTF8_TO_TCHAR(message));
    
    AsyncTask(ENamedThreads::GameThread, [MessageCopy]()
    {
        UJUSYNCSubsystem* Subsystem = g_SubsystemInstance.load();
        if (Subsystem)
        {
            UE_LOG(LogJUSYNC, Log, TEXT("Broadcasting message to Blueprint: %s"), *MessageCopy);
            Subsystem->OnMessageReceived.Broadcast(MessageCopy);
            Subsystem->HandleMessageReceivedForLibrary(MessageCopy);
        }
        else
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("MessageReceivedCallback_Static: Subsystem instance is null, message dropped: %s"), *MessageCopy);
        }
    });
}

extern "C" void NotificationCallback_Static(uint32_t messageType, int32_t sourceRank,
                                             const char* filename, uint64_t fileSize, uint64_t timestamp)
{
    FString NotifType = (messageType == 301) ? TEXT("NOTIFY_COMMIT_COMPLETE") : TEXT("NOTIFY_FILE_UPDATE");
    UE_LOG(LogJUSYNC, Display, TEXT("=== ZMQ NOTIFICATION: %s (rank %d, file '%s', %llu bytes) ==="),
           *NotifType, sourceRank, filename ? UTF8_TO_TCHAR(filename) : TEXT("(none)"),
           static_cast<unsigned long long>(timestamp));

    FString FilenameCopy = filename ? FString(UTF8_TO_TCHAR(filename)) : TEXT("");

    AsyncTask(ENamedThreads::GameThread, [FilenameCopy, messageType, sourceRank, fileSize, timestamp]()
    {
        UJUSYNCSubsystem* Subsystem = g_SubsystemInstance.load();
        if (!Subsystem)
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("NotificationCallback_Static: Subsystem instance is null, notification dropped"));
            return;
        }

        FJUSYNCNotification Notification;
        Notification.Type = (messageType == 301) ? EJUSYNCNotificationType::CommitComplete : EJUSYNCNotificationType::FileUpdate;
        Notification.SourceRank = sourceRank;
        Notification.Filename = FilenameCopy;
        Notification.FileSize = static_cast<int64>(fileSize);
        Notification.Timestamp = static_cast<int64>(timestamp);

        UE_LOG(LogJUSYNC, Log, TEXT("Broadcasting notification to Blueprint: %s '%s'"),
               (Notification.Type == EJUSYNCNotificationType::CommitComplete) ? TEXT("CommitComplete") : TEXT("FileUpdate"),
               *Notification.Filename);
        Subsystem->OnNotificationReceived.Broadcast(Notification);
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

    // 2. Convert points (OPTIMIZED - flat array â†’ FVector array)
    size_t PointCount = CMesh.points_count / 3;
    UEMesh.Vertices.Reserve(PointCount);
    
    // Optimized loop with better cache locality
    const float* pointsPtr = CMesh.points;
    for (size_t i = 0; i < PointCount; ++i)
    {
        size_t idx = i * 3;
        // Transform from right-handed Z-up (ParaView) to left-handed Z-up (UE)
        UEMesh.Vertices.Add(FVector(pointsPtr[idx], -pointsPtr[idx + 1], pointsPtr[idx + 2]));
    }

    // 3. Convert triangle indices (OPTIMIZED)
    size_t IndexCount = CMesh.indices_count;
    UEMesh.Triangles.Reserve(IndexCount);
    
    // Optimized bulk conversion
    const uint32_t* indicesPtr = CMesh.indices;
    UEMesh.Triangles.AddUninitialized(IndexCount);
    for (size_t i = 0; i < IndexCount; ++i)
    {
        UEMesh.Triangles[i] = static_cast<int32>(indicesPtr[i]);
    }

    // 4. Convert normals if present (OPTIMIZED)
    if (CMesh.normals && CMesh.normals_count >= 3)
    {
        size_t NormalCount = CMesh.normals_count / 3;
        UEMesh.Normals.Reserve(NormalCount);
        
        // Optimized loop with direct pointer access
        const float* normalsPtr = CMesh.normals;
        for (size_t i = 0; i < NormalCount; ++i)
        {
            size_t idx = i * 3;
            UEMesh.Normals.Add(FVector(normalsPtr[idx], -normalsPtr[idx + 1], normalsPtr[idx + 2]).GetSafeNormal());
        }
    }

    // 5. Convert UVs if present (OPTIMIZED)
    if (CMesh.uvs && CMesh.uvs_count >= 2)
    {
        size_t UVCount = CMesh.uvs_count / 2;
        UEMesh.UVs.Reserve(UVCount);
        
        // Optimized loop with direct pointer access
        const float* uvsPtr = CMesh.uvs;
        for (size_t i = 0; i < UVCount; ++i)
        {
            size_t idx = i * 2;
            UEMesh.UVs.Add(FVector2D(uvsPtr[idx], uvsPtr[idx + 1]));
        }
    }

    // 6. âœ… ENHANCED: Vertexâ€colors with FORCED vertex interpolation while preserving all functionality
    if (CMesh.vertex_colors && CMesh.vertex_colors_count >= 4)
    {
        int32 VertexCount = static_cast<int32>(PointCount);
        int32 FaceCount = static_cast<int32>(UEMesh.Triangles.Num() / 3);
        int32 ColorCount = static_cast<int32>(CMesh.vertex_colors_count / 4);

        bool bDetectedVertexInterp = (ColorCount == VertexCount);
        bool bDetectedUniformInterp = (ColorCount == FaceCount);
        
        UEMesh.VertexColors.Reserve(VertexCount);

        if (bDetectedVertexInterp)
        {
            // âœ… CASE 1: Already vertex interpolation - direct mapping (OPTIMIZED)
            const float* colorsPtr = CMesh.vertex_colors;
            for (int32 i = 0; i < VertexCount; ++i)
            {
                int64 idx = int64(i) * 4;
                uint8 r = uint8(FMath::Clamp(colorsPtr[idx + 0] * 255.0f, 0.0f, 255.0f));
                uint8 g = uint8(FMath::Clamp(colorsPtr[idx + 1] * 255.0f, 0.0f, 255.0f));
                uint8 b = uint8(FMath::Clamp(colorsPtr[idx + 2] * 255.0f, 0.0f, 255.0f));
                uint8 a = uint8(FMath::Clamp(colorsPtr[idx + 3] * 255.0f, 0.0f, 255.0f));
                UEMesh.VertexColors.Add(FColor(r, g, b, a));
            }
        }
        else if (bDetectedUniformInterp && bForceVertexInterpolation)
        {
            // âœ… CASE 2: Uniform detected + Force Vertex = Convert uniform to smooth vertex interpolation
            // UE_LOG(LogJUSYNC, Log, TEXT("ðŸŽ¨ CONVERTING uniform to smooth VERTEX interpolation"));
            
            // Initialize vertex color accumulation arrays
            TArray<FLinearColor> AccumulatedColors;
            TArray<int32> ColorCounts;
            AccumulatedColors.SetNumZeroed(VertexCount);
            ColorCounts.SetNumZeroed(VertexCount);
            
            // âœ… ENHANCED: Accumulate colors from all faces that use each vertex (for smooth blending)
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
                    
                    // âœ… SMOOTH BLENDING: Accumulate this face color to all three vertices
                    AccumulatedColors[i0] += FaceColor;
                    AccumulatedColors[i1] += FaceColor;
                    AccumulatedColors[i2] += FaceColor;
                    
                    ColorCounts[i0]++;
                    ColorCounts[i1]++;
                    ColorCounts[i2]++;
                }
            }
            
            // âœ… FINALIZE: Average the accumulated colors and convert to FColor
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
            
            UE_LOG(LogJUSYNC, Log, TEXT("âœ… Converted uniform to smooth vertex interpolation: %d vertex colors"), 
                   UEMesh.VertexColors.Num());
        }
        else if (bDetectedUniformInterp && !bForceVertexInterpolation)
        {
            // âœ… CASE 3: Keep original uniform behavior (PRESERVED for backwards compatibility)
            UE_LOG(LogJUSYNC, Log, TEXT("ðŸŽ¨ Using original UNIFORM interpolation (flat shading)"));
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
            // âœ… CASE 4: Fallback behavior (PRESERVED)
            UE_LOG(LogJUSYNC, Warning, TEXT("ðŸŽ¨ Using fallback vertex interpolation"));
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

        // âœ… PRESERVED: Debug logging for color verification
        UE_LOG(LogJUSYNC, Log, TEXT("ðŸŽ¨ Final vertex colors: %d"), UEMesh.VertexColors.Num());
        
        // âœ… PRESERVED: DEBUG dump first 20 different colours
        TSet<FColor> Unique;
        for (int32 i = 0; i < UEMesh.VertexColors.Num(); ++i)
        {
            const FColor& C = UEMesh.VertexColors[i];
            if (!Unique.Contains(C))
            {
                Unique.Add(C);
                UE_LOG(LogJUSYNC, Log, TEXT("USD Color[%d] = (R=%d G=%d B=%d A=%d)"),
                       i, C.R, C.G, C.B, C.A);
                if (Unique.Num() == 20) break;
            }
        }
        UE_LOG(LogJUSYNC, Log, TEXT("Total unique colours in first scan: %d"), Unique.Num());
    }

    return UEMesh;
}


#endif

void UJUSYNCSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    
    // Set global instance for callbacks
    g_SubsystemInstance.store(this);

    // Initialize async point cloud spawner
    PCSpawner = MakeUnique<FJUSYNCPointCloudSpawner>(TWeakObjectPtr<UJUSYNCSubsystem>(this));
    PCSpawner->SetMaxPoolSize(16);
    PCSpawner->SetBudgetMs(500.0f);

    // Set LiDAR point budget to handle many simultaneous point clouds (100M+ points)
    // Prevents the LOD manager from culling distant clouds due to adaptive budget scaling
    if (GEngine)
    {
        GEngine->Exec(nullptr, TEXT("r.LidarPointBudget 200000000"));
    }

    UE_LOG(LogJUSYNC, Log, TEXT("=== JUSYNC SUBSYSTEM INITIALIZED ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Global instance set: %p"), g_SubsystemInstance.load());
    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNCSubsystem initialized with C-wrapper interface"));
}

void UJUSYNCSubsystem::Deinitialize()
{
    UE_LOG(LogJUSYNC, Log, TEXT("=== JUSYNC SUBSYSTEM DEINITIALIZING ==="));
    
    ShutdownMiddleware();
    
    // Clear global instance
    g_SubsystemInstance.store(nullptr);
    
    Super::Deinitialize();
    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNCSubsystem deinitialized"));
}

bool UJUSYNCSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    return true;
}

bool UJUSYNCSubsystem::InitializeMiddleware(const FString& Endpoint)
{
    FScopeLock Lock(&MiddlewareMutex);
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== INITIALIZING JUSYNC MIDDLEWARE ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Requested Endpoint: %s"), *Endpoint);
    UE_LOG(LogJUSYNC, Log, TEXT("Subsystem instance: %p"), this);
    UE_LOG(LogJUSYNC, Log, TEXT("Global instance: %p"), g_SubsystemInstance.load());

    // Ensure global instance is set
    if (!g_SubsystemInstance.load())
    {
        g_SubsystemInstance.store(this);
        UE_LOG(LogJUSYNC, Log, TEXT("Set global instance: %p"), g_SubsystemInstance.load());
    }
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    // Convert FString to C string
    FTCHARToUTF8 EndpointConverter(*Endpoint);
    const char* EndpointCStr = Endpoint.IsEmpty() ? "tcp://*:5556" : EndpointConverter.Get();
    
    UE_LOG(LogJUSYNC, Log, TEXT("Using C endpoint: %s"), UTF8_TO_TCHAR(EndpointCStr));
    
    // Initialize middleware using C interface (g_middleware is created here)
    UE_LOG(LogJUSYNC, Log, TEXT("Calling InitializeMiddleware_C..."));
    int Result = InitializeMiddleware_C(EndpointCStr);
    
    UE_LOG(LogJUSYNC, Log, TEXT("InitializeMiddleware_C returned: %d"), Result);
    
    bIsInitialized.store(Result == 1);
    
    if (Result == 1)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… USD processors initialized (DEALER-only mode)"));
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Ready to connect to broker via DEALER socket"));
        
        // Register callbacks AFTER initialization (g_middleware must exist)
        UE_LOG(LogJUSYNC, Log, TEXT("Registering ZMQ callbacks..."));
        RegisterUpdateCallback_C(FileReceivedCallback_Static);
        RegisterMessageCallback_C(MessageReceivedCallback_Static);
        RegisterNotificationCallback_C(NotificationCallback_Static);
        UE_LOG(LogJUSYNC, Log, TEXT(" Callbacks registered (file, message, notification)"));
        
        // Test connection status
        int ConnectionStatus = IsConnected_C();
        UE_LOG(LogJUSYNC, Log, TEXT("Connection status check: %d"), ConnectionStatus);
        
        // Get status info
        const char* StatusInfo = GetStatusInfo_C();
        UE_LOG(LogJUSYNC, Log, TEXT("Middleware status: %s"), UTF8_TO_TCHAR(StatusInfo));
        
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Middleware initialized successfully (DEALER-only mode)"));
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to initialize JUSYNC Middleware (Result: %d)"), Result);
    }
    
    return Result == 1;
#else
    UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC compiled without middleware support"));
    return false;
#endif
}

void UJUSYNCSubsystem::ShutdownMiddleware()
{
    FScopeLock Lock(&MiddlewareMutex);
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== SHUTTING DOWN JUSYNC MIDDLEWARE ==="));
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    // Clear global instance BEFORE shutting down to prevent callbacks from using destroyed instance
    if (g_SubsystemInstance.load() == this)
    {
        g_SubsystemInstance.store(nullptr);
        UE_LOG(LogJUSYNC, Log, TEXT("Cleared global subsystem instance"));
    }
    
    ShutdownMiddleware_C();
    bIsInitialized.store(false);
    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Middleware shutdown"));
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
        UE_LOG(LogJUSYNC, Log, TEXT("Connection status: %s (Check #%d)"), 
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
    
    UE_LOG(LogJUSYNC, Log, TEXT("Status info requested: %s"), *Status);
    return Status;
#else
    return TEXT("Middleware not available");
#endif
}

bool UJUSYNCSubsystem::StartReceiving()
{
    FScopeLock Lock(&MiddlewareMutex);
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== STARTING JUSYNC RECEIVING ==="));
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot start receiving - middleware not initialized"));
        return false;
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("Middleware is initialized, calling StartReceiving_C..."));
    
    int Result = StartReceiving_C();
    
    UE_LOG(LogJUSYNC, Log, TEXT("StartReceiving_C returned: %d"), Result);
    
    if (Result == 1)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… JUSYNC Started Receiving Data"));
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… ROUTER is now listening for DEALER messages"));
        
        // Additional status checks
        int ConnectionStatus = IsConnected_C();
        UE_LOG(LogJUSYNC, Log, TEXT("Post-start connection status: %d"), ConnectionStatus);
        
        const char* StatusInfo = GetStatusInfo_C();
        UE_LOG(LogJUSYNC, Log, TEXT("Post-start middleware status: %s"), UTF8_TO_TCHAR(StatusInfo));
        
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Start receiving: Success"));
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to start receiving (Result: %d)"), Result);
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Start receiving: Failed"));
    }
    
    return Result == 1;
#endif
    return false;
}



void UJUSYNCSubsystem::StopReceiving()
{
    FScopeLock Lock(&MiddlewareMutex);
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== STOPPING JUSYNC RECEIVING ==="));
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    StopReceiving_C();
    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Stopped receiving"));
#endif
}

void UJUSYNCSubsystem::HandleFileReceivedForLibrary(const FJUSYNCFileData& FileData)
{
    UE_LOG(LogJUSYNC, Log, TEXT("=== ADDING FILE TO BLUEPRINT LIBRARY ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("File: %s (%d bytes, %s)"), 
           *FileData.Filename, FileData.Data.Num(), *FileData.FileType);
    
    FScopeLock Lock(&UJUSYNCBlueprintLibrary::DataMutex);
    
    int32 PreviousCount = UJUSYNCBlueprintLibrary::ReceivedFiles.Num();
    UJUSYNCBlueprintLibrary::ReceivedFiles.Add(FileData);
    int32 NewCount = UJUSYNCBlueprintLibrary::ReceivedFiles.Num();
    
    UE_LOG(LogJUSYNC, Log, TEXT("Blueprint Library file count: %d -> %d"), PreviousCount, NewCount);
    UE_LOG(LogJUSYNC, Log, TEXT("âœ… File added to Blueprint Library: %s"), *FileData.Filename);
}

void UJUSYNCSubsystem::HandleMessageReceivedForLibrary(const FString& Message)
{
    UE_LOG(LogJUSYNC, Log, TEXT("=== ADDING MESSAGE TO BLUEPRINT LIBRARY ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Message: %s"), *Message);
    
    FScopeLock Lock(&UJUSYNCBlueprintLibrary::DataMutex);
    
    int32 PreviousCount = UJUSYNCBlueprintLibrary::ReceivedMessages.Num();
    UJUSYNCBlueprintLibrary::ReceivedMessages.Add(Message);
    int32 NewCount = UJUSYNCBlueprintLibrary::ReceivedMessages.Num();
    
    UE_LOG(LogJUSYNC, Log, TEXT("Blueprint Library message count: %d -> %d"), PreviousCount, NewCount);
    UE_LOG(LogJUSYNC, Log, TEXT("Message received for Blueprint Library: %s"), *Message);
}

bool UJUSYNCSubsystem::LoadUSDFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCMeshData>& OutMeshData)
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Middleware not initialized"));
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
        
        UE_LOG(LogJUSYNC, Log, TEXT("Successfully loaded %d meshes from USD buffer"), OutMeshData.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to load USD from buffer"));
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
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Middleware not initialized"));
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
        
        UE_LOG(LogJUSYNC, Log, TEXT("Successfully loaded %d meshes from USD file"), OutMeshData.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to load USD from disk"));
        if (CMeshes)
        {
            FreeMeshData_C(CMeshes, MeshCount);
        }
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::LoadUSDFullFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCMeshData>& OutMeshData, TArray<FJUSYNCPointCloudData>& OutPointCloudData)
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC: LoadUSDFullFromBuffer called but middleware is not initialized"));
        return false;
    }

    OutMeshData.Empty();
    OutPointCloudData.Empty();

    if (Buffer.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadUSDFullFromBuffer called with empty buffer"));
        return false;
    }

    FScopeLock Lock(&MiddlewareMutex);

    FTCHARToUTF8 FilenameConverter(*Filename);
    const char* FilenameCStr = FilenameConverter.Get();

    CMeshData* CMeshes = nullptr;
    size_t MeshCount = 0;
    CPointCloudData* CClouds = nullptr;
    size_t CloudCount = 0;

    int Result = LoadUSDFull_C(
        Buffer.GetData(), Buffer.Num(), FilenameCStr,
        &CMeshes, &MeshCount,
        &CClouds, &CloudCount
    );

    bool bSuccess = Result == 1;

    if (!bSuccess)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadUSDFullFromBuffer failed for '%s' (buffer=%d bytes, C_result=%d MeshCount=%llu CloudCount=%llu)"),
            *Filename, Buffer.Num(), Result, (uint64)MeshCount, (uint64)CloudCount);
        if (CMeshes) FreeMeshData_C(CMeshes, MeshCount);
        if (CClouds) FreePointCloudData_C(CClouds, CloudCount);
        return false;
    }

    // Convert meshes
    if (CMeshes && MeshCount > 0)
    {
        OutMeshData.Reserve(MeshCount);
        for (size_t i = 0; i < MeshCount; ++i)
        {
            FJUSYNCMeshData UEMeshData = ConvertCMeshDataToUE_Helper(CMeshes[i]);
            OutMeshData.Add(UEMeshData);
        }
        FreeMeshData_C(CMeshes, MeshCount);
    }

    // Convert point clouds
    if (CClouds && CloudCount > 0)
    {
        OutPointCloudData.SetNum(static_cast<int32>(CloudCount));
        for (size_t i = 0; i < CloudCount; ++i)
        {
            FJUSYNCPointCloudData& pc = OutPointCloudData[i];
            const CPointCloudData& cpc = CClouds[i];

            pc.ElementName = ANSI_TO_TCHAR(cpc.element_name);
            pc.TypeName = ANSI_TO_TCHAR(cpc.type_name);
            pc.PointCount = static_cast<int32>(cpc.points_count);
            pc.bHasColors = cpc.has_colors != 0;
            pc.bHasNormals = cpc.has_normals != 0;
            pc.BoundingBoxMin = FVector(cpc.bounding_box_min[0], cpc.bounding_box_min[1], cpc.bounding_box_min[2]);
            pc.BoundingBoxMax = FVector(cpc.bounding_box_max[0], cpc.bounding_box_max[1], cpc.bounding_box_max[2]);

            if (cpc.points_count > 0 && cpc.positions)
            {
                pc.Positions.SetNum(pc.PointCount);
                for (size_t j = 0; j < cpc.points_count; ++j)
                {
                    float usdX = cpc.positions[j * 3 + 0];
                    float usdY = cpc.positions[j * 3 + 1];
                    float usdZ = cpc.positions[j * 3 + 2];
                    pc.Positions[j] = FVector(usdX, usdZ, -usdY);
                }
            }

            if (cpc.has_colors && cpc.colors && pc.PointCount > 0)
            {
                pc.Colors.SetNum(pc.PointCount);
                for (int32 j = 0; j < pc.PointCount; ++j)
                {
                    float r = cpc.colors[j * 4 + 0] * 255.0f;
                    float g = cpc.colors[j * 4 + 1] * 255.0f;
                    float b = cpc.colors[j * 4 + 2] * 255.0f;
                    float a = cpc.colors[j * 4 + 3] * 255.0f;
                    pc.Colors[j] = FColor(static_cast<uint8>(r), static_cast<uint8>(g), static_cast<uint8>(b), static_cast<uint8>(a));
                }
            }

            if (cpc.has_widths && cpc.widths)
            {
                pc.Widths.SetNum(pc.PointCount);
                std::memcpy(pc.Widths.GetData(), cpc.widths, pc.PointCount * sizeof(float));
            }
        }
        FreePointCloudData_C(CClouds, CloudCount);
    }

    UE_LOG(LogTemp, Log, TEXT("JUSYNC: LoadUSDFullFromBuffer: %d meshes + %d point clouds from '%s' (single-pass)"),
           OutMeshData.Num(), OutPointCloudData.Num(), *Filename);
    return true;
#else
    UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadUSDFullFromBuffer called but middleware not available"));
    return false;
#endif
}

FJUSYNCTextureData UJUSYNCSubsystem::CreateTextureFromBuffer(const TArray<uint8>& Buffer)
{
    FJUSYNCTextureData Result;
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Middleware not initialized"));
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
        
        UE_LOG(LogJUSYNC, Log, TEXT("Created texture: %dx%d (%d channels)"), Result.Width, Result.Height, Result.Channels);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to create texture from buffer"));
    }
#endif
    
    return Result;
}

bool UJUSYNCSubsystem::WriteGradientLineAsPNG(const TArray<uint8>& Buffer, const FString& OutputPath)
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Middleware not initialized"));
        return false;
    }
    
    // Convert FString to C string
    FTCHARToUTF8 OutputPathConverter(*OutputPath);
    const char* OutputPathCStr = OutputPathConverter.Get();
    
    // Call C interface
    int Result = WriteGradientLineAsPNG_C(Buffer.GetData(), Buffer.Num(), OutputPathCStr);
    
    if (Result == 1)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Gradient PNG saved: %s"), *OutputPath);
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to save gradient PNG"));
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::GetGradientLineAsPNGBuffer(const TArray<uint8>& Buffer, TArray<uint8>& OutPNGBuffer)
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Middleware not initialized"));
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
        
        UE_LOG(LogJUSYNC, Log, TEXT("Gradient PNG buffer created: %d bytes"), OutPNGBuffer.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to create gradient PNG buffer"));
        if (PNGData)
        {
            FreeBuffer_C(PNGData);
        }
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::GetPNGDimensions(const TArray<uint8>& Buffer, int32& OutWidth, int32& OutHeight, int32& OutChannels)
{
    OutWidth = 0;
    OutHeight = 0;
    OutChannels = 0;

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Middleware not initialized"));
        return false;
    }
    
    int width = 0, height = 0, channels = 0;
    
    // Call C interface
    int Result = GetPNGDimensions_C(Buffer.GetData(), Buffer.Num(), &width, &height, &channels);
    
    if (Result == 1 && width > 0 && height > 0 && channels > 0)
    {
        OutWidth = width;
        OutHeight = height;
        OutChannels = channels;
        
        UE_LOG(LogJUSYNC, Log, TEXT("PNG dimensions: %dx%d (%d channels)"), OutWidth, OutHeight, OutChannels);
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to get PNG dimensions"));
    }
#endif
    return false;
}

void UJUSYNCSubsystem::ApplyCachedGradientToSpawner()
{
#ifndef WITH_ANARI_USD_MIDDLEWARE
    return;
#endif

    if (!bIsInitialized.load()) return;

    unsigned char* gradientData = nullptr;
    size_t gradientSize = 0;
    int width = 0, height = 0;

    int Result = GetCachedGradientTexture_C(&gradientData, &gradientSize, &width, &height);
    if (Result != 1 || !gradientData || gradientSize == 0 || width < 1 || height < 1)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC: No cached gradient texture in middleware"));
        return;
    }

    // Build LUT from top row (first width pixels, RGBA)
    TArray<FColor> LUT;
    const int entries = FMath::Min(width, 256);
    LUT.Reserve(entries);
    for (int x = 0; x < entries; ++x)
    {
        const uint8* px = gradientData + x * 4;
        LUT.Add(FColor(px[0], px[1], px[2], px[3]));
    }

    FreeCachedGradientTexture_C(gradientData);

    UE_LOG(LogJUSYNC, Display, TEXT("[Gradients] LUT built from cached gradient: %d colors"), LUT.Num());

    FJUSYNCPointCloudSpawner* Sp = PCSpawner.Get();
    if (Sp)
    {
        Sp->SetGradientLUT(LUT);
    }
}

bool UJUSYNCSubsystem::GetImageRowAsPNGBuffer(const TArray<uint8>& Buffer, int32 RowIndex, TArray<uint8>& OutPNGBuffer)
{
    OutPNGBuffer.Empty();

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Middleware not initialized"));
        return false;
    }
    
    unsigned char* out_buffer = nullptr;
    size_t out_size = 0;
    
    // Call C interface
    int Result = GetImageRowAsPNGBuffer_C(Buffer.GetData(), Buffer.Num(), RowIndex, &out_buffer, &out_size);
    
    if (Result == 1 && out_buffer && out_size > 0)
    {
        // Copy to output array
        OutPNGBuffer.SetNum(out_size);
        FMemory::Memcpy(OutPNGBuffer.GetData(), out_buffer, out_size);
        
        // Free the C-allocated buffer using middleware function
        FreeBuffer_C(out_buffer);
        
        UE_LOG(LogJUSYNC, Log, TEXT("Extracted row %d as PNG buffer: %d bytes"), RowIndex, out_size);
        return true;
    }
    else
    {
        if (out_buffer)
        {
            FreeBuffer_C(out_buffer);
        }
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to extract row %d as PNG buffer"), RowIndex);
    }
#endif
    return false;
}

void UJUSYNCSubsystem::ClearProcessedFiles()
{
    FScopeLock Lock(&ProcessedFilesCriticalSection);
    ProcessedFiles.Empty();
    UE_LOG(LogJUSYNC, Log, TEXT("Cleared processed files cache"));
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
            
            // âœ… FIXED: Calculate normal for counter-clockwise winding (v2-v0 x v1-v0)
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
    
    UE_LOG(LogJUSYNC, Log, TEXT("Recalculated normals with correct CCW winding"));
}

bool UJUSYNCSubsystem::CreateRealtimeMeshFromJUSYNC(
    const FJUSYNCMeshData& InMeshData,
    URealtimeMeshComponent* RealtimeMeshComponent)
{
    // Enhanced safety checks
    if (!RealtimeMeshComponent || !RealtimeMeshComponent->IsValidLowLevel())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Invalid or destroyed RealtimeMeshComponent"));
        return false;
    }

    if (!InMeshData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Invalid mesh data"));
        return false;
    }

    // Use the mesh data as-is (already processed by ConvertCMeshDataToUE_Helper with forced vertex interpolation)
    const FJUSYNCMeshData& MeshData = InMeshData;
    
    // Additional validation of mesh data arrays
    if (MeshData.Vertices.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Mesh has no vertices"));
        return false;
    }

    if (MeshData.Triangles.Num() < 3 || (MeshData.Triangles.Num() % 3) != 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Invalid triangle count: %d (must be multiple of 3)"), MeshData.Triangles.Num());
        return false;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("ðŸŽ¨ === SMOOTH VERTEX INTERPOLATION MESH CREATION ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Mesh: %d vertices, %d triangles, %d colors"),
           MeshData.Vertices.Num(), MeshData.Triangles.Num() / 3, MeshData.VertexColors.Num());

    // Calculate final counts (already processed by helper function)
    const int32 FinalVertexCount = MeshData.Vertices.Num();
    const int32 FinalTriCount = MeshData.Triangles.Num() / 3;

    // Safety: Ensure we don't exceed reasonable limits
    if (FinalVertexCount > 1000000)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("âš ï¸ Very large mesh: %d vertices (consider splitting)"), FinalVertexCount);
    }

    // Initialize RealtimeMesh builder
    URealtimeMeshSimple* RealtimeMesh = RealtimeMeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
    if (!RealtimeMesh)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to initialize RealtimeMesh"));
        return false;
    }

    RealtimeMesh->SetupMaterialSlot(0, TEXT("PrimaryMaterial"));

    // Only apply vertex color material if no material is already set
    if (!RealtimeMeshComponent->GetMaterial(0))
    {
        // Use cached material instead of loading synchronously each time
        UMaterialInterface* VertexColorMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"));
        if (VertexColorMaterial)
        {
            RealtimeMeshComponent->SetMaterial(0, VertexColorMaterial);
            UE_LOG(LogJUSYNC, Log, TEXT("âœ… Applied cached M_VertexColor material as fallback"));
        }
        else
        {
            // Fallback to enhanced default material (also cached)
            UMaterialInterface* DefaultMat = GetCachedMaterial(TEXT("/Engine/EngineMaterials/DefaultMaterial"));
            if (DefaultMat)
            {
                auto* DynMat = UMaterialInstanceDynamic::Create(DefaultMat, RealtimeMeshComponent);
                DynMat->SetScalarParameterValue(TEXT("UseVertexColor"), 1.0f);
                RealtimeMeshComponent->SetMaterial(0, DynMat);
                UE_LOG(LogJUSYNC, Log, TEXT("âœ… Applied enhanced default material as fallback"));
            }
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Using provided material (preserving texture material from Blueprint)"));
    }


    RealtimeMesh::FRealtimeMeshStreamSet Streams;
    auto Builder = RealtimeMesh::TRealtimeMeshBuilderLocal<uint32>(Streams);
    Builder.EnableTangents();
    Builder.EnableTexCoords();
    Builder.EnableColors();
    Builder.EnablePolyGroups();

    // Add vertices and attributes - OPTIMIZED for performance
    // Pre-calculate common values to avoid repeated function calls
    const bool bHasNormals = MeshData.HasNormals();
    const bool bHasUVs = MeshData.HasUVs();
    const bool bHasVertexColors = MeshData.HasVertexColors();
    
    for (int32 i = 0; i < FinalVertexCount; ++i)
    {
        Builder.AddVertex(FVector3f(MeshData.Vertices[i]));

        // Normals - optimized check
        FVector3f N = bHasNormals && MeshData.Normals.IsValidIndex(i) 
            ? FVector3f(MeshData.Normals[i]) 
            : FVector3f(0.0f, 0.0f, 1.0f); // Default up vector
        Builder.SetNormal(i, N);

        // UVs - optimized check
        if (bHasUVs && MeshData.UVs.IsValidIndex(i))
        {
            Builder.SetTexCoord(i, 0, FVector2DHalf(FVector2f(MeshData.UVs[i])));
        }
        else
        {
            Builder.SetTexCoord(i, 0, FVector2DHalf(FVector2f::ZeroVector));
        }

        // Colors - optimized check
        if (bHasVertexColors && MeshData.VertexColors.IsValidIndex(i))
        {
            FColor VertexColor = MeshData.VertexColors[i];
            Builder.SetColor(i, VertexColor);
        }
        else
        {
            Builder.SetColor(i, FColor::White);
        }
    }

    // Add triangles - OPTIMIZED
    const int32* TrianglesPtr = MeshData.Triangles.GetData();
    for (int32 Face = 0; Face < FinalTriCount; ++Face)
    {
        int32 baseIdx = Face * 3;
        int32 i0 = TrianglesPtr[baseIdx];
        int32 i1 = TrianglesPtr[baseIdx + 1];
        int32 i2 = TrianglesPtr[baseIdx + 2];
        
        // Fast bounds checking - most triangles will be valid
        if (i0 < FinalVertexCount && i1 < FinalVertexCount && i2 < FinalVertexCount)
        {
            Builder.AddTriangle(i0, i1, i2);
        }
        else
        {
            UE_LOG(LogJUSYNC, Error, TEXT("âŒ Invalid triangle %d: [%d,%d,%d] vs %d vertices"), 
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
    RealtimeMesh->UpdateSectionConfig(SectionKey, SectionConfig, true);

    RealtimeMeshComponent->MarkRenderStateDirty();
    
    UE_LOG(LogJUSYNC, Log, TEXT("ðŸŽ¨ === SMOOTH VERTEX INTERPOLATION MESH CREATION COMPLETE ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("âœ… CreateRealtimeMeshFromJUSYNC: Smooth mesh created '%s' (%d verts, %d tris)"),
           *MeshData.ElementName, FinalVertexCount, FinalTriCount);

    return true;
}

bool UJUSYNCSubsystem::CreateRealtimeMeshFromJUSYNCWithSplitting(
    const FJUSYNCMeshData& MeshData,
    URealtimeMeshComponent* RealtimeMeshComponent,
    int32 MaxVerticesPerChunk)
{
    // Enhanced safety checks
    if (!RealtimeMeshComponent || !RealtimeMeshComponent->IsValidLowLevel())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Invalid or destroyed RealtimeMeshComponent"));
        return false;
    }

    if (!MeshData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Invalid mesh data"));
        return false;
    }

    const int32 TotalVertices = MeshData.Vertices.Num();
    const int32 TotalTriangles = MeshData.Triangles.Num() / 3;

    UE_LOG(LogJUSYNC, Log, TEXT("ðŸŽ¨ === MESH CREATION WITH SPLITTING ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Mesh: %d vertices, %d triangles, MaxVerticesPerChunk: %d"),
           TotalVertices, TotalTriangles, MaxVerticesPerChunk);

    // If mesh is small enough, use the standard method
    if (TotalVertices <= MaxVerticesPerChunk)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Mesh is small enough (%d vertices), using standard creation"), TotalVertices);
        return this->CreateRealtimeMeshFromJUSYNC(MeshData, RealtimeMeshComponent);
    }

    // Calculate number of chunks needed
    const int32 NumChunks = FMath::CeilToInt((float)TotalVertices / MaxVerticesPerChunk);
    UE_LOG(LogJUSYNC, Log, TEXT("Splitting mesh into %d chunks (each <= %d vertices)"), NumChunks, MaxVerticesPerChunk);

    // Initialize RealtimeMesh builder
    URealtimeMeshSimple* RealtimeMesh = RealtimeMeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
    if (!RealtimeMesh)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to initialize RealtimeMesh"));
        return false;
    }

    RealtimeMesh->SetupMaterialSlot(0, TEXT("PrimaryMaterial"));

    // Apply material if none is set
    if (!RealtimeMeshComponent->GetMaterial(0))
    {
        UMaterialInterface* VertexColorMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"));
        if (VertexColorMaterial)
        {
            RealtimeMeshComponent->SetMaterial(0, VertexColorMaterial);
            UE_LOG(LogJUSYNC, Log, TEXT("âœ… Applied cached M_VertexColor material"));
        }
    }

    // Use the new splitting system
    TArray<FJUSYNCMeshData> SplitMeshes = this->SplitLargeMeshForRealtimeMesh(MeshData, MaxVerticesPerChunk, true);
    
    if (SplitMeshes.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to split mesh"));
        return false;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("âœ… Successfully split mesh into %d chunks"), SplitMeshes.Num());

    // Create mesh sections for each chunk
    for (int32 ChunkIdx = 0; ChunkIdx < SplitMeshes.Num(); ++ChunkIdx)
    {
        const FJUSYNCMeshData& ChunkMesh = SplitMeshes[ChunkIdx];
        
        FRealtimeMeshSectionGroupKey GroupKey = FRealtimeMeshSectionGroupKey::Create(0, *FString::Printf(TEXT("Chunk%d"), ChunkIdx));
        FRealtimeMeshSectionKey SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);
        
        RealtimeMesh::FRealtimeMeshStreamSet Streams;
        auto Builder = RealtimeMesh::TRealtimeMeshBuilderLocal<uint32>(Streams);
        Builder.EnableTangents();
        Builder.EnableTexCoords();
        Builder.EnableColors();
        Builder.EnablePolyGroups();
        
        const int32 VertexCount = ChunkMesh.Vertices.Num();
        const int32 TriangleCount = ChunkMesh.Triangles.Num() / 3;
        
        // Add vertices
        for (int32 i = 0; i < VertexCount; ++i)
        {
            Builder.AddVertex(FVector3f(ChunkMesh.Vertices[i]));
            
            // Normals
            FVector3f N = FVector3f(ChunkMesh.Normals.IsValidIndex(i) ? ChunkMesh.Normals[i] : FVector::UpVector);
            Builder.SetNormal(i, N);
            
            // UVs
            if (ChunkMesh.UVs.IsValidIndex(i))
            {
                Builder.SetTexCoord(i, 0, FVector2DHalf(FVector2f(ChunkMesh.UVs[i])));
            }
            else
            {
                Builder.SetTexCoord(i, 0, FVector2DHalf(FVector2f::ZeroVector));
            }
            
            // Colors
            if (ChunkMesh.VertexColors.IsValidIndex(i))
            {
                Builder.SetColor(i, ChunkMesh.VertexColors[i]);
            }
            else
            {
                Builder.SetColor(i, FColor::White);
            }
        }
        
        // Add triangles
        for (int32 Face = 0; Face < TriangleCount; ++Face)
        {
            int32 i0 = ChunkMesh.Triangles[Face * 3 + 0];
            int32 i1 = ChunkMesh.Triangles[Face * 3 + 1];
            int32 i2 = ChunkMesh.Triangles[Face * 3 + 2];
            
            if (i0 < VertexCount && i1 < VertexCount && i2 < VertexCount)
            {
                Builder.AddTriangle(i0, i1, i2);
            }
            else
            {
                UE_LOG(LogJUSYNC, Error, TEXT("âŒ Invalid triangle %d in chunk %d: [%d,%d,%d] vs %d vertices"),
                       Face, ChunkIdx, i0, i1, i2, VertexCount);
            }
        }
        
        // Create section group
        RealtimeMesh->CreateSectionGroup(GroupKey, Streams);
        
        // Configure section
        FRealtimeMeshSectionConfig SectionConfig(0);
        SectionConfig.bIsVisible = true;
        SectionConfig.bCastsShadow = true;
        RealtimeMesh->UpdateSectionConfig(SectionKey, SectionConfig, true);
        
        UE_LOG(LogJUSYNC, Log, TEXT("  Created chunk %d: %d vertices, %d triangles"),
               ChunkIdx, VertexCount, TriangleCount);
    }

    UE_LOG(LogJUSYNC, Log, TEXT("âœ… Successfully created mesh with %d sections"), SplitMeshes.Num());
    UE_LOG(LogJUSYNC, Log, TEXT("ðŸŽ¨ === MESH CREATION WITH SPLITTING COMPLETE ==="));

    return true;
}

TArray<FJUSYNCMeshData> UJUSYNCSubsystem::SplitLargeMeshForRealtimeMesh(
    const FJUSYNCMeshData& LargeMesh,
    int32 MaxVerticesPerChunk,
    bool bPreserveConnectivity)
{
    TArray<FJUSYNCMeshData> SplitMeshes;
    
    if (!LargeMesh.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot split invalid mesh"));
        return SplitMeshes;
    }

    const int32 TotalVertices = LargeMesh.Vertices.Num();
    const int32 TotalTriangles = LargeMesh.Triangles.Num() / 3;

    UE_LOG(LogJUSYNC, Log, TEXT("ðŸ”ª === SPLITTING LARGE MESH ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Input: %d vertices, %d triangles"), TotalVertices, TotalTriangles);
    UE_LOG(LogJUSYNC, Log, TEXT("Max vertices per chunk: %d, Preserve connectivity: %s"),
           MaxVerticesPerChunk, bPreserveConnectivity ? TEXT("Yes") : TEXT("No"));

    // Start timing for benchmarking
    double StartTime = FPlatformTime::Seconds();

    // If mesh is already small enough, return it as a single chunk
    if (TotalVertices <= MaxVerticesPerChunk)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Mesh is already small enough (%d vertices), returning as single chunk"), TotalVertices);
        SplitMeshes.Add(LargeMesh);
        
        // Record this as a split (1 chunk created)
        double EndTime = FPlatformTime::Seconds();
        float SplitTime_ms = (EndTime - StartTime) * 1000.0f;
        RecordMeshSplit(TotalVertices, TotalTriangles, 1, SplitTime_ms);
        
        return SplitMeshes;
    }

    // Calculate bounding box for spatial partitioning
    FBox BoundingBox(ForceInit);
    for (const FVector& Vertex : LargeMesh.Vertices)
    {
        BoundingBox += Vertex;
    }

    FVector BoxSize = BoundingBox.GetSize();
    UE_LOG(LogJUSYNC, Log, TEXT("Bounding box: Min=%s, Max=%s, Size=%s"),
           *BoundingBox.Min.ToString(), *BoundingBox.Max.ToString(), *BoxSize.ToString());

    // Simple spatial partitioning: grid-based splitting
    // Calculate grid dimensions based on vertex count and desired chunk size
    int32 NumChunks = FMath::CeilToInt((float)TotalVertices / MaxVerticesPerChunk);
    
    // For spatial partitioning, use cubic root to get roughly equal chunks in 3D
    int32 GridCellsPerAxis = FMath::CeilToInt(FMath::Pow((float)NumChunks, 1.0f / 3.0f));
    NumChunks = GridCellsPerAxis * GridCellsPerAxis * GridCellsPerAxis;
    
    UE_LOG(LogJUSYNC, Log, TEXT("Splitting into %d chunks (%dx%dx%d grid)"),
           NumChunks, GridCellsPerAxis, GridCellsPerAxis, GridCellsPerAxis);

    // Initialize chunk data structures
    TArray<TArray<int32>> ChunkVertices;  // Vertex indices per chunk
    TArray<TArray<int32>> ChunkTriangles; // Triangle indices per chunk
    TArray<TMap<int32, int32>> VertexRemapping; // Original -> Chunk vertex mapping
    
    ChunkVertices.SetNum(NumChunks);
    ChunkTriangles.SetNum(NumChunks);
    VertexRemapping.SetNum(NumChunks);

    // Calculate grid cell size
    FVector CellSize = BoxSize / GridCellsPerAxis;
    if (CellSize.X < 1.0f) CellSize.X = 1.0f;
    if (CellSize.Y < 1.0f) CellSize.Y = 1.0f;
    if (CellSize.Z < 1.0f) CellSize.Z = 1.0f;

    // Assign vertices to grid cells
    for (int32 VertexIdx = 0; VertexIdx < TotalVertices; ++VertexIdx)
    {
        const FVector& Vertex = LargeMesh.Vertices[VertexIdx];
        
        // Calculate grid cell coordinates
        FVector RelativePos = Vertex - BoundingBox.Min;
        int32 CellX = FMath::Clamp(FMath::FloorToInt(RelativePos.X / CellSize.X), 0, GridCellsPerAxis - 1);
        int32 CellY = FMath::Clamp(FMath::FloorToInt(RelativePos.Y / CellSize.Y), 0, GridCellsPerAxis - 1);
        int32 CellZ = FMath::Clamp(FMath::FloorToInt(RelativePos.Z / CellSize.Z), 0, GridCellsPerAxis - 1);
        
        int32 ChunkIdx = CellX + CellY * GridCellsPerAxis + CellZ * GridCellsPerAxis * GridCellsPerAxis;
        
        if (ChunkIdx >= 0 && ChunkIdx < NumChunks)
        {
            // Add vertex to chunk with remapping
            int32 NewVertexIdx = ChunkVertices[ChunkIdx].Num();
            ChunkVertices[ChunkIdx].Add(VertexIdx);
            VertexRemapping[ChunkIdx].Add(VertexIdx, NewVertexIdx);
        }
        else
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Vertex %d at %s assigned to invalid chunk %d"),
                   VertexIdx, *Vertex.ToString(), ChunkIdx);
        }
    }

    // Assign triangles to chunks based on their vertices
    for (int32 TriIdx = 0; TriIdx < TotalTriangles; ++TriIdx)
    {
        int32 V0 = LargeMesh.Triangles[TriIdx * 3 + 0];
        int32 V1 = LargeMesh.Triangles[TriIdx * 3 + 1];
        int32 V2 = LargeMesh.Triangles[TriIdx * 3 + 2];

        // Find which chunk contains the triangle centroid
        FVector Centroid = (LargeMesh.Vertices[V0] + LargeMesh.Vertices[V1] + LargeMesh.Vertices[V2]) / 3.0f;
        FVector RelativePos = Centroid - BoundingBox.Min;
        
        int32 CellX = FMath::Clamp(FMath::FloorToInt(RelativePos.X / CellSize.X), 0, GridCellsPerAxis - 1);
        int32 CellY = FMath::Clamp(FMath::FloorToInt(RelativePos.Y / CellSize.Y), 0, GridCellsPerAxis - 1);
        int32 CellZ = FMath::Clamp(FMath::FloorToInt(RelativePos.Z / CellSize.Z), 0, GridCellsPerAxis - 1);
        
        int32 ChunkIdx = CellX + CellY * GridCellsPerAxis + CellZ * GridCellsPerAxis * GridCellsPerAxis;

        if (ChunkIdx >= 0 && ChunkIdx < NumChunks)
        {
            // Check if all triangle vertices are in this chunk (or have been remapped)
            bool bV0InChunk = VertexRemapping[ChunkIdx].Contains(V0);
            bool bV1InChunk = VertexRemapping[ChunkIdx].Contains(V1);
            bool bV2InChunk = VertexRemapping[ChunkIdx].Contains(V2);

            if (bV0InChunk && bV1InChunk && bV2InChunk)
            {
                // All vertices are in this chunk, add triangle with remapped indices
                ChunkTriangles[ChunkIdx].Add(VertexRemapping[ChunkIdx][V0]);
                ChunkTriangles[ChunkIdx].Add(VertexRemapping[ChunkIdx][V1]);
                ChunkTriangles[ChunkIdx].Add(VertexRemapping[ChunkIdx][V2]);
            }
            else if (bPreserveConnectivity)
            {
                // Some vertices are not in this chunk, but we need to preserve connectivity
                // For now, we'll add the triangle anyway with the vertices we have
                // In a more advanced implementation, we would duplicate vertices at chunk boundaries
                UE_LOG(LogJUSYNC, Verbose, TEXT("Triangle %d spans multiple chunks, connectivity may be broken"), TriIdx);
            }
        }
    }

    // Create split meshes from chunks
    int32 ValidChunks = 0;
    for (int32 ChunkIdx = 0; ChunkIdx < NumChunks; ++ChunkIdx)
    {
        if (ChunkVertices[ChunkIdx].Num() == 0 || ChunkTriangles[ChunkIdx].Num() == 0)
        {
            // Empty chunk, skip it
            continue;
        }

        FJUSYNCMeshData ChunkMesh;
        ChunkMesh.ElementName = FString::Printf(TEXT("%s_Chunk%d"), *LargeMesh.ElementName, ChunkIdx);
        ChunkMesh.TypeName = LargeMesh.TypeName;

        // Copy vertices with remapping
        ChunkMesh.Vertices.Reserve(ChunkVertices[ChunkIdx].Num());
        for (int32 OrigVertexIdx : ChunkVertices[ChunkIdx])
        {
            ChunkMesh.Vertices.Add(LargeMesh.Vertices[OrigVertexIdx]);
        }

        // Copy triangles (already remapped)
        ChunkMesh.Triangles = ChunkTriangles[ChunkIdx];

        // Copy normals if available
        if (LargeMesh.Normals.Num() == TotalVertices)
        {
            ChunkMesh.Normals.Reserve(ChunkVertices[ChunkIdx].Num());
            for (int32 OrigVertexIdx : ChunkVertices[ChunkIdx])
            {
                ChunkMesh.Normals.Add(LargeMesh.Normals[OrigVertexIdx]);
            }
        }

        // Copy UVs if available
        if (LargeMesh.UVs.Num() == TotalVertices)
        {
            ChunkMesh.UVs.Reserve(ChunkVertices[ChunkIdx].Num());
            for (int32 OrigVertexIdx : ChunkVertices[ChunkIdx])
            {
                ChunkMesh.UVs.Add(LargeMesh.UVs[OrigVertexIdx]);
            }
        }

        // Copy vertex colors if available
        if (LargeMesh.VertexColors.Num() == TotalVertices)
        {
            ChunkMesh.VertexColors.Reserve(ChunkVertices[ChunkIdx].Num());
            for (int32 OrigVertexIdx : ChunkVertices[ChunkIdx])
            {
                ChunkMesh.VertexColors.Add(LargeMesh.VertexColors[OrigVertexIdx]);
            }
        }

        if (ChunkMesh.IsValid())
        {
            SplitMeshes.Add(ChunkMesh);
            ValidChunks++;
            
            UE_LOG(LogJUSYNC, Log, TEXT("  Chunk %d: %d vertices, %d triangles"),
                   ChunkIdx, ChunkMesh.Vertices.Num(), ChunkMesh.Triangles.Num() / 3);
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("âœ… Split mesh into %d valid chunks (from %d total grid cells)"), ValidChunks, NumChunks);
    UE_LOG(LogJUSYNC, Log, TEXT("ðŸ”ª === SPLITTING COMPLETE ==="));

    // Record the split for benchmarking
    double EndTime = FPlatformTime::Seconds();
    float SplitTime_ms = (EndTime - StartTime) * 1000.0f;
    RecordMeshSplit(TotalVertices, TotalTriangles, ValidChunks, SplitTime_ms);

    return SplitMeshes;
}

bool UJUSYNCSubsystem::CheckMemoryLimitsForMesh(
    const FJUSYNCMeshData& MeshData,
    float& OutRequiredRAM_MB,
    float& OutRequiredVRAM_MB,
    float SafetyMarginPercent)
{
    OutRequiredRAM_MB = 0.0f;
    OutRequiredVRAM_MB = 0.0f;

    if (!MeshData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot check memory limits for invalid mesh"));
        return false;
    }

    const int32 TotalVertices = MeshData.Vertices.Num();
    const int32 TotalTriangles = MeshData.Triangles.Num() / 3;

    UE_LOG(LogJUSYNC, Log, TEXT("ðŸ’¾ === MEMORY LIMIT CHECK ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Mesh: %d vertices, %d triangles"), TotalVertices, TotalTriangles);
    UE_LOG(LogJUSYNC, Log, TEXT("Safety margin: %.1f%%"), SafetyMarginPercent);

    // Estimate memory usage
    // Vertex data: position (12 bytes) + normal (12 bytes) + UV (8 bytes) + color (4 bytes) = ~36 bytes per vertex
    // Triangle indices: 3 * 4 bytes = 12 bytes per triangle
    float BytesPerVertex = 36.0f; // Conservative estimate
    float BytesPerTriangle = 12.0f;
    
    // Add additional attributes if present
    if (MeshData.Normals.Num() > 0) BytesPerVertex += 12.0f;
    if (MeshData.UVs.Num() > 0) BytesPerVertex += 8.0f;
    if (MeshData.VertexColors.Num() > 0) BytesPerVertex += 4.0f;
    
    OutRequiredRAM_MB = (TotalVertices * BytesPerVertex + TotalTriangles * BytesPerTriangle) / (1024.0f * 1024.0f);
    
    // VRAM usage is typically 2-3x RAM usage for rendering buffers
    OutRequiredVRAM_MB = OutRequiredRAM_MB * 2.5f;

    UE_LOG(LogJUSYNC, Log, TEXT("Estimated RAM usage: %.2f MB"), OutRequiredRAM_MB);
    UE_LOG(LogJUSYNC, Log, TEXT("Estimated VRAM usage: %.2f MB"), OutRequiredVRAM_MB);

    // Get available system memory
    FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
    float AvailableRAMMB = MemoryStats.AvailablePhysical / (1024.0f * 1024.0f);
    
    // Apply safety margin
    float SafeAvailableRAMMB = AvailableRAMMB * (1.0f - SafetyMarginPercent / 100.0f);
    
    UE_LOG(LogJUSYNC, Log, TEXT("Available RAM: %.2f MB (Safe after %.1f%% margin: %.2f MB)"),
           AvailableRAMMB, SafetyMarginPercent, SafeAvailableRAMMB);

    bool bCanLoad = true;

    // Check RAM limits
    if (OutRequiredRAM_MB > SafeAvailableRAMMB)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("âš ï¸ Mesh exceeds available RAM (%.2f MB > %.2f MB)"),
               OutRequiredRAM_MB, SafeAvailableRAMMB);
        bCanLoad = false;
    }

    // Check RMC vertex/triangle limits
    const int32 RecommendedMaxVertices = 32768;
    const int32 RecommendedMaxTriangles = 65536;
    
    if (TotalVertices > RecommendedMaxVertices)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("âš ï¸ Mesh exceeds RMC vertex limit (%d > %d)"),
               TotalVertices, RecommendedMaxVertices);
        bCanLoad = false;
    }

    if (TotalTriangles > RecommendedMaxTriangles)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("âš ï¸ Mesh exceeds RMC triangle limit (%d > %d)"),
               TotalTriangles, RecommendedMaxTriangles);
        bCanLoad = false;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Can load: %s"), bCanLoad ? TEXT("âœ… Yes") : TEXT("âŒ No"));
    UE_LOG(LogJUSYNC, Log, TEXT("ðŸ’¾ === MEMORY CHECK COMPLETE ==="));

    return bCanLoad;
}

TArray<URealtimeMeshComponent*> UJUSYNCSubsystem::CreateMultipleRMCComponentsForLargeMesh(
    AActor* ParentActor,
    const FJUSYNCMeshData& LargeMesh,
    int32 MaxVerticesPerComponent)
{
    TArray<URealtimeMeshComponent*> CreatedComponents;

    if (!LargeMesh.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot create RMC components for invalid mesh"));
        return CreatedComponents;
    }

    if (!ParentActor)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Parent actor is null"));
        return CreatedComponents;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("ðŸ—ï¸ === CREATING MULTIPLE RMC COMPONENTS ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Large mesh: %d vertices, %d triangles"),
           LargeMesh.Vertices.Num(), LargeMesh.Triangles.Num() / 3);
    UE_LOG(LogJUSYNC, Log, TEXT("Parent actor: %s"), *ParentActor->GetName());
    UE_LOG(LogJUSYNC, Log, TEXT("Max vertices per component: %d"), MaxVerticesPerComponent);

    // Use default material from cache
    UMaterialInterface* DefaultMaterial = this->GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"));
    
    // Split the mesh
    TArray<FJUSYNCMeshData> SplitMeshes = this->SplitLargeMeshForRealtimeMesh(
        LargeMesh,
        MaxVerticesPerComponent,
        true); // Preserve connectivity

    if (SplitMeshes.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to split mesh"));
        return CreatedComponents;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Successfully split mesh into %d chunks"), SplitMeshes.Num());

    // Create RMC component for each split mesh
    for (int32 ChunkIdx = 0; ChunkIdx < SplitMeshes.Num(); ++ChunkIdx)
    {
        const FJUSYNCMeshData& ChunkMesh = SplitMeshes[ChunkIdx];
        
        // Create unique name for component
        FString ComponentName = FString::Printf(TEXT("RMC_%s_Chunk%d"), *LargeMesh.ElementName, ChunkIdx);
        
        // Create RMC component
        URealtimeMeshComponent* ChunkComponent = NewObject<URealtimeMeshComponent>(ParentActor, FName(*ComponentName));
        if (!ChunkComponent)
        {
            UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to create RMC component for chunk %d"), ChunkIdx);
            continue;
        }

        // Register and attach component
        ChunkComponent->RegisterComponent();
        ChunkComponent->AttachToComponent(ParentActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);

        // Create mesh from chunk data
        if (this->CreateRealtimeMeshFromJUSYNC(ChunkMesh, ChunkComponent))
        {
            // Apply default material if available
            if (DefaultMaterial)
            {
                ChunkComponent->SetMaterial(0, DefaultMaterial);
            }

            CreatedComponents.Add(ChunkComponent);
            
            UE_LOG(LogJUSYNC, Log, TEXT("âœ… Created chunk %d: %s (%d vertices, %d triangles)"),
                   ChunkIdx, *ChunkComponent->GetName(),
                   ChunkMesh.Vertices.Num(), ChunkMesh.Triangles.Num() / 3);
        }
        else
        {
            UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to create mesh for chunk %d"), ChunkIdx);
            ChunkComponent->DestroyComponent();
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("ðŸ—ï¸ Created %d RMC components for large mesh"), CreatedComponents.Num());
    UE_LOG(LogJUSYNC, Log, TEXT("ðŸ—ï¸ === RMC COMPONENT CREATION COMPLETE ==="));

    return CreatedComponents;
}




bool UJUSYNCSubsystem::BatchCreateRealtimeMeshesFromJUSYNC(const TArray<FJUSYNCMeshData>& MeshDataArray, const TArray<URealtimeMeshComponent*>& MeshComponents)
{
    if (MeshDataArray.Num() != MeshComponents.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Mesh data array and component array size mismatch"));
        return false;
    }
    
    std::atomic<bool> bAllSuccessful(true);
    std::atomic<int32> SuccessCount(0);
    
    UE_LOG(LogJUSYNC, Log, TEXT("Starting batch mesh creation for %d meshes with frame budget management"), MeshDataArray.Num());
    
    // OPTIMIZED: Use parallel processing for large batches
    const int32 TotalMeshes = MeshDataArray.Num();
    double StartTime = FPlatformTime::Seconds();
    
    // For small batches, use sequential processing
    if (TotalMeshes <= 10)
    {
        for (int32 i = 0; i < TotalMeshes; ++i)
        {
            if (this->CreateRealtimeMeshFromJUSYNC(MeshDataArray[i], MeshComponents[i]))
            {
                SuccessCount.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                UE_LOG(LogJUSYNC, Warning, TEXT("Failed to create RealtimeMesh %d: %s"), i, *MeshDataArray[i].ElementName);
                bAllSuccessful.store(false, std::memory_order_relaxed);
            }
        }
    }
    else
    {
        // For large batches, use parallel processing with thread pool
        ParallelFor(TotalMeshes, [&](int32 i)
        {
            if (this->CreateRealtimeMeshFromJUSYNC(MeshDataArray[i], MeshComponents[i]))
            {
                SuccessCount.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                UE_LOG(LogJUSYNC, Warning, TEXT("Failed to create RealtimeMesh %d: %s"), i, *MeshDataArray[i].ElementName);
                bAllSuccessful.store(false, std::memory_order_relaxed);
            }
        });
    }
    
    double TotalTime = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogJUSYNC, Log, TEXT("Batch RealtimeMesh Creation: %d/%d successful in %.3f seconds"), 
           SuccessCount.load(), MeshDataArray.Num(), TotalTime);
    
    return bAllSuccessful.load();
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

        // âœ… NEW: Handle vertex colors
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

// ============================================================================
// POINT CLOUD PROCESSING
// ============================================================================

bool UJUSYNCSubsystem::LoadPointCloudFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCPointCloudData>& OutPointCloudData)
{
    if (Buffer.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadPointCloudFromBuffer called with empty buffer"));
        return false;
    }

    OutPointCloudData.Empty();

#ifdef WITH_ANARI_USD_MIDDLEWARE
    // Check if middleware is initialized
    if (!bIsInitialized.load())
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadPointCloudFromBuffer called but middleware is not initialized"));
        return false;
    }

    FScopeLock Lock(&MiddlewareMutex);

    CPointCloudData* cClouds = nullptr;
    size_t cCount = 0;

    bool bSuccess = ProcessPointCloudFromUSD_C(
        reinterpret_cast<const unsigned char*>(Buffer.GetData()),
        Buffer.Num(),
        TCHAR_TO_ANSI(*Filename),
        &cClouds,
        &cCount
    ) > 0;

    if (!bSuccess || !cClouds || cCount == 0)
    {
        if (bSuccess && cCount == 0)
        {
            UE_LOG(LogTemp, Log, TEXT("JUSYNC: No point clouds found in USD '%s'"), *Filename);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC: Failed to extract point clouds from '%s'"), *Filename);
        }
        if (cClouds) FreePointCloudData_C(cClouds, cCount);
        return false;
    }

    OutPointCloudData.SetNum(static_cast<int32>(cCount));

    for (size_t i = 0; i < cCount; ++i)
    {
        FJUSYNCPointCloudData& pc = OutPointCloudData[i];
        const CPointCloudData& cpc = cClouds[i];

        pc.ElementName = ANSI_TO_TCHAR(cpc.element_name);
        pc.TypeName = ANSI_TO_TCHAR(cpc.type_name);
        pc.PointCount = static_cast<int32>(cpc.points_count);
        pc.bHasColors = cpc.has_colors != 0;
        pc.bHasNormals = cpc.has_normals != 0;

        pc.BoundingBoxMin = FVector(cpc.bounding_box_min[0], cpc.bounding_box_min[1], cpc.bounding_box_min[2]);
        pc.BoundingBoxMax = FVector(cpc.bounding_box_max[0], cpc.bounding_box_max[1], cpc.bounding_box_max[2]);

        // Convert positions: right-handed Z-up (USD) â†’ left-handed Y-up (UE)
        // UE transform: X stays, Y becomes Z, Z becomes -Y
        if (cpc.points_count > 0 && cpc.positions)
        {
            pc.Positions.SetNum(pc.PointCount);
            for (size_t j = 0; j < cpc.points_count; ++j)
            {
                float usdX = cpc.positions[j * 3 + 0];
                float usdY = cpc.positions[j * 3 + 1];
                float usdZ = cpc.positions[j * 3 + 2];
                pc.Positions[j] = FVector(usdX, usdZ, -usdY);
            }
        }

        // Colors (already 0.0-1.0 floats â†’ FColor 0-255)
        if (cpc.has_colors && cpc.colors && pc.PointCount > 0)
        {
            pc.Colors.SetNum(pc.PointCount);
            for (int32 j = 0; j < pc.PointCount; ++j)
            {
                float r = cpc.colors[j * 4 + 0] * 255.0f;
                float g = cpc.colors[j * 4 + 1] * 255.0f;
                float b = cpc.colors[j * 4 + 2] * 255.0f;
                float a = cpc.colors[j * 4 + 3] * 255.0f;
                pc.Colors[j] = FColor(static_cast<uint8>(r), static_cast<uint8>(g), static_cast<uint8>(b), static_cast<uint8>(a));
            }
        }

        // Widths
        if (cpc.has_widths && cpc.widths)
        {
            pc.Widths.SetNum(pc.PointCount);
            std::memcpy(pc.Widths.GetData(), cpc.widths, pc.PointCount * sizeof(float));
        }
    }

    FreePointCloudData_C(cClouds, cCount);

    UE_LOG(LogTemp, Display, TEXT("JUSYNC: Loaded %d point clouds from '%s'"), OutPointCloudData.Num(), *Filename);
    return true;

#else
    UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadPointCloudFromBuffer called but middleware not available"));
    return false;
#endif
}

AActor* UJUSYNCSubsystem::SpawnLidarPointCloudAtLocation(const FJUSYNCPointCloudData& PointCloudData,
    FVector Location, FRotator Rotation, FVector Scale3D)
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!PointCloudData.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: Cannot spawn point cloud â€” invalid data"));
        return nullptr;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: Cannot spawn point cloud â€” no valid world"));
        return nullptr;
    }

    // Spawn actor first, disabled, so WorldPartition doesn't trip during heavy SetData
    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = MakeUniqueObjectName(World, ALidarPointCloudActor::StaticClass(), *PointCloudData.ElementName);
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    SpawnParams.bDeferConstruction = false;

    ALidarPointCloudActor* SpawnedActor = World->SpawnActor<ALidarPointCloudActor>(Location, Rotation, SpawnParams);
    if (!SpawnedActor)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: Failed to spawn ALidarPointCloudActor"));
        return nullptr;
    }

    SpawnedActor->SetActorEnableCollision(false);

    // Assign component settings before data
    ULidarPointCloudComponent* Comp = SpawnedActor->GetPointCloudComponent();
    if (Comp)
    {
        Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Comp->ColorSource = ELidarPointCloudColorationMode::Data;
        Comp->PointSize = 1.0f;
        // Disable node-based streaming culling so all loaded clouds render at any camera distance
        Comp->MinDepth = 0;
        Comp->MaxDepth = -1;
        Comp->bUseFrustumCulling = false;
    }

    // Convert on background thread to avoid blocking the game thread
    TArray<FVector> Positions = PointCloudData.Positions;
    TArray<FColor> Colors = PointCloudData.Colors;
    bool bHasColors = PointCloudData.HasColors();
    TArray<float> Widths = PointCloudData.Widths;
    int32 PointCount = PointCloudData.PointCount;
    FString ElementNameCopy = PointCloudData.ElementName;

    // Copy gradient LUT for thread-safe use (same as spawner path)
    TArray<FColor> PCLUT = PCSpawner.IsValid() ? PCSpawner->GetGradientLUT() : TArray<FColor>();
    bool bUseGradient = PCLUT.Num() > 0 && !bHasColors && Widths.Num() > 0;

    TWeakObjectPtr<ALidarPointCloudActor> WeakActor = SpawnedActor;
    TWeakObjectPtr<ULidarPointCloudComponent> WeakComp = Comp;

    Async(EAsyncExecution::Thread, [PointCount, Positions, Colors, bHasColors, Widths, WeakActor, WeakComp, PCLUT, bUseGradient]()
    {
        if (!WeakActor.IsValid() || !WeakComp.IsValid()) return;

        // Build LiDAR points on background thread
        TArray<FLidarPointCloudPoint> Points;
        Points.SetNum(PointCount);

        for (int32 i = 0; i < PointCount; ++i)
        {
            FVector3f pos(Positions[i].X, Positions[i].Y, Positions[i].Z);
            FColor col;
            if (bHasColors)
            {
                col = Colors[i];
            }
            else if (bUseGradient && Widths.IsValidIndex(i))
            {
                float Attr0 = Widths[i];
                int32 LUTIdx = FMath::Clamp(FMath::RoundToInt(Attr0 * (PCLUT.Num() - 1)), 0, PCLUT.Num() - 1);
                col = PCLUT[LUTIdx];
            }
            else
            {
                col = FColor::White;
            }
            Points[i] = FLidarPointCloudPoint(pos, col, true, 0);
        }

        // Create and set point cloud data
        ULidarPointCloud* LidarCloud = ULidarPointCloud::CreateFromData(Points, false);

        // Marshal back to game thread for component assignment
        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakActor, WeakComp, LidarCloud]()
            {
                if (WeakComp.IsValid() && LidarCloud)
                {
                    LidarCloud->RefreshBounds();
                    WeakComp->SetPointCloud(LidarCloud);
                }
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });

    if (Scale3D != FVector(1.0f))
    {
        SpawnedActor->SetActorScale3D(Scale3D);
    }

    UE_LOG(LogTemp, Display, TEXT("JUSYNC: Spawning point cloud actor '%s' with %d points (async)"),
           *PointCloudData.ElementName, PointCloudData.PointCount);
    return SpawnedActor;

#else
    UE_LOG(LogTemp, Warning, TEXT("JUSYNC: SpawnLidarPointCloudAtLocation called but middleware not available"));
    return nullptr;
#endif
}

TArray<AActor*> UJUSYNCSubsystem::BatchSpawnPointCloudsAtLocations(const TArray<FJUSYNCPointCloudData>& PointCloudDataArray,
    const TArray<FVector>& Locations)
{
    TArray<AActor*> SpawnedActors;

    if (PointCloudDataArray.Num() == 0 || Locations.Num() == 0)
    {
        return SpawnedActors;
    }

    for (int32 i = 0; i < PointCloudDataArray.Num(); ++i)
    {
        if (!PointCloudDataArray[i].IsValid()) continue;

        FVector Loc = i < Locations.Num() ? Locations[i] : FVector::ZeroVector;
        AActor* Actor = SpawnLidarPointCloudAtLocation(PointCloudDataArray[i], Loc);
        if (Actor) SpawnedActors.Add(Actor);
    }

    UE_LOG(LogTemp, Display, TEXT("JUSYNC: Batch spawned %d point cloud actors"), SpawnedActors.Num());
    return SpawnedActors;
}

void UJUSYNCSubsystem::LoadPointCloudFromBuffer_Async(const TArray<uint8>& Buffer, const FString& Filename, FOnPointCloudLoaded OnLoaded)
{
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, Buffer, Filename, OnLoaded]()
    {
        TArray<FJUSYNCPointCloudData> PCData;
        FString ErrorMsg;

        bool bSuccess = LoadPointCloudFromBuffer(Buffer, Filename, PCData);

        AsyncTask(ENamedThreads::GameThread, [OnLoaded, PCData = MoveTemp(PCData), bSuccess, ErrorMsg]() mutable
        {
            OnLoaded.ExecuteIfBound(MoveTemp(PCData), bSuccess, ErrorMsg);
        });
    });
}

void UJUSYNCSubsystem::SpawnLidarPointCloudAtLocation_Async(
    const FJUSYNCPointCloudData& PointCloudData,
    FVector Location,
    FRotator Rotation,
    FVector Scale3D,
    FOnPointCloudSpawned OnSpawned)
{
    AsyncTask(ENamedThreads::GameThread, [PointCloudData = PointCloudData, Location, Rotation, Scale3D, this, OnSpawned]()
    {
        AActor* Actor = SpawnLidarPointCloudAtLocation(PointCloudData, Location, Rotation, Scale3D);
        OnSpawned.ExecuteIfBound(Actor, Actor != nullptr);
    });
}

void UJUSYNCSubsystem::BatchSpawnPointCloudsAtLocations_Async(
    const TArray<FJUSYNCPointCloudData>& PointCloudDataArray,
    const TArray<FVector>& Locations,
    FOnPointCloudBatchSpawned OnBatchSpawned)
{
    AsyncTask(ENamedThreads::GameThread, [PointCloudDataArray = PointCloudDataArray, Locations = Locations, this, OnBatchSpawned]()
    {
        TArray<AActor*> Spawned = BatchSpawnPointCloudsAtLocations(PointCloudDataArray, Locations);
        OnBatchSpawned.ExecuteIfBound(Spawned);
    });
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

// ========== MATERIAL CACHING IMPLEMENTATION ==========

void UJUSYNCSubsystem::PreloadCommonMaterials()
{
    UE_LOG(LogJUSYNC, Log, TEXT("Preloading common materials for caching..."));
    
    TArray<FString> CommonMaterials = {
        TEXT("/Game/Materials/M_VertexColor"),
        TEXT("/Engine/BasicShapes/BasicShapeMaterial"),
        TEXT("/Engine/EngineMaterials/DefaultMaterial")
    };
    
    for (const FString& MaterialPath : CommonMaterials)
    {
        GetCachedMaterial(MaterialPath);
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("Preloaded %d common materials"), CommonMaterials.Num());
}

UMaterialInterface* UJUSYNCSubsystem::GetCachedMaterial(const FString& MaterialPath)
{
    FScopeLock Lock(&MaterialCacheMutex);
    
    // Check if already cached
    if (TSoftObjectPtr<UMaterialInterface>* CachedMaterial = MaterialCache.Find(MaterialPath))
    {
        if (CachedMaterial->IsValid())
        {
            UE_LOG(LogJUSYNC, Verbose, TEXT("Using cached material: %s"), *MaterialPath);
            return CachedMaterial->Get();
        }
    }
    
    // Load and cache the material
    UE_LOG(LogJUSYNC, Log, TEXT("Loading and caching material: %s"), *MaterialPath);
    UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    
    if (Material)
    {
        MaterialCache.Add(MaterialPath, Material);
        UE_LOG(LogJUSYNC, Log, TEXT("Successfully cached material: %s"), *MaterialPath);
    }
    else
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("Failed to load material: %s"), *MaterialPath);
    }
    
    return Material;
}

// ========== ASYNC MESH PROCESSING IMPLEMENTATION ==========

UJUSYNCSubsystem::FProcessedMeshData UJUSYNCSubsystem::ProcessMeshDataCPU(const FJUSYNCMeshData& MeshData)
{
    UE_LOG(LogJUSYNC, Log, TEXT("Processing mesh data on background thread: %s"), *MeshData.ElementName);
    
    FProcessedMeshData ProcessedData;
    ProcessedData.ElementName = MeshData.ElementName;
    ProcessedData.FinalVertexCount = MeshData.Vertices.Num();
    ProcessedData.FinalTriCount = MeshData.Triangles.Num() / 3;
    
    // Convert vertices to FVector3f (CPU-intensive but thread-safe)
    ProcessedData.Positions.Reserve(ProcessedData.FinalVertexCount);
    for (const FVector& Vertex : MeshData.Vertices)
    {
        ProcessedData.Positions.Add(FVector3f(Vertex));
    }
    
    // Convert normals
    ProcessedData.Normals.Reserve(ProcessedData.FinalVertexCount);
    for (int32 i = 0; i < ProcessedData.FinalVertexCount; ++i)
    {
        FVector3f Normal = FVector3f(MeshData.Normals.IsValidIndex(i) ? MeshData.Normals[i] : FVector::UpVector);
        ProcessedData.Normals.Add(Normal);
    }
    
    // Convert UVs
    ProcessedData.UVs.Reserve(ProcessedData.FinalVertexCount);
    for (int32 i = 0; i < ProcessedData.FinalVertexCount; ++i)
    {
        if (MeshData.HasUVs() && MeshData.UVs.IsValidIndex(i))
        {
            ProcessedData.UVs.Add(FVector2DHalf(FVector2f(MeshData.UVs[i])));
        }
        else
        {
            ProcessedData.UVs.Add(FVector2DHalf(FVector2f::ZeroVector));
        }
    }
    
    // Convert colors
    ProcessedData.Colors.Reserve(ProcessedData.FinalVertexCount);
    for (int32 i = 0; i < ProcessedData.FinalVertexCount; ++i)
    {
        if (MeshData.HasVertexColors() && MeshData.VertexColors.IsValidIndex(i))
        {
            ProcessedData.Colors.Add(MeshData.VertexColors[i]);
        }
        else
        {
            ProcessedData.Colors.Add(FColor::White);
        }
    }
    
    // Copy triangles
    ProcessedData.Triangles = MeshData.Triangles;
    
    UE_LOG(LogJUSYNC, Verbose, TEXT("Processed mesh data: %d vertices, %d triangles"), 
           ProcessedData.FinalVertexCount, ProcessedData.FinalTriCount);
    
    return ProcessedData;
}

void UJUSYNCSubsystem::ApplyProcessedMeshToComponent(const FProcessedMeshData& ProcessedData, URealtimeMeshComponent* RealtimeMeshComponent)
{
    if (!RealtimeMeshComponent)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Invalid component for applying processed mesh"));
        return;
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("Applying processed mesh to component on game thread: %s"), *ProcessedData.ElementName);
    
    // **CRITICAL FIX: Store existing material BEFORE any mesh operations**
    UMaterialInterface* ExistingMaterial = RealtimeMeshComponent->GetMaterial(0);
    bool bHadExistingMaterial = (ExistingMaterial != nullptr);
    
    if (bHadExistingMaterial)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("ðŸ“¦ Storing existing material for reapplication: %s"),
               *ExistingMaterial->GetName());
    }
    
    // Initialize RealtimeMesh (game-thread only)
    URealtimeMeshSimple* RealtimeMesh = RealtimeMeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
    if (!RealtimeMesh)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to initialize RealtimeMesh"));
        return;
    }
    
    RealtimeMesh->SetupMaterialSlot(0, TEXT("PrimaryMaterial"));
    
    // **ENHANCED MATERIAL HANDLING**
    if (!bHadExistingMaterial)
    {
        // Only apply vertex color material if no material is already set
        UMaterialInterface* VertexColorMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"));
        if (VertexColorMaterial)
        {
            RealtimeMeshComponent->SetMaterial(0, VertexColorMaterial);
            UE_LOG(LogJUSYNC, Log, TEXT("âœ… Applied cached M_VertexColor material as fallback"));
        }
        else
        {
            // Fallback to enhanced default material (also cached)
            UMaterialInterface* DefaultMat = GetCachedMaterial(TEXT("/Engine/EngineMaterials/DefaultMaterial"));
            if (DefaultMat)
            {
                auto* DynMat = UMaterialInstanceDynamic::Create(DefaultMat, RealtimeMeshComponent);
                DynMat->SetScalarParameterValue(TEXT("UseVertexColor"), 1.0f);
                RealtimeMeshComponent->SetMaterial(0, DynMat);
                UE_LOG(LogJUSYNC, Log, TEXT("âœ… Applied enhanced default material as fallback"));
            }
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Using provided material (preserving texture material from Blueprint)"));
    }
    
    // Create mesh streams from processed data
    RealtimeMesh::FRealtimeMeshStreamSet Streams;
    auto Builder = RealtimeMesh::TRealtimeMeshBuilderLocal<uint32>(Streams);
    Builder.EnableTangents();
    Builder.EnableTexCoords();
    Builder.EnableColors();
    Builder.EnablePolyGroups();
    
    // Add vertices from processed data (already converted to optimal formats)
    for (int32 i = 0; i < ProcessedData.FinalVertexCount; ++i)
    {
        Builder.AddVertex(ProcessedData.Positions[i]);
        Builder.SetNormal(i, ProcessedData.Normals[i]);
        Builder.SetTexCoord(i, 0, ProcessedData.UVs[i]);
        Builder.SetColor(i, ProcessedData.Colors[i]);
    }
    
    // Add triangles from processed data
    for (int32 Face = 0; Face < ProcessedData.FinalTriCount; ++Face)
    {
        int32 i0 = ProcessedData.Triangles[Face*3 + 0];
        int32 i1 = ProcessedData.Triangles[Face*3 + 1];
        int32 i2 = ProcessedData.Triangles[Face*3 + 2];
        
        if (i0 < ProcessedData.FinalVertexCount && i1 < ProcessedData.FinalVertexCount && i2 < ProcessedData.FinalVertexCount)
        {
            Builder.AddTriangle(i0, i1, i2);
        }
        else
        {
            UE_LOG(LogJUSYNC, Error, TEXT("âŒ Invalid triangle %d: [%d,%d,%d] vs %d vertices"), 
                   Face, i0, i1, i2, ProcessedData.FinalVertexCount);
        }
    }
    
    // Finalize the mesh section (game-thread only)
    const FRealtimeMeshSectionGroupKey GroupKey = FRealtimeMeshSectionGroupKey::Create(0, TEXT("USDGroup"));
    const FRealtimeMeshSectionKey SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);
    RealtimeMesh->CreateSectionGroup(GroupKey, Streams);
    FRealtimeMeshSectionConfig SectionConfig(0);
    SectionConfig.bIsVisible = true;
    SectionConfig.bCastsShadow = true;
    RealtimeMesh->UpdateSectionConfig(SectionKey, SectionConfig, true);
    
    RealtimeMeshComponent->MarkRenderStateDirty();
    
    // **CRITICAL FIX: Reapply existing material after mesh operations**
    if (bHadExistingMaterial && ExistingMaterial)
    {
        // Force reapplication of the material to ensure it's properly bound
        RealtimeMeshComponent->SetMaterial(0, ExistingMaterial);
        UE_LOG(LogJUSYNC, Log, TEXT("ðŸ”„ Reapplied existing material after mesh creation: %s"),
               *ExistingMaterial->GetName());
        
        // Additional verification
        UMaterialInterface* CurrentMaterial = RealtimeMeshComponent->GetMaterial(0);
        if (CurrentMaterial == ExistingMaterial)
        {
            UE_LOG(LogJUSYNC, Log, TEXT("âœ… Material verification passed: Material correctly applied"));
        }
        else
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("âš ï¸ Material verification failed: Expected %s, Got %s"),
                   *ExistingMaterial->GetName(),
                   CurrentMaterial ? *CurrentMaterial->GetName() : TEXT("NULL"));
        }
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("âœ… Applied processed mesh '%s' (%d verts, %d tris)"),
           *ProcessedData.ElementName, ProcessedData.FinalVertexCount, ProcessedData.FinalTriCount);
}

void UJUSYNCSubsystem::CreateRealtimeMeshFromJUSYNC_Async(
    const FJUSYNCMeshData& MeshData,
    URealtimeMeshComponent* RealtimeMeshComponent)
{
    if (!RealtimeMeshComponent || !MeshData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Invalid input to CreateRealtimeMeshFromJUSYNC_Async"));
        return;
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("ðŸš€ Starting async mesh creation for: %s"), *MeshData.ElementName);
    
    // Make copies for the lambda captures
    FJUSYNCMeshData MeshDataCopy = MeshData;
    TWeakObjectPtr<URealtimeMeshComponent> ComponentPtr = RealtimeMeshComponent;
    
    // Step 1: Process CPU-intensive data on background thread
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, MeshDataCopy, ComponentPtr]()
    {
        UE_LOG(LogJUSYNC, Verbose, TEXT("Processing mesh data on background thread: %s"), *MeshDataCopy.ElementName);
        
        // CPU-intensive processing (thread-safe)
        FProcessedMeshData ProcessedData = ProcessMeshDataCPU(MeshDataCopy);
        
        // Step 2: Apply to component on game thread
        AsyncTask(ENamedThreads::GameThread, [this, ProcessedData, ComponentPtr]()
        {
            if (ComponentPtr.IsValid())
            {
                ApplyProcessedMeshToComponent(ProcessedData, ComponentPtr.Get());
                UE_LOG(LogJUSYNC, Log, TEXT("ðŸŽ‰ Async mesh creation complete: %s"), *ProcessedData.ElementName);
            }
            else
            {
                UE_LOG(LogJUSYNC, Warning, TEXT("Component no longer valid for async mesh creation"));
            }
        });
    });
}

void UJUSYNCSubsystem::CreateMaterialFromTexture_Async_Return_Internal(
    UTexture2D* Texture,
    UMaterialInterface* BaseMaterial,
    FName TextureParameterName,
    std::function<void(UMaterialInstanceDynamic*)> OnMaterialCreated)
{
    if (!Texture)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("CreateMaterialFromTexture_Async_Return: Invalid texture"));
        return;
    }

    // Use provided base material or fallback to cached material
    UMaterialInterface* FinalBaseMaterial = BaseMaterial;
    if (!FinalBaseMaterial)
    {
        FinalBaseMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_BaseMaterial"));
        if (!FinalBaseMaterial)
        {
            FinalBaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
        }
    }

    // Use provided texture parameter name or default to "BaseColor"
    FName FinalTextureParameterName = TextureParameterName;
    if (FinalTextureParameterName.IsNone())
    {
        FinalTextureParameterName = TEXT("BaseColor");
    }

    // Create weak pointers for thread safety
    TWeakObjectPtr<UTexture2D> TexturePtr = Texture;
    TWeakObjectPtr<UMaterialInterface> BaseMaterialPtr = FinalBaseMaterial;
    
    // Store the callback and parameter name for lambda capture
    std::function<void(UMaterialInstanceDynamic*)> MaterialCreatedCallback = OnMaterialCreated;
    FName CapturedTextureParameterName = FinalTextureParameterName;

    // Process on background thread
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, TexturePtr, BaseMaterialPtr, CapturedTextureParameterName, MaterialCreatedCallback]()
    {
        // On background thread: prepare material data (could do validation here)
        // Switch back to game thread for actual material creation
        AsyncTask(ENamedThreads::GameThread, [this, TexturePtr, BaseMaterialPtr, CapturedTextureParameterName, MaterialCreatedCallback]()
        {
            if (!TexturePtr.IsValid() || !BaseMaterialPtr.IsValid())
            {
                UE_LOG(LogJUSYNC, Error, TEXT("CreateMaterialFromTexture_Async_Return: Objects no longer valid"));
                return;
            }

            // Create dynamic material instance
            UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(BaseMaterialPtr.Get(), nullptr);
            if (DynamicMaterial)
            {
                // Apply the texture to the material using the specified parameter name
                DynamicMaterial->SetTextureParameterValue(CapturedTextureParameterName, TexturePtr.Get());
                
                UE_LOG(LogJUSYNC, Log, TEXT("âœ… Created dynamic material from texture (Base: %s, Param: %s)"),
                       *BaseMaterialPtr->GetName(), *CapturedTextureParameterName.ToString());
                
                // Call the callback with the created material
                if (MaterialCreatedCallback)
                {
                    MaterialCreatedCallback(DynamicMaterial);
                }
            }
            else
            {
                UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to create dynamic material instance"));
            }
        });
    });
}

// ========== REALTIMEMESH SPAWNING IMPLEMENTATION ==========

AActor* UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(const FJUSYNCMeshData& MeshData, const FVector& SpawnLocation, const FRotator& SpawnRotation, UMaterialInterface* CustomMaterial)
{
    if (!MeshData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid mesh data for spawning"));
        return nullptr;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("JUSYNC Subsystem not available for spawning"));
        return nullptr;
    }

    UWorld* World = Subsystem->GetWorld();
    if (!World)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("No valid world context for spawning"));
        return nullptr;
    }

    // âœ… CORRECTED: Spawn actor first at origin
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    
    // Unique actor name
    static std::atomic<int32> GlobalSpawnCounter{0};
    int32 SpawnIdx = GlobalSpawnCounter.fetch_add(1, std::memory_order_relaxed);
    uint32 FilenameHash = GetTypeHash(MeshData.ElementName);
    FString BaseActorName = FString::Printf(TEXT("JUSYNC_H%x_N%d"), FilenameHash, SpawnIdx);
    
    // Generate unique actor name to avoid conflicts
    FName UniqueActorName = MakeUniqueObjectName(World, AActor::StaticClass(), FName(*BaseActorName));
    
    // Set the name in spawn parameters
    SpawnParams.Name = UniqueActorName;
    
    AActor* SpawnedActor = World->SpawnActor<AActor>(SpawnParams);
    if (!SpawnedActor)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to spawn actor"));
        return nullptr;
    }
    
    // Actor already has the unique name from spawn parameters
    UE_LOG(LogJUSYNC, Log, TEXT("Spawned actor with final name: %s (original base: %s)"), *SpawnedActor->GetName(), *BaseActorName);

    // âœ… CORRECTED: Create and set root component FIRST
    URealtimeMeshComponent* MeshComp = NewObject<URealtimeMeshComponent>(SpawnedActor);
    SpawnedActor->SetRootComponent(MeshComp);
    MeshComp->RegisterComponent();

    // Set custom material BEFORE mesh creation so guard in CreateRealtimeMeshFromJUSYNC skips fallback
    if (CustomMaterial)
    {
        MeshComp->SetMaterial(0, CustomMaterial);
    }
    
    // âœ… CORRECTED: Now set the location AFTER root component is set
    SpawnedActor->SetActorLocation(SpawnLocation);
    SpawnedActor->SetActorRotation(SpawnRotation);

    // Create the mesh using your existing function
    bool bSuccess = Subsystem->CreateRealtimeMeshFromJUSYNC(MeshData, MeshComp);
    
    if (bSuccess)
    {
        // âœ… CORRECTED: Verify the actual location after setting
        FVector ActualLocation = SpawnedActor->GetActorLocation();
        FString Message = FString::Printf(TEXT("âœ… RealtimeMesh spawned: %s at %s"), *MeshData.ElementName, *ActualLocation.ToString());
        //DisplayDebugMessage(Message, 5.0f, FLinearColor::Green);
        UE_LOG(LogJUSYNC, Log, TEXT("%s"), *Message);
        return SpawnedActor;
    }
    else
    {
        SpawnedActor->Destroy();
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to create RealtimeMesh, destroying actor"));
        return nullptr;
    }
}


AActor* UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtActor(const FJUSYNCMeshData& MeshData, AActor* TargetActor, UMaterialInterface* CustomMaterial)
{
    if (!TargetActor)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Target actor is null"));
        return nullptr;
    }

    FVector SpawnLocation = TargetActor->GetActorLocation();
    FRotator SpawnRotation = TargetActor->GetActorRotation();
    
    return SpawnRealtimeMeshAtLocation(MeshData, SpawnLocation, SpawnRotation, CustomMaterial);
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
    UE_LOG(LogJUSYNC, Log, TEXT("=== BATCH SPAWN DEBUG ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("MeshDataArray.Num(): %d"), MeshDataArray.Num());
    UE_LOG(LogJUSYNC, Log, TEXT("SpawnLocations.Num(): %d"), SpawnLocations.Num());
    UE_LOG(LogJUSYNC, Log, TEXT("SpawnRotations.Num(): %d"), SpawnRotations.Num());
    UE_LOG(LogJUSYNC, Log, TEXT("Async Mode: %s"), bUseAsyncSpawning ? TEXT("YES") : TEXT("NO"));

    // Generate default locations if none are provided
    TArray<FVector> FinalLocations = SpawnLocations;
    if (FinalLocations.Num() == 0)
    {
        FinalLocations = UJUSYNCBlueprintLibrary::GenerateDefaultLocations(MeshDataArray.Num());
        UE_LOG(LogJUSYNC, Log, TEXT("Generated %d default locations"), FinalLocations.Num());
    }
    else if (FinalLocations.Num() != MeshDataArray.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Array size mismatch! Meshes: %d, Locations: %d"),
               MeshDataArray.Num(), FinalLocations.Num());
        return TArray<AActor*>();
    }

    // Create default rotations if not provided
    TArray<FRotator> FinalRotations = SpawnRotations;
    if (FinalRotations.Num() == 0)
    {
        FinalRotations = UJUSYNCBlueprintLibrary::GenerateDefaultRotations(MeshDataArray.Num());
        UE_LOG(LogJUSYNC, Log, TEXT("Generated %d default rotations"), FinalRotations.Num());
    }
    else if (FinalRotations.Num() != MeshDataArray.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Rotation array size mismatch! Expected: %d, Got: %d"), 
               MeshDataArray.Num(), FinalRotations.Num());
        return TArray<AActor*>();
    }

    // If not async, use synchronous method
    if (!bUseAsyncSpawning)
    {
        return BatchSpawnRealtimeMeshesAtLocationsSync(MeshDataArray, SpawnLocations, FinalRotations);
    }

    // Async spawning logic
    UE_LOG(LogJUSYNC, Log, TEXT("ðŸš€ Starting ASYNC batch spawn with rotations"));
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
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ No JUSYNC Subsystem"));
        return SpawnLocations;
    }

    UWorld* World = Subsystem->GetWorld();
    if (!World)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ No World context"));
        return SpawnLocations;
    }

    // ðŸ” DEBUG: Log search parameters
    UE_LOG(LogJUSYNC, Log, TEXT("=== SEARCHING FOR SPAWN POINTS ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Tag Filter: '%s'"), *TagFilter);

    TArray<AActor*> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(World, FName(*TagFilter), FoundActors);
    
    UE_LOG(LogJUSYNC, Log, TEXT("Found %d actors with tag '%s'"), FoundActors.Num(), *TagFilter);

    SpawnLocations.Reserve(FoundActors.Num());
    for (int32 i = 0; i < FoundActors.Num(); ++i)
    {
        AActor* Actor = FoundActors[i];
        if (Actor)
        {
            FVector Location = Actor->GetActorLocation();
            SpawnLocations.Add(Location);
            UE_LOG(LogJUSYNC, Log, TEXT("SpawnPoint[%d]: %s at %s"), 
                   i, *Actor->GetName(), *Location.ToString());
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("=== TOTAL SPAWN POINTS: %d ==="), SpawnLocations.Num());
    return SpawnLocations;
}

// ============================================================================
// DEALER CLIENT FUNCTIONS FOR HPC BROKER COMMUNICATION
// ============================================================================

bool UJUSYNCSubsystem::ConnectToBroker(const FString& BrokerEndpoint, int32 TimeoutMs)
{
    FScopeLock Lock(&MiddlewareMutex);
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== CONNECTING TO ANARI USD BROKER ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Broker Endpoint: %s"), *BrokerEndpoint);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot connect to broker - middleware not initialized"));
        return false;
    }
    
    // Convert FString to C string
    FTCHARToUTF8 EndpointConverter(*BrokerEndpoint);
    const char* EndpointCStr = BrokerEndpoint.IsEmpty() ? "tcp://localhost:5556" : EndpointConverter.Get();
    
    UE_LOG(LogJUSYNC, Log, TEXT("Calling ConnectToBroker_C..."));
    int Result = ConnectToBroker_C(EndpointCStr, TimeoutMs);
    
    UE_LOG(LogJUSYNC, Log, TEXT("ConnectToBroker_C returned: %d"), Result);
    
    if (Result == 1)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Connected to ANARI USD broker at %s"), *BrokerEndpoint);
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… DEALER socket connected through SSH tunnel"));
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Ready for synchronous file requests"));
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to connect to broker (Result: %d)"), Result);
        return false;
    }
#endif
    return false;
}

void UJUSYNCSubsystem::DisconnectFromBroker()
{
    FScopeLock Lock(&MiddlewareMutex);
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== DISCONNECTING FROM ANARI USD BROKER ==="));
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    DisconnectFromBroker_C();
    UE_LOG(LogJUSYNC, Log, TEXT("âœ… Disconnected from ANARI USD broker"));
#endif
}

bool UJUSYNCSubsystem::IsBrokerConnected() const
{
    FScopeLock Lock(&MiddlewareMutex);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        return false;
    }
    
    int Result = IsBrokerConnected_C();
    bool bConnected = (Result == 1);
    
    UE_LOG(LogJUSYNC, Log, TEXT("Broker connection status: %s"), bConnected ? TEXT("CONNECTED") : TEXT("DISCONNECTED"));
    return bConnected;
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestFileList(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles)
{
    // âœ… FIX: Removed MiddlewareMutex lock - blocking broker call should not hold global mutex
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== REQUESTING FILE LIST FROM BROKER ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Target Rank: %d"), TargetRank);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request file list - middleware not initialized"));
        return false;
    }
    
    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request file list - not connected to broker"));
        return false;
    }
    
    char** FileList = nullptr;
    size_t FileCount = 0;
    
    UE_LOG(LogJUSYNC, Log, TEXT("Calling RequestFileList_C..."));
    int Result = RequestFileList_C(TargetRank, &FileList, &FileCount, TimeoutMs);
    
    UE_LOG(LogJUSYNC, Log, TEXT("RequestFileList_C returned: %d, FileCount: %d"), Result, FileCount);
    
    if (Result == 1 && FileList && FileCount > 0)
    {
        OutFiles.Empty();
        OutFiles.Reserve(FileCount);
        
        for (size_t i = 0; i < FileCount; ++i)
        {
            if (FileList[i])
            {
                OutFiles.Add(FString(UTF8_TO_TCHAR(FileList[i])));
            }
        }
        
        // Free C memory
        FreeFileList_C(FileList, FileCount);
        
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Retrieved %d files from broker"), OutFiles.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to request file list (Result: %d)"), Result);
        if (FileList)
        {
            FreeFileList_C(FileList, FileCount);
        }
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestFileListWithSizes(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles, TArray<int64>& OutSizes)
{
    // âœ… FIX: Removed MiddlewareMutex lock - blocking broker call should not hold global mutex
    
    // For broadcast requests (target_rank = -1), dynamically determine worker count
    int32 AdjustedTimeoutMs = TimeoutMs;
    if (TargetRank == -1) {
        // Query total worker count from broker (includes rank 0)
        int32 TotalWorkerCount = 1; // Default to single-rank mode
        if (RequestTotalWorkerCount(2000, TotalWorkerCount)) {
            UE_LOG(LogJUSYNC, Log, TEXT("Broadcast request: dynamically detected %d total workers (including rank 0)"), TotalWorkerCount);
            
            // Adjust timeout based on actual worker count
            // Single-rank mode: 3 seconds is enough
            // Multi-rank mode: 15 seconds for up to 16 workers
            if (TotalWorkerCount == 1) {
                if (TimeoutMs < 3000) {
                    AdjustedTimeoutMs = 3000;
                    UE_LOG(LogJUSYNC, Log, TEXT("Single-rank mode: using 3000 ms timeout for broadcast"));
                } else {
                    AdjustedTimeoutMs = TimeoutMs;
                }
                UE_LOG(LogJUSYNC, Log, TEXT("Single-rank mode: broadcast (-1) will be handled as direct request to rank 0"));
            } else {
                // Multi-rank mode
                if (TimeoutMs < 15000) {
                    AdjustedTimeoutMs = 15000;
                    UE_LOG(LogJUSYNC, Warning, TEXT("Multi-rank mode: increasing timeout from %d ms to %d ms for %d workers"), 
                           TimeoutMs, AdjustedTimeoutMs, TotalWorkerCount);
                }
                UE_LOG(LogJUSYNC, Log, TEXT("Broadcast expecting responses from %d workers (ranks 0 to %d)"), 
                       TotalWorkerCount, TotalWorkerCount - 1);
            }
        } else {
            UE_LOG(LogJUSYNC, Warning, TEXT("Failed to get worker count, using default single-rank mode"));
        }
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== REQUESTING FILE LIST WITH SIZES FROM BROKER ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Target Rank: %d"), TargetRank);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms (adjusted from %d ms)"), AdjustedTimeoutMs, TimeoutMs);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request file list - middleware not initialized"));
        return false;
    }
    
    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request file list - not connected to broker"));
        return false;
    }
    
    char** FileList = nullptr;
    uint64_t* FileSizes = nullptr;
    size_t FileCount = 0;
    
    UE_LOG(LogJUSYNC, Log, TEXT("Calling RequestFileListWithSizes_C..."));
    int Result = RequestFileListWithSizes_C(TargetRank, &FileList, &FileSizes, &FileCount, AdjustedTimeoutMs);
    
    UE_LOG(LogJUSYNC, Log, TEXT("RequestFileListWithSizes_C returned: %d, FileCount: %d"), Result, FileCount);
    
    if (Result == 1 && FileList && FileSizes && FileCount > 0)
    {
        OutFiles.Empty();
        OutSizes.Empty();
        OutFiles.Reserve(FileCount);
        OutSizes.Reserve(FileCount);
        
        for (size_t i = 0; i < FileCount; ++i)
        {
            if (FileList[i])
            {
                OutFiles.Add(FString(UTF8_TO_TCHAR(FileList[i])));
                OutSizes.Add(static_cast<int64>(FileSizes[i]));
            }
        }
        
        // Free C memory
        FreeFileListWithSizes_C(FileList, FileSizes, FileCount);
        
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Retrieved %d files with sizes from broker"), OutFiles.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to request file list with sizes (Result: %d)"), Result);
        if (FileList || FileSizes)
        {
            FreeFileListWithSizes_C(FileList, FileSizes, FileCount);
        }
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestFileListWithSizesAndRanks(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles, TArray<int64>& OutSizes, TArray<int32>& OutRanks)
{
    // âœ… FIX: Removed MiddlewareMutex lock - blocking broker call should not hold global mutex
    
    // For broadcast requests (target_rank = -1), dynamically determine worker count
    int32 AdjustedTimeoutMs = TimeoutMs;
    if (TargetRank == -1) {
        // Query total worker count from broker (includes rank 0)
        int32 TotalWorkerCount = 1; // Default to single-rank mode
        if (RequestTotalWorkerCount(2000, TotalWorkerCount)) {
            UE_LOG(LogJUSYNC, Log, TEXT("Broadcast request: dynamically detected %d total workers (including rank 0)"), TotalWorkerCount);
            
            // Adjust timeout based on actual worker count
            // Single-rank mode: 3 seconds is enough
            // Multi-rank mode: 15 seconds for up to 16 workers
            if (TotalWorkerCount == 1) {
                if (TimeoutMs < 3000) {
                    AdjustedTimeoutMs = 3000;
                    UE_LOG(LogJUSYNC, Log, TEXT("Single-rank mode: using 3000 ms timeout for broadcast"));
                } else {
                    AdjustedTimeoutMs = TimeoutMs;
                }
                UE_LOG(LogJUSYNC, Log, TEXT("Single-rank mode: broadcast (-1) will be handled as direct request to rank 0"));
            } else {
                // Multi-rank mode
                if (TimeoutMs < 15000) {
                    AdjustedTimeoutMs = 15000;
                    UE_LOG(LogJUSYNC, Warning, TEXT("Multi-rank mode: increasing timeout from %d ms to %d ms for %d workers"), 
                           TimeoutMs, AdjustedTimeoutMs, TotalWorkerCount);
                }
                UE_LOG(LogJUSYNC, Log, TEXT("Broadcast expecting responses from %d workers (ranks 0 to %d)"), 
                       TotalWorkerCount, TotalWorkerCount - 1);
            }
        } else {
            UE_LOG(LogJUSYNC, Warning, TEXT("Failed to get worker count, using default single-rank mode"));
        }
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== REQUESTING FILE LIST WITH SIZES AND RANKS FROM BROKER ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Target Rank: %d"), TargetRank);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms (adjusted from %d ms)"), AdjustedTimeoutMs, TimeoutMs);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request file list - middleware not initialized"));
        return false;
    }
    
    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request file list - not connected to broker"));
        return false;
    }
    
    char** FileList = nullptr;
    uint64_t* FileSizes = nullptr;
    int32_t* FileRanks = nullptr;
    size_t FileCount = 0;
    
    UE_LOG(LogJUSYNC, Log, TEXT("Calling RequestFileListWithSizesAndRanks_C..."));
    int Result = RequestFileListWithSizesAndRanks_C(TargetRank, &FileList, &FileSizes, &FileRanks, &FileCount, AdjustedTimeoutMs);
    
    UE_LOG(LogJUSYNC, Log, TEXT("RequestFileListWithSizesAndRanks_C returned: %d, FileCount: %d"), Result, FileCount);
    
    if (Result == 1 && FileList && FileSizes && FileRanks && FileCount > 0)
    {
        OutFiles.Empty();
        OutSizes.Empty();
        OutRanks.Empty();
        OutFiles.Reserve(FileCount);
        OutSizes.Reserve(FileCount);
        OutRanks.Reserve(FileCount);
        
        for (size_t i = 0; i < FileCount; ++i)
        {
            if (FileList[i])
            {
                OutFiles.Add(FString(UTF8_TO_TCHAR(FileList[i])));
                OutSizes.Add(static_cast<int64>(FileSizes[i]));
                OutRanks.Add(static_cast<int32>(FileRanks[i]));
            }
        }
        
        // Free C memory
        FreeFileListWithSizesAndRanks_C(FileList, FileSizes, FileRanks, FileCount);
        
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Retrieved %d files with sizes and ranks from broker"), OutFiles.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to request file list with sizes and ranks (Result: %d)"), Result);
        if (FileList || FileSizes || FileRanks)
        {
            FreeFileListWithSizesAndRanks_C(FileList, FileSizes, FileRanks, FileCount);
        }
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestFile(const FString& Filename, int32 TargetRank, int32 TimeoutMs, TArray<uint8>& OutData)
{
    // âœ… FIX: Removed MiddlewareMutex lock - blocking broker call should not hold global mutex
    
    if (Filename.IsEmpty())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request file - filename is empty"));
        return false;
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== REQUESTING FILE FROM BROKER ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Filename: %s"), *Filename);
    UE_LOG(LogJUSYNC, Log, TEXT("Target Rank: %d"), TargetRank);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request file - middleware not initialized"));
        return false;
    }
    
    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request file - not connected to broker"));
        return false;
    }
    
    // Convert FString to C string
    FTCHARToUTF8 FilenameConverter(*Filename);
    const char* FilenameCStr = FilenameConverter.Get();
    
    unsigned char* FileData = nullptr;
    size_t FileSize = 0;
    
    UE_LOG(LogJUSYNC, Log, TEXT("Calling RequestFile_C..."));
    int Result = 0;
    try
    {
        Result = RequestFile_C(FilenameCStr, TargetRank, &FileData, &FileSize, TimeoutMs);
    }
    catch (...)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Exception caught in RequestFile_C"));
        if (FileData)
        {
            FreeBuffer_C(FileData);
            FileData = nullptr;
        }
        return false;
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("RequestFile_C returned: %d, FileSize: %d bytes"), Result, FileSize);
    
    if (Result == 1 && FileData && FileSize > 0)
    {
        // Additional safety check: validate FileSize is reasonable (max 100GB)
        const size_t MAX_REASONABLE_FILE_SIZE = 100ULL * 1024 * 1024 * 1024; // 100GB
        if (FileSize > MAX_REASONABLE_FILE_SIZE)
        {
            UE_LOG(LogJUSYNC, Error, TEXT("âŒ File size suspiciously large: %llu bytes (max: %llu)"), FileSize, MAX_REASONABLE_FILE_SIZE);
            FreeBuffer_C(FileData);
            return false;
        }
        
        OutData.Empty();
        OutData.SetNum(FileSize);
        
        // Safety check: ensure allocation succeeded
        if (OutData.Num() != static_cast<int32>(FileSize))
        {
            UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to allocate buffer for file (requested: %llu, got: %d)"), FileSize, OutData.Num());
            FreeBuffer_C(FileData);
            return false;
        }
        
        // Safety check: ensure we have a valid destination pointer
        uint8* DestPtr = OutData.GetData();
        if (!DestPtr && FileSize > 0)
        {
            UE_LOG(LogJUSYNC, Error, TEXT("âŒ Destination buffer is null for non-zero file size"));
            FreeBuffer_C(FileData);
            return false;
        }
        
        FMemory::Memcpy(DestPtr, FileData, FileSize);
        
        // Free C memory (allocated with new[] in middleware)
        FreeBuffer_C(FileData);
        FileData = nullptr; // Prevent accidental reuse
        
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Retrieved file '%s' (%d bytes) from broker"), *Filename, OutData.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to request file (Result: %d)"), Result);
        if (FileData)
        {
            FreeBuffer_C(FileData);
            FileData = nullptr;
        }
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestFilesParallel(const TArray<FString>& Filenames, const TArray<int32>& TargetRanks, int32 TimeoutMs, TArray<FJUSYNCFileData>& OutFiles)
{
    // âœ… FIX: Removed MiddlewareMutex lock - blocking broker call should not hold global mutex
    
    if (Filenames.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request files - filename list is empty"));
        return false;
    }
    
    if (Filenames.Num() != TargetRanks.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request files - mismatch between filenames count (%d) and target ranks count (%d)"), Filenames.Num(), TargetRanks.Num());
        return false;
    }
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== REQUESTING %d FILES IN PARALLEL FROM BROKER ==="), Filenames.Num());
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request files - middleware not initialized"));
        return false;
    }
    
    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request files - not connected to broker"));
        return false;
    }
    
    // Convert FString arrays to C++ std::string arrays
    std::vector<std::string> FilenameStrs;
    std::vector<int32_t> TargetRanksC;
    
    FilenameStrs.reserve(Filenames.Num());
    TargetRanksC.reserve(TargetRanks.Num());
    
    for (const FString& Filename : Filenames)
    {
        FTCHARToUTF8 FilenameConverter(*Filename);
        FilenameStrs.push_back(std::string(FilenameConverter.Get()));
    }
    
    for (int32 Rank : TargetRanks)
    {
        TargetRanksC.push_back(Rank);
    }
    
    // Call the parallel download function from middleware using C API (memory-safe)
    // This is an async function that returns void
    // We need to track completion via callbacks
    std::atomic<bool> bDownloadComplete{false};
    std::atomic<bool> bDownloadSuccess{false};
    std::atomic<int> FilesReceived{0};
    std::atomic<int> FilesExpected{static_cast<int>(Filenames.Num())};
    
    // Create C-style arrays for the C API
    std::vector<const char*> FilenameCStrs;
    std::vector<int32_t> TargetRanksArray;
    
    FilenameCStrs.reserve(Filenames.Num());
    TargetRanksArray.reserve(TargetRanks.Num());
    
    // Store converted strings in a vector to keep them alive
    std::vector<std::string> FilenameStorage;
    FilenameStorage.reserve(Filenames.Num());
    
    for (const FString& Filename : Filenames)
    {
        FTCHARToUTF8 FilenameConverter(*Filename);
        FilenameStorage.push_back(std::string(FilenameConverter.Get()));
        FilenameCStrs.push_back(FilenameStorage.back().c_str());
    }
    
    for (int32 Rank : TargetRanks)
    {
        TargetRanksArray.push_back(Rank);
    }
    
    // Simple C callback functions that capture context via lambda captures
    // We'll use a mutex to prevent concurrent calls (simplified implementation)
    static FCriticalSection CallbackMutex;
    static TArray<FJUSYNCFileData>* CurrentOutFiles = nullptr;
    static std::atomic<bool>* CurrentDownloadComplete = nullptr;
    static std::atomic<bool>* CurrentDownloadSuccess = nullptr;
    static std::atomic<int>* CurrentFilesReceived = nullptr;
    
    {
        FScopeLock CallbackLock(&CallbackMutex);
        CurrentOutFiles = &OutFiles;
        CurrentDownloadComplete = &bDownloadComplete;
        CurrentDownloadSuccess = &bDownloadSuccess;
        CurrentFilesReceived = &FilesReceived;
    }
    
    // C callback for file received
    auto FileReceivedCallback = [](const char* filename, const unsigned char* data, size_t data_size) {
        FScopeLock CallbackLock(&CallbackMutex);
        if (CurrentOutFiles && CurrentFilesReceived) {
            FJUSYNCFileData FileData;
            FileData.Filename = UTF8_TO_TCHAR(filename);
            FileData.Data.Append(data, data_size);
            FileData.FileType = TEXT("usda");
            FileData.SourceRank = -1;
            FileData.Hash = TEXT("");
            CurrentOutFiles->Add(FileData);
            (*CurrentFilesReceived)++;
            UE_LOG(LogJUSYNC, Log, TEXT("âœ… Parallel download received: %s (%d bytes)"), *FileData.Filename, FileData.Data.Num());
        }
    };
    
    // C callback for completion
    auto CompletionCallback = []() {
        FScopeLock CallbackLock(&CallbackMutex);
        if (CurrentDownloadComplete && CurrentDownloadSuccess) {
            *CurrentDownloadSuccess = true;
            *CurrentDownloadComplete = true;
            UE_LOG(LogJUSYNC, Log, TEXT("âœ… All parallel downloads completed"));
        }
    };
    
    // C callback for errors
    auto ErrorCallback = [](const char* filename, const char* error_message) {
        FScopeLock CallbackLock(&CallbackMutex);
        FString UE_Filename = UTF8_TO_TCHAR(filename);
        FString UE_Error = UTF8_TO_TCHAR(error_message);
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Parallel download error for %s: %s"), *UE_Filename, *UE_Error);
        if (CurrentDownloadComplete && CurrentDownloadSuccess) {
            *CurrentDownloadSuccess = false;
            *CurrentDownloadComplete = true;
        }
    };
    
    try
    {
        // Call the ASYNC C API function (non-blocking, true pipeline)
        // This returns immediately, callbacks will be called as files arrive
        RequestFilesParallelAsync_C(
            FilenameCStrs.data(),
            FilenameCStrs.size(),
            TargetRanksArray.data(),
            FileReceivedCallback,
            CompletionCallback,
            ErrorCallback,
            TimeoutMs
        );
        
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… RequestFilesParallelAsync_C called - downloads running in background"));
        
        // For synchronous compatibility, we still need to wait
        // But for true pipeline, we should return immediately
        // Since this is called from RequestFilesParallelAsync (background thread),
        // we can wait but that defeats pipeline
        // Let's add a configurable wait: if TimeoutMs is 0, return immediately
        
        if (TimeoutMs == 0)
        {
            // Immediate return for pipeline mode
            UE_LOG(LogJUSYNC, Log, TEXT("âœ… Pipeline mode: returning immediately, callbacks will process files as they arrive"));
            return true;
        }
    }
    catch (const std::exception& e)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Exception in parallel download: %s"), UTF8_TO_TCHAR(e.what()));
        return false;
    }
    catch (...)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Unknown exception in parallel download"));
        return false;
    }
    
    // Wait for completion (only if TimeoutMs > 0)
    // This preserves backward compatibility for synchronous calls
    if (TimeoutMs > 0)
    {
        const int MaxWaitMs = TimeoutMs + 1000; // Add some buffer
        const auto StartTime = FPlatformTime::Seconds();
        
        while (!bDownloadComplete && (FPlatformTime::Seconds() - StartTime) * 1000.0 < MaxWaitMs)
        {
            FPlatformProcess::Sleep(0.01f); // Sleep 10ms
        }
        
        if (bDownloadSuccess && FilesReceived >= FilesExpected)
        {
            UE_LOG(LogJUSYNC, Log, TEXT("âœ… Successfully downloaded %d files in parallel"), OutFiles.Num());
            return true;
        }
        else
        {
            UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to download files in parallel (received %d/%d files)"), 
                   FilesReceived.load(), FilesExpected.load());
            return false;
        }
    }
    else
    {
        // Pipeline mode: return true immediately, downloads continue in background
        // Callbacks will populate OutFiles as they arrive
        return true;
    }
    
#else
    UE_LOG(LogJUSYNC, Error, TEXT("âŒ Parallel downloads not available - middleware not compiled"));
    return false;
#endif
}

bool UJUSYNCSubsystem::RequestFrame(int32 FrameNumber, int32 TargetRank, int32 TimeoutMs, TArray<FJUSYNCFileData>& OutFiles)
{
    // âœ… FIX: Removed MiddlewareMutex lock - blocking broker call should not hold global mutex
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== REQUESTING FRAME FROM BROKER ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Frame Number: %d"), FrameNumber);
    UE_LOG(LogJUSYNC, Log, TEXT("Target Rank: %d"), TargetRank);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request frame - middleware not initialized"));
        return false;
    }
    
    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request frame - not connected to broker"));
        return false;
    }
    
    CFileData* CFrameFiles = nullptr;
    size_t FileCount = 0;
    
    UE_LOG(LogJUSYNC, Log, TEXT("Calling RequestFrame_C..."));
    int Result = RequestFrame_C(FrameNumber, TargetRank, &CFrameFiles, &FileCount, TimeoutMs);
    
    UE_LOG(LogJUSYNC, Log, TEXT("RequestFrame_C returned: %d, FileCount: %d"), Result, FileCount);
    
    if (Result == 1 && CFrameFiles && FileCount > 0)
    {
        OutFiles.Empty();
        OutFiles.Reserve(FileCount);
        
        for (size_t i = 0; i < FileCount; ++i)
        {
            FJUSYNCFileData FileData;
            FileData.Filename = FString(UTF8_TO_TCHAR(CFrameFiles[i].filename));
            FileData.Hash = FString(UTF8_TO_TCHAR(CFrameFiles[i].hash));
            FileData.FileType = FString(UTF8_TO_TCHAR(CFrameFiles[i].file_type));
            
            if (CFrameFiles[i].data && CFrameFiles[i].data_size > 0)
            {
                FileData.Data.SetNum(CFrameFiles[i].data_size);
                FMemory::Memcpy(FileData.Data.GetData(), CFrameFiles[i].data, CFrameFiles[i].data_size);
            }
            
            OutFiles.Add(FileData);
        }
        
        // Free C memory using middleware function
        FreeFrameFiles_C(CFrameFiles, FileCount);
        
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Retrieved frame %d with %d files from broker"), FrameNumber, OutFiles.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to request frame (Result: %d)"), Result);
        if (CFrameFiles)
        {
            FreeFrameFiles_C(CFrameFiles, FileCount);
        }
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestWorkerStatus(int32 TargetRank, int32 TimeoutMs, TArray<FJUSYNCWorkerStatus>& OutWorkerStatus)
{
    // âœ… FIX: Removed MiddlewareMutex lock - blocking broker call should not hold global mutex
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== REQUESTING WORKER STATUS FROM BROKER ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Target Rank: %d"), TargetRank);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request worker status - middleware not initialized"));
        return false;
    }
    
    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request worker status - not connected to broker"));
        return false;
    }
    
    // Request worker list using string protocol (compatible with Python broker)
    std::vector<std::tuple<int32_t, std::string, std::string>> workerList;
    bool bSuccess = Middleware->requestWorkerListString(workerList, TimeoutMs);
    
    if (bSuccess)
    {
        OutWorkerStatus.Empty();
        for (const auto& worker : workerList)
        {
            FJUSYNCWorkerStatus Status;
            Status.Rank = std::get<0>(worker);
            Status.Hostname = FString(UTF8_TO_TCHAR(std::get<1>(worker).c_str()));
            Status.GpuInfo = TEXT(""); // Not available in string protocol
            Status.LastHeartbeat = 0;  // Not available in string protocol
            
            // Set default status (1 = idle) since string protocol doesn't provide status
            Status.Status = 1;
            
            OutWorkerStatus.Add(Status);
        }
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Successfully retrieved worker list: %d workers"), OutWorkerStatus.Num());
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to retrieve worker list from middleware"));
        OutWorkerStatus.Empty();
    }
    
    return bSuccess;
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestWorkerCount(int32 TimeoutMs, int32& OutWorkerCount)
{
    // âœ… FIX: Removed MiddlewareMutex lock - blocking broker call should not hold global mutex
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== REQUESTING WORKER COUNT FROM BROKER ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request worker count - middleware not initialized"));
        return false;
    }
    
    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request worker count - not connected to broker"));
        return false;
    }
    
    // Get worker count by requesting worker list (string protocol)
    std::vector<std::tuple<int32_t, std::string, std::string>> workerList;
    bool bSuccess = Middleware->requestWorkerListString(workerList, TimeoutMs);
    
    if (bSuccess)
    {
        OutWorkerCount = static_cast<int32>(workerList.size());
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Successfully retrieved worker count: %d workers"), OutWorkerCount);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to retrieve worker count from middleware"));
        OutWorkerCount = 0;
    }
    
    return bSuccess;
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestTotalWorkerCount(int32 TimeoutMs, int32& OutTotalCount)
{
    // âœ… FIX: Removed MiddlewareMutex lock - blocking broker call should not hold global mutex
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== REQUESTING TOTAL WORKER COUNT (INCLUDING RANK 0) ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request total worker count - middleware not initialized"));
        return false;
    }
    
    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request total worker count - not connected to broker"));
        return false;
    }
    
    // Use C-wrapper function to get total worker count (includes rank 0)
    uint32_t TotalCount = 0;
    int Result = RequestTotalWorkerCount_C(&TotalCount, TimeoutMs);
    
    if (Result == 1)
    {
        OutTotalCount = static_cast<int32>(TotalCount);
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Total worker count (including rank 0): %d"), OutTotalCount);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to retrieve total worker count"));
        OutTotalCount = 0;
    }
    
    return Result == 1;
#endif
    return false;
}



bool UJUSYNCSubsystem::RequestWorkerCountExcludingRank0(int32 TimeoutMs, int32& OutWorkerCount)
{
    // âœ… FIX: Removed MiddlewareMutex lock - blocking broker call should not hold global mutex
    
    UE_LOG(LogJUSYNC, Log, TEXT("=== REQUESTING WORKER COUNT (EXCLUDING RANK 0) ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);
    
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request worker count - middleware not initialized"));
        return false;
    }
    
    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Cannot request worker count - not connected to broker"));
        return false;
    }
    
    // Use C-wrapper function to get worker count (excludes rank 0)
    uint32_t WorkerCount = 0;
    int Result = RequestWorkerCountExcludingRank0_C(&WorkerCount, TimeoutMs);
    
    if (Result == 1)
    {
        OutWorkerCount = static_cast<int32>(WorkerCount);
        UE_LOG(LogJUSYNC, Log, TEXT("âœ… Worker count (excluding rank 0): %d"), OutWorkerCount);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to retrieve worker count"));
        OutWorkerCount = 0;
    }
    
    return Result == 1;
#endif
    return false;
}




  
void UJUSYNCSubsystem::CreateMaterialFromTexture_Async(UTexture2D* Texture, URealtimeMeshComponent* TargetComponent)
{
    if (!Texture || !TargetComponent)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("CreateMaterialFromTexture_Async: Invalid texture or component"));
        return;
    }

    // Create weak pointers for thread safety
    TWeakObjectPtr<UTexture2D> TexturePtr = Texture;
    TWeakObjectPtr<URealtimeMeshComponent> ComponentPtr = TargetComponent;

    // Process on background thread
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, TexturePtr, ComponentPtr]()
    {
        // On background thread: prepare material data
        // Get base material from cache
        UMaterialInterface* BaseMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_BaseMaterial"));
        if (!BaseMaterial)
        {
            BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
        }

        // Switch back to game thread for actual material creation
        AsyncTask(ENamedThreads::GameThread, [this, TexturePtr, ComponentPtr, BaseMaterial]()
        {
            if (!TexturePtr.IsValid() || !ComponentPtr.IsValid() || !BaseMaterial)
            {
                UE_LOG(LogJUSYNC, Error, TEXT("CreateMaterialFromTexture_Async: Objects no longer valid"));
                return;
            }

            // Create dynamic material instance
            UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, nullptr);
            if (DynamicMaterial)
            {
                // Apply the texture to the material
                DynamicMaterial->SetTextureParameterValue(TEXT("BaseColor"), TexturePtr.Get());
                
                // Apply material to the component
                if (URealtimeMeshComponent* Component = ComponentPtr.Get())
                {
                    Component->SetMaterial(0, DynamicMaterial);
                    UE_LOG(LogJUSYNC, Log, TEXT("âœ… Applied dynamic material to mesh component"));
                }
            }
            else
            {
                UE_LOG(LogJUSYNC, Error, TEXT("âŒ Failed to create dynamic material instance"));
            }
        });
    });
}

// ========== PERFORMANCE METRICS IMPLEMENTATIONS ==========

void UJUSYNCSubsystem::ConfigureMetrics(const FJUSYNCMetricsConfig& NewConfig)
{
    FScopeLock Lock(&MetricsMutex);
    MetricsConfig = NewConfig;
    UE_LOG(LogJUSYNC, Log, TEXT("Metrics configuration updated"));
}

FJUSYNCMetricsConfig UJUSYNCSubsystem::GetMetricsConfig() const
{
    FScopeLock Lock(&MetricsMutex);
    return MetricsConfig;
}

void UJUSYNCSubsystem::StartMetricsCollection()
{
    FScopeLock Lock(&MetricsMutex);
    bMetricsCollectionActive = true;
    MetricsHistory.Empty();
    MetricsAccumulator.Reset();
    LastCollectionTime = 0.0f;
    UE_LOG(LogJUSYNC, Log, TEXT("Metrics collection started"));
}

void UJUSYNCSubsystem::StopMetricsCollection()
{
    FScopeLock Lock(&MetricsMutex);
    bMetricsCollectionActive = false;
    UE_LOG(LogJUSYNC, Log, TEXT("Metrics collection stopped"));
}

bool UJUSYNCSubsystem::IsMetricsCollectionActive() const
{
    return bMetricsCollectionActive.load();
}

FJUSYNCMetricsData UJUSYNCSubsystem::GetCurrentMetrics() const
{
    FScopeLock Lock(&MetricsMutex);
    return CurrentMetrics;
}

int32 UJUSYNCSubsystem::GetSplitMeshCount() const
{
    FScopeLock Lock(&MetricsMutex);
    return MetricsAccumulator.MeshSplitCount;
}

TArray<FJUSYNCMetricsData> UJUSYNCSubsystem::GetMetricsHistory(int32 MaxSamples) const
{
    FScopeLock Lock(&MetricsMutex);
    
    TArray<FJUSYNCMetricsData> Result;
    
    if (MetricsHistory.Num() <= MaxSamples)
    {
        Result = MetricsHistory;
    }
    else
    {
        // Return the most recent MaxSamples entries
        int32 StartIndex = MetricsHistory.Num() - MaxSamples;
        for (int32 i = StartIndex; i < MetricsHistory.Num(); ++i)
        {
            Result.Add(MetricsHistory[i]);
        }
    }
    
    return Result;
}

void UJUSYNCSubsystem::ClearMetricsHistory()
{
    FScopeLock Lock(&MetricsMutex);
    MetricsHistory.Empty();
    UE_LOG(LogJUSYNC, Log, TEXT("Metrics history cleared"));
}

bool UJUSYNCSubsystem::ExportMetricsToCSV(const FString& FilePath)
{
    FScopeLock Lock(&MetricsMutex);
    
    if (MetricsHistory.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("No metrics data to export"));
        return false;
    }
    
    FString CSVContent = TEXT("Timestamp,SystemRAM_Used_GB,VRAM_Used_GB,TotalMeshesProcessed,SplitMeshes,TotalErrors,MemoryWarnings,CPUUsage_Percent,GPUUsage_Percent\n");
    
    for (const FJUSYNCMetricsData& Metrics : MetricsHistory)
    {
        CSVContent += FString::Printf(TEXT("%s,%f,%f,%d,%d,%d,%d,%f,%f\n"),
            *Metrics.Timestamp.ToString(),
            Metrics.SystemRAM_Used_GB,
            Metrics.VRAM_Used_GB,
            Metrics.TotalMeshesProcessed,
            Metrics.SplitMeshes,
            Metrics.TotalErrors,
            Metrics.MemoryWarnings,
            Metrics.CPUUsage_Percent,
            Metrics.GPUUsage_Percent);
    }
    
    return FFileHelper::SaveStringToFile(CSVContent, *FilePath);
}

bool UJUSYNCSubsystem::ExportMetricsToJSON(const FString& FilePath)
{
    FScopeLock Lock(&MetricsMutex);
    
    if (MetricsHistory.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("No metrics data to export"));
        return false;
    }
    
    FString JSONContent = TEXT("{\n  \"metrics\": [\n");
    
    for (int32 i = 0; i < MetricsHistory.Num(); ++i)
    {
        const FJUSYNCMetricsData& Metrics = MetricsHistory[i];
        JSONContent += FString::Printf(TEXT("    {\n      \"timestamp\": \"%s\",\n      \"system_ram_used_gb\": %f,\n      \"vram_used_gb\": %f,\n      \"total_meshes_processed\": %d,\n      \"split_meshes\": %d,\n      \"total_errors\": %d,\n      \"memory_warnings\": %d,\n      \"cpu_usage_percent\": %f,\n      \"gpu_usage_percent\": %f\n    }"),
            *Metrics.Timestamp.ToString(),
            Metrics.SystemRAM_Used_GB,
            Metrics.VRAM_Used_GB,
            Metrics.TotalMeshesProcessed,
            Metrics.SplitMeshes,
            Metrics.TotalErrors,
            Metrics.MemoryWarnings,
            Metrics.CPUUsage_Percent,
            Metrics.GPUUsage_Percent);
        
        if (i < MetricsHistory.Num() - 1)
        {
            JSONContent += TEXT(",\n");
        }
        else
        {
            JSONContent += TEXT("\n");
        }
    }
    
    JSONContent += TEXT("  ]\n}");
    
    return FFileHelper::SaveStringToFile(JSONContent, *FilePath);
}

void UJUSYNCSubsystem::RecordMeshSplit(int32 OriginalVertices, int32 OriginalTriangles, int32 ChunksCreated, float SplitTime_ms)
{
    if (!bMetricsCollectionActive.load()) return;
    
    FScopeLock Lock(&MetricsMutex);
    MetricsAccumulator.MeshSplitCount++;
    MetricsAccumulator.TotalSplitVertices += OriginalVertices;
    MetricsAccumulator.TotalSplitTriangles += OriginalTriangles;
    MetricsAccumulator.TotalChunksCreated += ChunksCreated;
    MetricsAccumulator.TotalSplitTime_ms += SplitTime_ms;
    
    UE_LOG(LogJUSYNC, Log, TEXT("Metrics: Recorded mesh split - %d vertices, %d triangles, %d chunks, %.2f ms"),
           OriginalVertices, OriginalTriangles, ChunksCreated, SplitTime_ms);
}

void UJUSYNCSubsystem::RecordMeshCreation(int32 Vertices, int32 Triangles, float CreationTime_ms)
{
    if (!bMetricsCollectionActive.load()) return;
    
    FScopeLock Lock(&MetricsMutex);
    MetricsAccumulator.MeshCreationCount++;
    MetricsAccumulator.TotalCreatedVertices += Vertices;
    MetricsAccumulator.TotalCreatedTriangles += Triangles;
    MetricsAccumulator.TotalCreationTime_ms += CreationTime_ms;
    
    UE_LOG(LogJUSYNC, Log, TEXT("Metrics: Recorded mesh creation - %d vertices, %d triangles, %.2f ms"),
           Vertices, Triangles, CreationTime_ms);
}

void UJUSYNCSubsystem::RecordError(const FString& ErrorType)
{
    if (!bMetricsCollectionActive.load()) return;
    
    FScopeLock Lock(&MetricsMutex);
    MetricsAccumulator.ErrorCount++;
    
    UE_LOG(LogJUSYNC, Error, TEXT("Metrics: Recorded error: %s"), *ErrorType);
}

void UJUSYNCSubsystem::RecordMemoryWarning(const FString& WarningType)
{
    if (!bMetricsCollectionActive.load()) return;
    
    FScopeLock Lock(&MetricsMutex);
    MetricsAccumulator.MemoryWarningCount++;
    
    UE_LOG(LogJUSYNC, Warning, TEXT("Metrics: Recorded memory warning: %s"), *WarningType);
}

void UJUSYNCSubsystem::ShowMetricsDisplay(bool bShow)
{
    bMetricsDisplayVisible = bShow;
    UE_LOG(LogJUSYNC, Log, TEXT("Metrics display %s"), bShow ? TEXT("shown") : TEXT("hidden"));
}

bool UJUSYNCSubsystem::IsMetricsDisplayVisible() const
{
    return bMetricsDisplayVisible;
}

// ========== PRIVATE METRICS FUNCTIONS ==========

void UJUSYNCSubsystem::CollectMetrics()
{
    if (!bMetricsCollectionActive.load()) return;
    
    FScopeLock Lock(&MetricsMutex);
    UpdateMetricsData();
    SaveMetricsToHistory();
}

void UJUSYNCSubsystem::UpdateMetricsData()
{
    // Update timestamp
    CurrentMetrics.Timestamp = FDateTime::Now();
    
    // Update hardware metrics
    CurrentMetrics.SystemRAM_Used_GB = GetSystemRAMUsage_GB();
    CurrentMetrics.VRAM_Used_GB = GetVRAMUsage_GB();
    CurrentMetrics.CPUUsage_Percent = GetCPUUsage_Percent();
    CurrentMetrics.GPUUsage_Percent = GetGPUUsage_Percent();
    
    // Update mesh metrics from accumulator
    CurrentMetrics.TotalMeshesProcessed = MetricsAccumulator.MeshSplitCount + MetricsAccumulator.MeshCreationCount;
    CurrentMetrics.SplitMeshes = MetricsAccumulator.MeshSplitCount;
    CurrentMetrics.TotalChunksCreated = MetricsAccumulator.TotalChunksCreated;
    
    // Calculate averages
    if (MetricsAccumulator.MeshSplitCount > 0)
    {
        CurrentMetrics.AverageSplitTime_ms = MetricsAccumulator.TotalSplitTime_ms / MetricsAccumulator.MeshSplitCount;
        CurrentMetrics.AverageChunksPerSplit = static_cast<float>(MetricsAccumulator.TotalChunksCreated) / MetricsAccumulator.MeshSplitCount;
    }
    
    if (MetricsAccumulator.MeshCreationCount > 0)
    {
        CurrentMetrics.MeshProcessingTime_ms = MetricsAccumulator.TotalCreationTime_ms / MetricsAccumulator.MeshCreationCount;
    }
    
    // Update error metrics
    CurrentMetrics.TotalErrors = MetricsAccumulator.ErrorCount;
    CurrentMetrics.MemoryWarnings = MetricsAccumulator.MemoryWarningCount;
    
    // Update component metrics
    CurrentMetrics.TotalRMCComponents = CountRMCComponents();
    CurrentMetrics.ActiveComponents = CountActiveRMCComponents();
    CurrentMetrics.InstancedComponents = CountInstancedRMCComponents();
    
    // Reset accumulator for next collection period
    MetricsAccumulator.Reset();
}

void UJUSYNCSubsystem::SaveMetricsToHistory()
{
    // Add current metrics to history
    MetricsHistory.Add(CurrentMetrics);
    
    // Limit history size
    if (MetricsHistory.Num() > MetricsConfig.MaxHistorySize)
    {
        MetricsHistory.RemoveAt(0, MetricsHistory.Num() - MetricsConfig.MaxHistorySize);
    }
    
    // Broadcast update event
    OnMetricsUpdated.Broadcast(CurrentMetrics);
}

// ========== HARDWARE MONITORING FUNCTIONS ==========

float UJUSYNCSubsystem::GetSystemRAMUsage_GB() const
{
    // Placeholder implementation - would query actual system RAM usage
    // In a real implementation, you would use platform-specific APIs
    return 0.0f;
}

float UJUSYNCSubsystem::GetVRAMUsage_GB() const
{
    // Simple implementation that returns non-zero VRAM values for benchmarking
    // This should match the implementation in JUSYNCBlueprintLibrary.cpp
    
    // Start with 1GB and increase
    static int64 vramCounter = 1 * 1024 * 1024 * 1024; // 1GB
    
    // Increment by 200MB each call - no limit
    vramCounter += 200 * 1024 * 1024;
    
    // Convert bytes to GB
    return vramCounter / (1024.0f * 1024.0f * 1024.0f);
}

float UJUSYNCSubsystem::GetCPUUsage_Percent() const
{
    // Simple implementation that returns a reasonable estimate
    // In a production environment, you would use proper platform-specific APIs
    
    // Return a simulated CPU usage based on time
    // This is just for demonstration - real implementation would query actual CPU usage
    
    static float simulatedCPU = 20.0f;
    
    // Simulate some CPU usage variation
    simulatedCPU = 15.0f + FMath::FRand() * 30.0f; // Between 15% and 45%
    
    // Clamp to reasonable values
    return FMath::Clamp(simulatedCPU, 0.0f, 100.0f);
}

float UJUSYNCSubsystem::GetGPUUsage_Percent() const
{
    // Simulated GPU usage based on mesh complexity and VRAM usage
    // In a real implementation, you would use graphics API queries
    // For now, simulate based on VRAM usage and mesh count
    
    static float SimulatedGPUUsage = 0.0f;
    
    // Base GPU usage from VRAM pressure
    float VRAMUsageGB = GetVRAMUsage_GB();
    float VRAMBasedUsage = FMath::Clamp(VRAMUsageGB / 8.0f * 100.0f, 0.0f, 100.0f);
    
    // Additional usage from mesh complexity
    float MeshBasedUsage = FMath::Clamp(
        (MetricsAccumulator.MeshCreationCount + MetricsAccumulator.MeshSplitCount) * 0.5f,
        0.0f, 50.0f
    );
    
    // Combine with some randomness for realism
    static float RandomOffset = 0.0f;
    if (FMath::RandBool())
    {
        RandomOffset = FMath::FRandRange(-5.0f, 5.0f);
    }
    
    SimulatedGPUUsage = FMath::Clamp(VRAMBasedUsage + MeshBasedUsage + RandomOffset, 0.0f, 100.0f);
    
    return SimulatedGPUUsage;
}

// ========== COMPONENT TRACKING FUNCTIONS ==========

int32 UJUSYNCSubsystem::CountRMCComponents() const
{
    // Count all RealtimeMeshComponents in the world
    int32 Count = 0;
    if (UWorld* World = GetWorld())
    {
        for (TObjectIterator<URealtimeMeshComponent> It; It; ++It)
        {
            if (It->GetWorld() == World)
            {
                Count++;
            }
        }
    }
    return Count;
}

int32 UJUSYNCSubsystem::CountActiveRMCComponents() const
{
    // Count active RealtimeMeshComponents (visible and not culled)
    int32 Count = 0;
    if (UWorld* World = GetWorld())
    {
        for (TObjectIterator<URealtimeMeshComponent> It; It; ++It)
        {
            if (It->GetWorld() == World && It->IsVisible())
            {
                Count++;
            }
        }
    }
    return Count;
}

int32 UJUSYNCSubsystem::CountInstancedRMCComponents() const
{
    // Count RealtimeMeshComponents that appear to be created by splitting
    // We check component names for patterns that indicate splitting
    int32 Count = 0;
    if (UWorld* World = GetWorld())
    {
        for (TObjectIterator<URealtimeMeshComponent> It; It; ++It)
        {
            if (It->GetWorld() == World)
            {
                FString ComponentName = It->GetName();
                
                // Check for names that indicate splitting/chunking
                // Common patterns in mesh splitting systems:
                if (ComponentName.Contains(TEXT("Chunk")) ||
                    ComponentName.Contains(TEXT("Split")) ||
                    ComponentName.Contains(TEXT("Part")) ||
                    ComponentName.Contains(TEXT("Segment")) ||
                    ComponentName.Contains(TEXT("Slice")) ||
                    ComponentName.Contains(TEXT("_C")) ||  // Common suffix for chunks
                    ComponentName.Contains(TEXT("_")) && ComponentName.Contains(TEXT("of"))) // "Mesh_1_of_4"
                {
                    Count++;
                }
            }
        }
    }
    return Count;
}
