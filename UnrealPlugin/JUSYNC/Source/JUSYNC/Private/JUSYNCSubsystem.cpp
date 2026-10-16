#include "JUSYNCSubsystem.h"
#include "JUSYNCBlueprintLibrary.h"
#include <cfloat>
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture.h"
#include "TextureResource.h"
#include "RenderUtils.h"
#include "RealtimeMeshComponent.h"
#include "JUSYNCModule.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "RealtimeMeshSimple.h"
#include "JUSYNCStreamBuilder.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Async/ParallelFor.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Engine/EngineBaseTypes.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "JUSYNCPointCloudSpawner.h"
#include "JUSYNCMeshMorpher.h"
#ifdef WITH_ANARI_USD_MIDDLEWARE
#include "LidarPointCloud.h"
#include "LidarPointCloudComponent.h"
#include "LidarPointCloudActor.h"
#endif
#include <atomic>

#ifdef WITH_ANARI_USD_MIDDLEWARE
extern "C" {
#include "AnariUsdMiddleware_C.h"
#include "AnariUsdProtocol.h"
}
#include <cstdint>
#endif

static std::atomic<UJUSYNCSubsystem*> g_SubsystemInstance = nullptr;

static bool JUSYNCGameSessionActive()
{
    if (!GEngine)
    {
        return false;
    }
    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        if (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
        {
            UWorld* World = Context.World();
            if (World && !World->bIsTearingDown)
            {
                return true;
            }
        }
    }
    return false;
}


static bool JUSYNCMatHasVertexColorNode(UMaterialInterface* Material)
{
    if (!Material)
    {
        return false;
    }
    const UMaterial* BaseMat = Cast<UMaterial>(Material->GetMaterial());
    if (!BaseMat)
    {
        return false;
    }
    return BaseMat->HasAnyExpressionsInMaterialAndFunctionsOfType<UMaterialExpressionVertexColor>();
}

static bool JUSYNCMatHasScalarParam(UMaterialInterface* Material, const TCHAR* ParamName)
{
    if (!Material)
    {
        return false;
    }
    const UMaterial* BaseMat = Cast<UMaterial>(Material->GetMaterial());
    if (!BaseMat)
    {
        return false;
    }
    TArray<UMaterialExpressionScalarParameter*> ScalarParams;
    BaseMat->GetAllExpressionsInMaterialAndFunctionsOfType<UMaterialExpressionScalarParameter>(ScalarParams);
    for (UMaterialExpressionScalarParameter* Param : ScalarParams)
    {
        if (Param && Param->ParameterName == FName(ParamName))
        {
            return true;
        }
    }
    return false;
}

static bool JUSYNCMatHasBoundColorTexture(UMaterialInterface* Material)
{
    if (!Material)
    {
        return false;
    }
    const UMaterial* BaseMat = Cast<UMaterial>(Material->GetMaterial());
    if (!BaseMat)
    {
        return false;
    }
    TArray<UMaterialExpressionTextureSampleParameter2D*> TextureParams;
    BaseMat->GetAllExpressionsInMaterialAndFunctionsOfType<UMaterialExpressionTextureSampleParameter2D>(TextureParams);
    for (UMaterialExpressionTextureSampleParameter2D* Param : TextureParams)
    {
        if (!Param)
        {
            continue;
        }
        const FString ParamStr = Param->ParameterName.ToString();
        if (ParamStr.Contains(TEXT("Normal")) || ParamStr.Contains(TEXT("Rough")) ||
            ParamStr.Contains(TEXT("Metal")) || ParamStr.Contains(TEXT("Opacity")) ||
            ParamStr.Contains(TEXT("Mask")) || ParamStr.Contains(TEXT("AO")) ||
            ParamStr.Contains(TEXT("AmbientOcclusion")))
        {
            continue;
        }
        UTexture* BoundTexture = nullptr;
        if (Material->GetTextureParameterValue(Param->ParameterName, BoundTexture) && BoundTexture)
        {
            const FString TexName = BoundTexture->GetName();
            if (TexName.Contains(TEXT("Default")) || TexName.Contains(TEXT("None")) ||
                TexName.Contains(TEXT("TransparentBlack")))
            {
                continue;
            }
            return true;
        }
    }
    return false;
}

static bool JUSYNCColorsAreMeaningful(const TArray<FColor>& Colors)
{
    if (Colors.Num() < 2)
    {
        return false;
    }
    const FColor First = Colors[0];
    for (const FColor& C : Colors)
    {
        if (C != First)
        {
            return true;
        }
    }
    return false;
}

static UMaterialInterface* JUSYNCResolveMaterialForColoredMesh(
    UJUSYNCSubsystem* Subsystem,
    UMaterialInterface* Provided,
    const TArray<FColor>& Colors,
    UObject* Owner,
    const TCHAR* Context)
{
    if (!Provided || !JUSYNCColorsAreMeaningful(Colors))
    {
        return Provided;
    }
    if (JUSYNCMatHasVertexColorNode(Provided))
    {
        return Provided;
    }

    if (JUSYNCMatHasScalarParam(Provided, TEXT("UseVertexColor")))
    {
        UMaterialInstanceDynamic* DynMat = UMaterialInstanceDynamic::Create(Provided, Owner);
        if (DynMat)
        {
            DynMat->SetScalarParameterValue(TEXT("UseVertexColor"), 1.0f);
            UE_LOG(LogJUSYNC, Display, TEXT("[%s] Enabled UseVertexColor on provided material '%s'"),
                Context, *Provided->GetName());
            return DynMat;
        }
    }

    if (JUSYNCMatHasBoundColorTexture(Provided))
    {
        UE_LOG(LogJUSYNC, Verbose, TEXT("[%s] Provided material '%s' has a bound color texture; keeping it"),
            Context, *Provided->GetName());
        return Provided;
    }

    static const TCHAR* CandidatePaths[] = {
        TEXT("/Game/Materials/M_VertexColor"),
        TEXT("/Game/Datasets/M_VertexColor"),
        TEXT("/Game/M_VertexColor"),
    };
    if (Subsystem)
    {
        for (const TCHAR* Path : CandidatePaths)
        {
            UMaterialInterface* VertexColorMaterial = Subsystem->GetCachedMaterial(Path);
            if (VertexColorMaterial)
            {
                UE_LOG(LogJUSYNC, Display,
                    TEXT("[%s] Provided material '%s' ignores vertex colors and has no bound color texture; substituting %s"),
                    Context, *Provided->GetName(), Path);
                return VertexColorMaterial;
            }
        }
    }

    UE_LOG(LogJUSYNC, Warning,
        TEXT("[%s] Colored mesh but material '%s' ignores vertex colors and no M_VertexColor asset exists under /Game (create one: VertexColor node -> BaseColor)"),
        Context, *Provided->GetName());
    return Provided;
}

#ifdef WITH_ANARI_USD_MIDDLEWARE
static TArray<FJUSYNCWorkerStatus> g_CapturedWorkers;
static void WorkerListCallback(uint32_t count, const int32_t* ranks, const char** hostnames, const char** ips)
{
    g_CapturedWorkers.Empty(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        FJUSYNCWorkerStatus Status;
        Status.Rank = ranks[i];
        Status.Hostname = hostnames[i] ? FString(UTF8_TO_TCHAR(hostnames[i])) : TEXT("");
        Status.GpuInfo = TEXT("");
        Status.LastHeartbeat = 0;
        Status.Status = 1;
        g_CapturedWorkers.Add(Status);
    }
}

static TSet<FString> ProcessedFiles;
static FCriticalSection ProcessedFilesCriticalSection;

extern "C" void FileReceivedCallback_Static(const CFileData* file_data)
{
    UE_LOG(LogJUSYNC, Verbose, TEXT("File received callback triggered"));

    if (!file_data)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("FileReceivedCallback_Static: NULL file_data received"));
        return;
    }

    FString Filename = UTF8_TO_TCHAR(file_data->filename);

    {
        FScopeLock Lock(&ProcessedFilesCriticalSection);
        if (ProcessedFiles.Contains(Filename))
        {
            UE_LOG(LogJUSYNC, Log, TEXT("Skipping duplicate file: %s (already processed)"), *Filename);
            return;
        }
        ProcessedFiles.Add(Filename);

        if (ProcessedFiles.Num() >= 4096)
        {
            ProcessedFiles.Reset();
        }
    }

    UE_LOG(LogJUSYNC, Verbose, TEXT("ZMQ file received: %s (type=%s, %d bytes)"),
           *Filename, UTF8_TO_TCHAR(file_data->file_type), file_data->data_size);

    UJUSYNCSubsystem* Subsystem = g_SubsystemInstance.load();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("FileReceivedCallback_Static: g_SubsystemInstance is NULL"));
        return;
    }

    FString CapturedFilename = file_data->filename[0] != '\0' ? FString(UTF8_TO_TCHAR(file_data->filename)) : TEXT("");
    FString CapturedHash = file_data->hash[0] != '\0' ? FString(UTF8_TO_TCHAR(file_data->hash)) : TEXT("");
    FString CapturedFileType = file_data->file_type[0] != '\0' ? FString(UTF8_TO_TCHAR(file_data->file_type)) : TEXT("");

    TArray<uint8> DataCopy;
    if (file_data->data && file_data->data_size > 0)
    {
        DataCopy.SetNum(file_data->data_size);
        FMemory::Memcpy(DataCopy.GetData(), file_data->data, file_data->data_size);
    }

    AsyncTask(ENamedThreads::GameThread, [CapturedFilename = MoveTemp(CapturedFilename), CapturedHash = MoveTemp(CapturedHash), CapturedFileType = MoveTemp(CapturedFileType), DataCopy = MoveTemp(DataCopy)]()
    {
        UJUSYNCSubsystem* Subsystem = g_SubsystemInstance.load();
        if (!Subsystem)
        {
            UE_LOG(LogJUSYNC, Error, TEXT("Async Task: g_SubsystemInstance is NULL on game thread"));
            return;
        }
        if (!JUSYNCGameSessionActive())
        {
            return;
        }

        FJUSYNCFileData UEFileData;
        UEFileData.Filename = CapturedFilename;
        UEFileData.Hash = CapturedHash;
        UEFileData.FileType = CapturedFileType;
        UEFileData.Data = DataCopy;

        Subsystem->HandleFileReceivedForLibrary(UEFileData);
        Subsystem->OnFileReceived.Broadcast(UEFileData);

    });
}

extern "C" void FileReceivedSpanCallback_Static(
    const char* filename,
    const unsigned char* /*data*/,
    size_t data_size,
    const char* hash,
    const char* file_type)
{
    UE_LOG(LogJUSYNC, Verbose, TEXT("File received span callback triggered"));

    if (!filename)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("FileReceivedSpanCallback_Static: NULL filename received"));
        return;
    }

    FString CapturedFilename = FString(UTF8_TO_TCHAR(filename));

    {
        FScopeLock Lock(&ProcessedFilesCriticalSection);
        if (ProcessedFiles.Contains(CapturedFilename))
        {
            UE_LOG(LogJUSYNC, Log, TEXT("Skipping duplicate file: %s (already processed)"), *CapturedFilename);
            return;
        }
        ProcessedFiles.Add(CapturedFilename);

        if (ProcessedFiles.Num() >= 4096)
        {
            ProcessedFiles.Reset();
        }
    }

    UE_LOG(LogJUSYNC, Verbose, TEXT("ZMQ file received (metadata only): %s (type=%s, %d bytes)"),
        *CapturedFilename, file_type ? UTF8_TO_TCHAR(file_type) : TEXT(""), static_cast<int32>(data_size));

    UJUSYNCSubsystem* Subsystem = g_SubsystemInstance.load();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("FileReceivedSpanCallback_Static: g_SubsystemInstance is NULL"));
        return;
    }

    FString CapturedHash = hash ? FString(UTF8_TO_TCHAR(hash)) : TEXT("");
    FString CapturedFileType = file_type ? FString(UTF8_TO_TCHAR(file_type)) : TEXT("");

    AsyncTask(ENamedThreads::GameThread, [CapturedFilename = MoveTemp(CapturedFilename),
        CapturedHash = MoveTemp(CapturedHash),
        CapturedFileType = MoveTemp(CapturedFileType)]()
    {
        UJUSYNCSubsystem* Subsystem = g_SubsystemInstance.load();
        if (!Subsystem)
        {
            UE_LOG(LogJUSYNC, Error, TEXT("FileReceivedSpanCallback_Static: Subsystem null on game thread"));
            return;
        }
        if (!JUSYNCGameSessionActive())
        {
            return;
        }

        FJUSYNCFileData UEFileData;
        UEFileData.Filename = CapturedFilename;
        UEFileData.Hash = CapturedHash;
        UEFileData.FileType = CapturedFileType;

        Subsystem->HandleFileReceivedForLibrary(UEFileData);
        Subsystem->OnFileReceived.Broadcast(UEFileData);
    });
}

extern "C" void MessageReceivedCallback_Static(const char* message)
{
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
        if (Subsystem && JUSYNCGameSessionActive())
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

static bool IsNotificationForSkippableFile(const char* filename)
{
    if (!filename) return false;
    const char* p = filename;
    while (*p)
    {
        if (p[0] == 'i' && p[1] == 'm' && p[2] == 'a' && p[3] == 'g' && p[4] == 'e' && p[5] == 's' && p[6] == '/') return true;
        if (p[0] == 's' && p[1] == 'h' && p[2] == 'a' && p[3] == 'r' && p[4] == 'e' && p[5] == 'd' && p[6] == '/') return true;
        if (p[0] == 'm' && p[1] == 'a' && p[2] == 'n' && p[3] == 'i' && p[4] == 'f' && p[5] == 'e' && p[6] == 's' && p[7] == 't') return true;
        if (p[0] == 'S' && p[1] == 'e' && p[2] == 's' && p[3] == 's' && p[4] == 'i' && p[5] == 'o' && p[6] == 'n' && p[7] == '_') return true;
        if (p[0] == 'p' && p[1] == 'r' && p[2] == 'i' && p[3] == 'm' && p[4] == 's' && p[5] == 't' && p[6] == 'a' && p[7] == 'g' && p[8] == 'e' && p[9] == 's') return true;
        if ((p[0] == '.' || p[0] == '/') && (p[1] == 'p' || p[1] == 'P') && (p[2] == 'n' || p[2] == 'N') && (p[3] == 'g' || p[3] == 'G')) return true;
        p++;
    }
    return false;
}

extern "C" void NotificationCallback_Static(uint32_t messageType, int32_t sourceRank,
                                              const char* filename, uint64_t fileSize, uint64_t timestamp,
                                              uint64_t hashLo, uint64_t hashHi,
                                              uint64_t hashPrevLo, uint64_t hashPrevHi,
                                              bool hasOldData)
{
    if (messageType != 301 && IsNotificationForSkippableFile(filename))
        return;

    FString NotifType = TEXT("NOTIFY_UNKNOWN");
    if (messageType == 302)      NotifType = TEXT("NOTIFY_FILE_UPDATE_V2");
    else if (messageType == 301) NotifType = TEXT("NOTIFY_COMMIT_COMPLETE");
    else if (messageType == 300) NotifType = TEXT("NOTIFY_FILE_UPDATE");

    UE_LOG(LogJUSYNC, Display, TEXT("ZMQ NOTIFICATION: %s (rank %d, file '%s', %llu bytes, hash %llx:%llx, old %llx:%llx, diff=%d)"),
           *NotifType, sourceRank, filename ? UTF8_TO_TCHAR(filename) : TEXT("(none)"),
           static_cast<unsigned long long>(fileSize),
           static_cast<unsigned long long>(hashLo),
           static_cast<unsigned long long>(hashHi),
           static_cast<unsigned long long>(hashPrevLo),
           static_cast<unsigned long long>(hashPrevHi),
           hasOldData ? 1 : 0);

    FString FilenameCopy = filename ? FString(UTF8_TO_TCHAR(filename)) : TEXT("");

    AsyncTask(ENamedThreads::GameThread, [FilenameCopy, messageType, sourceRank, fileSize, timestamp,
                                           hashLo, hashHi, hashPrevLo, hashPrevHi, hasOldData]()
    {
        UJUSYNCSubsystem* Subsystem = g_SubsystemInstance.load();
        if (!Subsystem)
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("NotificationCallback_Static: Subsystem instance is null, notification dropped"));
            return;
        }
        if (!JUSYNCGameSessionActive())
        {
            return;
        }

        EJUSYNCNotificationType NotifType;
        switch (messageType)
        {
            case 302:  NotifType = EJUSYNCNotificationType::FileUpdateV2; break;
            case 301:  NotifType = EJUSYNCNotificationType::CommitComplete; break;
            case 300:  default:  NotifType = EJUSYNCNotificationType::FileUpdate; break;
        }

        FJUSYNCNotification Notification;
        Notification.Type         = NotifType;
        Notification.SourceRank   = sourceRank;
        Notification.Filename     = FilenameCopy;
        Notification.FileSize     = static_cast<int64>(fileSize);
        Notification.Timestamp    = static_cast<int64>(timestamp);
        Notification.HashLo       = hashLo;
        Notification.HashHi       = hashHi;
        Notification.HashPrevLo   = hashPrevLo;
        Notification.HashPrevHi   = hashPrevHi;
        Notification.bHasOldData  = hasOldData;

        UE_LOG(LogJUSYNC, Log, TEXT("Broadcasting notification to Blueprint: %s '%s'"),
                (NotifType == EJUSYNCNotificationType::FileUpdateV2) ? TEXT("FileUpdateV2") :
                (NotifType == EJUSYNCNotificationType::CommitComplete) ? TEXT("CommitComplete") : TEXT("FileUpdate"),
                *Notification.Filename);
        Subsystem->OnNotificationReceived.Broadcast(Notification);
    });
}

static EJUSYNCSceneChangeType ConvertCSceneChangeType(int32 ChangeType)
{
    switch (ChangeType)
    {
        case 1:  return EJUSYNCSceneChangeType::Created;
        case 2:  return EJUSYNCSceneChangeType::Removed;
        case 3:  return EJUSYNCSceneChangeType::Visibility;
        case 4:  return EJUSYNCSceneChangeType::Transform;
        case 5:  return EJUSYNCSceneChangeType::Material;
        case 6:  return EJUSYNCSceneChangeType::Attribute;
        case 7:  return EJUSYNCSceneChangeType::Commit;
        case 8:  return EJUSYNCSceneChangeType::Property;
        case 0:
        default: return EJUSYNCSceneChangeType::None;
    }
}

static EJUSYNCPropertyValueType ConvertCPropertyValueType(int32 ValueType)
{
    switch (ValueType)
    {
        case 1:  return EJUSYNCPropertyValueType::Int;
        case 2:  return EJUSYNCPropertyValueType::Bool;
        case 3:  return EJUSYNCPropertyValueType::Float;
        case 4:  return EJUSYNCPropertyValueType::Float2;
        case 5:  return EJUSYNCPropertyValueType::Float3;
        case 6:  return EJUSYNCPropertyValueType::Float4;
        case 7:  return EJUSYNCPropertyValueType::String;
        case 8:  return EJUSYNCPropertyValueType::Path;
        case 9:  return EJUSYNCPropertyValueType::ArrayRef;
        case 0:
        default: return EJUSYNCPropertyValueType::None;
    }
}

extern "C" void JUSYNCSceneUpdateCallback_Static(
    uint32_t messageType,
    int32_t sourceRank,
    uint64_t timestamp,
    uint64_t commitId,
    uint64_t revision,
    const char* primPath,
    const char* propertyName,
    int32_t changeType,
    int32_t valueType,
    int64_t intValue,
    float floatValue,
    const float* vec4,
    const char* stringValue,
    uint32_t payloadSize)
{
    const FString PrimPathCopy = primPath ? FString(UTF8_TO_TCHAR(primPath)) : TEXT("");
    const FString PropertyNameCopy = propertyName ? FString(UTF8_TO_TCHAR(propertyName)) : TEXT("");
    const FString StringValueCopy = stringValue ? FString(UTF8_TO_TCHAR(stringValue)) : TEXT("");

    const FVector4 Vec4Copy = vec4
        ? FVector4(static_cast<double>(vec4[0]), static_cast<double>(vec4[1]), static_cast<double>(vec4[2]), static_cast<double>(vec4[3]))
        : FVector4(0.0, 0.0, 0.0, 0.0);

    UE_LOG(LogJUSYNC, Display,
        TEXT("SCENE/PROPERTY UPDATE: type=%u rank=%d prim='%s' property='%s' change=%d valueType=%d commit=%llu revision=%llu int=%lld float=%f vec=(%f,%f,%f,%f) string='%s' payload=%u"),
        messageType,
        sourceRank,
        *PrimPathCopy,
        *PropertyNameCopy,
        changeType,
        valueType,
        static_cast<unsigned long long>(commitId),
        static_cast<unsigned long long>(revision),
        static_cast<long long>(intValue),
        static_cast<double>(floatValue),
        Vec4Copy.X, Vec4Copy.Y, Vec4Copy.Z, Vec4Copy.W,
        *StringValueCopy,
        payloadSize);

    AsyncTask(ENamedThreads::GameThread, [messageType, sourceRank, timestamp, commitId, revision,
                                           PrimPathCopy, PropertyNameCopy, changeType, valueType,
                                           intValue, floatValue, Vec4Copy, StringValueCopy, payloadSize]()
    {
        UJUSYNCSubsystem* Subsystem = g_SubsystemInstance.load();
        if (!Subsystem)
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("JUSYNCSceneUpdateCallback_Static: Subsystem instance is null, scene update dropped"));
            return;
        }
        if (!JUSYNCGameSessionActive())
        {
            return;
        }

        FJUSYNCSceneUpdate Update;
        Update.MessageType      = static_cast<int32>(messageType);
        Update.SourceRank       = sourceRank;
        Update.Timestamp        = static_cast<int64>(timestamp);
        Update.CommitId         = static_cast<int64>(commitId);
        Update.Revision         = static_cast<int64>(revision);
        Update.PrimPath         = PrimPathCopy;
        Update.PropertyName     = PropertyNameCopy;
        Update.ChangeType       = ConvertCSceneChangeType(changeType);
        Update.ValueType        = ConvertCPropertyValueType(valueType);
        Update.IntValue         = intValue;
        Update.FloatValue       = floatValue;
        Update.Vec4             = Vec4Copy;
        Update.StringValue      = StringValueCopy;
        Update.PayloadSize      = static_cast<int32>(payloadSize);

        Subsystem->OnSceneUpdateReceived.Broadcast(Update);
    });
}

extern "C" void JUSYNCProtocolDiagnosticsCallback_Static(
    const char* event,
    const char* message,
    uint64_t value0,
    uint64_t value1)
{
    const FString EventCopy = event ? FString(UTF8_TO_TCHAR(event)) : TEXT("unknown");
    const FString MessageCopy = message ? FString(UTF8_TO_TCHAR(message)) : TEXT("");

    UE_LOG(LogJUSYNC, Display,
        TEXT("PROTOCOL DIAGNOSTICS: %s | %s (%llu, %llu)"),
        *EventCopy,
        *MessageCopy,
        static_cast<unsigned long long>(value0),
        static_cast<unsigned long long>(value1));

    AsyncTask(ENamedThreads::GameThread, [EventCopy, MessageCopy, value0, value1]()
    {
        UJUSYNCSubsystem* Subsystem = g_SubsystemInstance.load();
        if (!Subsystem)
        {
            return;
        }
        if (!JUSYNCGameSessionActive())
        {
            return;
        }

        FJUSYNCProtocolDiagnostics Diagnostics;
        Diagnostics.Event   = EventCopy;
        Diagnostics.Message = MessageCopy;
        Diagnostics.Value0  = static_cast<int64>(value0);
        Diagnostics.Value1  = static_cast<int64>(value1);

        Subsystem->OnProtocolDiagnosticsReceived.Broadcast(Diagnostics);
    });
}

static FJUSYNCMeshData ConvertCMeshDataToUE_Helper(const CMeshData& CMesh, bool bForceVertexInterpolation = true)
{
    FJUSYNCMeshData UEMesh;

    UEMesh.ElementName = FString(UTF8_TO_TCHAR(CMesh.element_name));
    UEMesh.TypeName = FString(UTF8_TO_TCHAR(CMesh.type_name));

    size_t PointCount = CMesh.points_count / 3;
    UEMesh.Vertices.Reserve(PointCount);

    const float* pointsPtr = CMesh.points;
    for (size_t i = 0; i < PointCount; ++i)
    {
        size_t idx = i * 3;
        UEMesh.Vertices.Add(FVector(pointsPtr[idx], -pointsPtr[idx + 1], pointsPtr[idx + 2]));
    }

    size_t IndexCount = CMesh.indices_count;
    UEMesh.Triangles.Reserve(IndexCount);

    const uint32_t* indicesPtr = CMesh.indices;
    UEMesh.Triangles.AddUninitialized(IndexCount);
    for (size_t i = 0; i < IndexCount; ++i)
    {
        UEMesh.Triangles[i] = static_cast<int32>(indicesPtr[i]);
    }

    if (CMesh.normals && CMesh.normals_count >= 3)
    {
        size_t NormalCount = CMesh.normals_count / 3;
        UEMesh.Normals.SetNum(NormalCount);

        const float* normalsPtr = CMesh.normals;
        for (size_t i = 0; i < NormalCount; ++i)
        {
            size_t idx = i * 3;
            UEMesh.Normals[i] = FVector(normalsPtr[idx], -normalsPtr[idx + 1], normalsPtr[idx + 2]);
        }

        for (int32 i = 0; i < UEMesh.Normals.Num(); ++i)
        {
            UEMesh.Normals[i].Normalize();
        }
    }

    if (CMesh.uvs && CMesh.uvs_count >= 2)
    {
        size_t UVCount = CMesh.uvs_count / 2;
        UEMesh.UVs.SetNum(UVCount);

        const float* uvsPtr = CMesh.uvs;
        for (size_t i = 0; i < UVCount; ++i)
        {
            size_t idx = i * 2;
            UEMesh.UVs[i] = FVector2D(uvsPtr[idx], uvsPtr[idx + 1]);
        }
    }

    if (CMesh.vertex_colors && CMesh.vertex_colors_count >= 4)
    {
        int32 VertexCount = static_cast<int32>(PointCount);
        int32 FaceCount = static_cast<int32>(UEMesh.Triangles.Num() / 3);
        int32 ColorCount = static_cast<int32>(CMesh.vertex_colors_count / 4);

        bool bDetectedVertexInterp = (ColorCount == VertexCount);
        bool bDetectedUniformInterp = (ColorCount == FaceCount);

        UEMesh.VertexColors.SetNum(VertexCount);

        if (bDetectedVertexInterp)
        {
            const float* colorsPtr = CMesh.vertex_colors;
            for (int32 i = 0; i < VertexCount; ++i)
            {
                int64 idx = int64(i) * 4;
                uint8 r = uint8(FMath::Clamp(colorsPtr[idx + 0] * 255.0f, 0.0f, 255.0f));
                uint8 g = uint8(FMath::Clamp(colorsPtr[idx + 1] * 255.0f, 0.0f, 255.0f));
                uint8 b = uint8(FMath::Clamp(colorsPtr[idx + 2] * 255.0f, 0.0f, 255.0f));
                uint8 a = uint8(FMath::Clamp(colorsPtr[idx + 3] * 255.0f, 0.0f, 255.0f));
                UEMesh.VertexColors[i] = FColor(r, g, b, a);
            }
        }
        else if (bDetectedUniformInterp && bForceVertexInterpolation)
        {

            TArray<FLinearColor> AccumulatedColors;
            TArray<int32> ColorCounts;
            AccumulatedColors.SetNumZeroed(VertexCount);
            ColorCounts.SetNumZeroed(VertexCount);

            for (int32 FaceIdx = 0; FaceIdx < FaceCount && FaceIdx < ColorCount; ++FaceIdx)
            {
                int32 i0 = UEMesh.Triangles[FaceIdx * 3 + 0];
                int32 i1 = UEMesh.Triangles[FaceIdx * 3 + 1];
                int32 i2 = UEMesh.Triangles[FaceIdx * 3 + 2];

                if (i0 < VertexCount && i1 < VertexCount && i2 < VertexCount)
                {
                    int64 cidx = int64(FaceIdx) * 4;

                    FLinearColor FaceColor(
                        CMesh.vertex_colors[cidx + 0],
                        CMesh.vertex_colors[cidx + 1],
                        CMesh.vertex_colors[cidx + 2],
                        CMesh.vertex_colors[cidx + 3]
                    );

                    AccumulatedColors[i0] += FaceColor;
                    AccumulatedColors[i1] += FaceColor;
                    AccumulatedColors[i2] += FaceColor;

                    ColorCounts[i0]++;
                    ColorCounts[i1]++;
                    ColorCounts[i2]++;
                }
            }

            for (int32 i = 0; i < VertexCount; ++i)
            {
                FLinearColor FinalColor;
                if (ColorCounts[i] > 0)
                {
                    FinalColor = AccumulatedColors[i] / ColorCounts[i];
                }
                else
                {
                    FinalColor = FLinearColor::White;
                }

                uint8 r = uint8(FMath::Clamp(FinalColor.R * 255.0f, 0.0f, 255.0f));
                uint8 g = uint8(FMath::Clamp(FinalColor.G * 255.0f, 0.0f, 255.0f));
                uint8 b = uint8(FMath::Clamp(FinalColor.B * 255.0f, 0.0f, 255.0f));
                uint8 a = uint8(FMath::Clamp(FinalColor.A * 255.0f, 0.0f, 255.0f));

                UEMesh.VertexColors.Add(FColor(r, g, b, a));
            }

            UE_LOG(LogJUSYNC, Log, TEXT("Converted uniform to smooth vertex interpolation: %d vertex colors"),
                    UEMesh.VertexColors.Num());
        }
        else if (bDetectedUniformInterp && !bForceVertexInterpolation)
        {
            UE_LOG(LogJUSYNC, Log, TEXT("Using original UNIFORM interpolation (flat shading)"));
            UEMesh.VertexColors.Empty();
            UEMesh.VertexColors.SetNum(FaceCount * 3);

            for (int32 f = 0; f < FaceCount; ++f)
            {
                int64 cidx = int64(f) * 4;
                uint8 r = uint8(FMath::Clamp(CMesh.vertex_colors[cidx + 0] * 255.0f, 0.0f, 255.0f));
                uint8 g = uint8(FMath::Clamp(CMesh.vertex_colors[cidx + 1] * 255.0f, 0.0f, 255.0f));
                uint8 b = uint8(FMath::Clamp(CMesh.vertex_colors[cidx + 2] * 255.0f, 0.0f, 255.0f));
                uint8 a = uint8(FMath::Clamp(CMesh.vertex_colors[cidx + 3] * 255.0f, 0.0f, 255.0f));
                FColor faceColor(r, g, b, a);

                int32 base = f * 3;
                UEMesh.VertexColors[base] = faceColor;
                UEMesh.VertexColors[base + 1] = faceColor;
                UEMesh.VertexColors[base + 2] = faceColor;
            }
        }
        else
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Using fallback vertex interpolation"));
            for (int32 i = 0; i < VertexCount; ++i)
            {
                if (i < ColorCount)
                {
                    int64 idx = int64(i) * 4;
                    uint8 r = uint8(FMath::Clamp(CMesh.vertex_colors[idx + 0] * 255.0f, 0.0f, 255.0f));
                    uint8 g = uint8(FMath::Clamp(CMesh.vertex_colors[idx + 1] * 255.0f, 0.0f, 255.0f));
                    uint8 b = uint8(FMath::Clamp(CMesh.vertex_colors[idx + 2] * 255.0f, 0.0f, 255.0f));
                    uint8 a = uint8(FMath::Clamp(CMesh.vertex_colors[idx + 3] * 255.0f, 0.0f, 255.0f));
                    UEMesh.VertexColors[i] = FColor(r, g, b, a);
                }
                else
                {
                    UEMesh.VertexColors[i] = FColor::White;
                }
            }
        }

        UE_LOG(LogJUSYNC, Log, TEXT("Final vertex colors: %d"), UEMesh.VertexColors.Num());

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

    g_SubsystemInstance.store(this);

    PCSpawner = MakeUnique<FJUSYNCPointCloudSpawner>(TWeakObjectPtr<UJUSYNCSubsystem>(this));
    PCSpawner->SetMaxPoolSize(16);
    PCSpawner->SetBudgetMs(33.0f);

    MeshMorpher = MakeUnique<FJUSYNCMeshMorpher>(TWeakObjectPtr<UJUSYNCSubsystem>(this));

    ApplyLidarPerformanceCvars();

    UE_LOG(LogJUSYNC, Log, TEXT("Global instance set: %p"), g_SubsystemInstance.load());
    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNCSubsystem initialized with C-wrapper interface"));
}

void UJUSYNCSubsystem::ApplyLidarPerformanceCvars() const
{
    if (!GEngine)
    {
        return;
    }

    GEngine->Exec(nullptr, *FString::Printf(TEXT("r.LidarPointBudget %d"), LidarPointBudget));
    GEngine->Exec(nullptr, *FString::Printf(TEXT("r.LidarIncrementalBudget %d"), bLidarIncrementalBudget ? 1 : 0));
    GEngine->Exec(nullptr, *FString::Printf(TEXT("r.LidarScreenCenterImportance %.3f"), LidarScreenCenterImportance));
    GEngine->Exec(nullptr, *FString::Printf(TEXT("r.LidarBaseLODImportance %.3f"), LidarBaseLODImportance));

    UE_LOG(LogJUSYNC, Log, TEXT("LiDAR cvars: Budget=%d Incremental=%d ScreenCenter=%.2f BaseLOD=%.2f"),
        LidarPointBudget, bLidarIncrementalBudget ? 1 : 0, LidarScreenCenterImportance, LidarBaseLODImportance);
}

void UJUSYNCSubsystem::Deinitialize()
{
    ShutdownMiddleware();

    g_SubsystemInstance.store(nullptr);

    OnFileReceived.Clear();
    OnMessageReceived.Clear();
    OnNotificationReceived.Clear();
    OnSceneUpdateReceived.Clear();
    OnProtocolDiagnosticsReceived.Clear();

    if (PCSpawner.IsValid())
    {
        PCSpawner->WaitForCompletion(10.0f);
    }

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

    UE_LOG(LogJUSYNC, Log, TEXT("Requested Endpoint: %s"), *Endpoint);
    UE_LOG(LogJUSYNC, Log, TEXT("Subsystem instance: %p"), this);
    UE_LOG(LogJUSYNC, Log, TEXT("Global instance: %p"), g_SubsystemInstance.load());

    if (!g_SubsystemInstance.load())
    {
        g_SubsystemInstance.store(this);
        UE_LOG(LogJUSYNC, Log, TEXT("Set global instance: %p"), g_SubsystemInstance.load());
    }

#ifdef WITH_ANARI_USD_MIDDLEWARE
    FTCHARToUTF8 EndpointConverter(*Endpoint);
    const char* EndpointCStr = Endpoint.IsEmpty() ? "tcp://*:5556" : EndpointConverter.Get();

    UE_LOG(LogJUSYNC, Log, TEXT("Using C endpoint: %s"), UTF8_TO_TCHAR(EndpointCStr));

    UE_LOG(LogJUSYNC, Log, TEXT("Calling InitializeMiddleware_C..."));
    int Result = InitializeMiddleware_C(EndpointCStr);

    UE_LOG(LogJUSYNC, Log, TEXT("InitializeMiddleware_C returned: %d"), Result);

    bIsInitialized.store(Result == 1);

    if (Result == 1)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("USD processors initialized (DEALER-only mode)"));
        UE_LOG(LogJUSYNC, Log, TEXT("Ready to connect to broker via DEALER socket"));

        UE_LOG(LogJUSYNC, Log, TEXT("Registering ZMQ callbacks (deferred)..."));
        RegisterUpdateCallbackSpan_C(FileReceivedSpanCallback_Static);
        RegisterMessageCallback_C(MessageReceivedCallback_Static);
        UE_LOG(LogJUSYNC, Log, TEXT("File & message callbacks registered"));

        int ConnectionStatus = IsConnected_C();
        UE_LOG(LogJUSYNC, Log, TEXT("Connection status check: %d"), ConnectionStatus);

        const char* StatusInfo = GetStatusInfo_C();
        UE_LOG(LogJUSYNC, Log, TEXT("Middleware status: %s"), UTF8_TO_TCHAR(StatusInfo));

        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC middleware initialized (DEALER-only mode)"));
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to initialize JUSYNC Middleware (Result: %d)"), Result);
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

#ifdef WITH_ANARI_USD_MIDDLEWARE
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

    static int32 StatusCheckCount = 0;
    if (++StatusCheckCount >= 10000) { StatusCheckCount = 0; }
    if (StatusCheckCount == 0)
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

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot start receiving - middleware not initialized"));
        return false;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Middleware is initialized, calling StartReceiving_C..."));

    int Result = StartReceiving_C();

    UE_LOG(LogJUSYNC, Log, TEXT("StartReceiving_C returned: %d"), Result);

    if (Result == 1)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Started Receiving Data"));
        UE_LOG(LogJUSYNC, Log, TEXT("ROUTER is now listening for DEALER messages"));

        int ConnectionStatus = IsConnected_C();
        UE_LOG(LogJUSYNC, Log, TEXT("Post-start connection status: %d"), ConnectionStatus);

        const char* StatusInfo = GetStatusInfo_C();
        UE_LOG(LogJUSYNC, Log, TEXT("Post-start middleware status: %s"), UTF8_TO_TCHAR(StatusInfo));

        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Start receiving: Success"));
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to start receiving (Result: %d)"), Result);
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Start receiving: Failed"));
    }

    return Result == 1;
#endif
    return false;
}


void UJUSYNCSubsystem::StopReceiving()
{
    FScopeLock Lock(&MiddlewareMutex);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    StopReceiving_C();
    UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Stopped receiving"));
#endif
}

void UJUSYNCSubsystem::HandleFileReceivedForLibrary(const FJUSYNCFileData& FileData)
{
    UE_LOG(LogJUSYNC, Log, TEXT("File: %s (%d bytes, %s)"),
           *FileData.Filename, FileData.Data.Num(), *FileData.FileType);

    FScopeLock Lock(&UJUSYNCBlueprintLibrary::DataMutex);

    int32 PreviousCount = UJUSYNCBlueprintLibrary::ReceivedFiles.Num();
    UJUSYNCBlueprintLibrary::ReceivedFiles.Add(FileData);
    int32 NewCount = UJUSYNCBlueprintLibrary::ReceivedFiles.Num();

    UE_LOG(LogJUSYNC, Log, TEXT("Blueprint Library file count: %d -> %d"), PreviousCount, NewCount);
    UE_LOG(LogJUSYNC, Log, TEXT("File added to Blueprint Library: %s"), *FileData.Filename);
}

void UJUSYNCSubsystem::HandleMessageReceivedForLibrary(const FString& Message)
{
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

    FTCHARToUTF8 FilenameConverter(*Filename);
    const char* FilenameCStr = FilenameConverter.Get();

    CMeshData* CMeshes = nullptr;
    size_t MeshCount = 0;

    int Result = 0;
    {
        FScopeLock Lock(&ParseMutex);
        Result = LoadUSDBuffer_C(Buffer.GetData(), Buffer.Num(), FilenameCStr, &CMeshes, &MeshCount);
    }

    if (Result == 1 && CMeshes && MeshCount > 0)
    {
        OutMeshData.Empty();
        OutMeshData.Reserve(MeshCount);

        for (size_t i = 0; i < MeshCount; ++i)
        {
            FJUSYNCMeshData UEMeshData = ConvertCMeshDataToUE_Helper(CMeshes[i]);
            OutMeshData.Add(UEMeshData);
        }

        FreeMeshData_C(CMeshes, MeshCount);

        UE_LOG(LogJUSYNC, Log, TEXT("Loaded %d meshes from USD buffer"), OutMeshData.Num());
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

    FTCHARToUTF8 FilePathConverter(*FilePath);
    const char* FilePathCStr = FilePathConverter.Get();

    CMeshData* CMeshes = nullptr;
    size_t MeshCount = 0;

    int Result = LoadUSDFromDisk_C(FilePathCStr, &CMeshes, &MeshCount);

    if (Result == 1 && CMeshes && MeshCount > 0)
    {
        OutMeshData.Empty();
        OutMeshData.Reserve(MeshCount);

        for (size_t i = 0; i < MeshCount; ++i)
        {
            FJUSYNCMeshData UEMeshData = ConvertCMeshDataToUE_Helper(CMeshes[i]);
            OutMeshData.Add(UEMeshData);
        }

        FreeMeshData_C(CMeshes, MeshCount);

        UE_LOG(LogJUSYNC, Log, TEXT("Loaded %d meshes from USD file"), OutMeshData.Num());
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

    FScopeLock Lock(&ParseMutex);

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
            pc.BoundingBoxMin = FVector(cpc.bounding_box_min[0], cpc.bounding_box_min[2], -cpc.bounding_box_min[1]);
            pc.BoundingBoxMax = FVector(cpc.bounding_box_max[0], cpc.bounding_box_max[2], -cpc.bounding_box_max[1]);

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

bool UJUSYNCSubsystem::LoadUSDFullFromBufferNoCopy(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCMeshData>& OutMeshData, TArray<FJUSYNCPointCloudData>& OutPointCloudData)
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC: LoadUSDFullFromBufferNoCopy called but middleware is not initialized"));
        return false;
    }

    OutMeshData.Empty();
    OutPointCloudData.Empty();

    if (Buffer.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadUSDFullFromBufferNoCopy called with empty buffer"));
        return false;
    }

    FTCHARToUTF8 FilenameConverter(*Filename);
    const char* FilenameCStr = FilenameConverter.Get();

    void* ParseHandle = nullptr;
    CMeshLayout* MeshLayouts = nullptr;
    size_t MeshCount = 0;
    CPointCloudLayout* CloudLayouts = nullptr;
    size_t CloudCount = 0;

    // C-call), serialized on ParseMutex like before.

    int Result = 0;
    {
        FScopeLock Lock(&ParseMutex);
        Result = QueryUSDFullLayout_C(
            Buffer.GetData(), Buffer.Num(), FilenameCStr,
            &ParseHandle, &MeshLayouts, &MeshCount,
            &CloudLayouts, &CloudCount
        );
    }

    if (Result != 1 || !ParseHandle)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadUSDFullFromBufferNoCopy failed for '%s' (buffer=%d bytes, C_result=%d MeshCount=%llu CloudCount=%llu)"),
            *Filename, Buffer.Num(), Result, (uint64)MeshCount, (uint64)CloudCount);
        if (ParseHandle) FreeParsedUSD_C(ParseHandle);
        FreeUSDFullLayouts_C(MeshLayouts, MeshCount, CloudLayouts, CloudCount);
        return false;
    }

    if (MeshCount > 0)
    {
        OutMeshData.SetNum(static_cast<int32>(MeshCount));
        CMeshDataFill* FillMeshes = reinterpret_cast<CMeshDataFill*>(FMemory::Malloc(MeshCount * sizeof(CMeshDataFill)));
        FMemory::Memzero(FillMeshes, MeshCount * sizeof(CMeshDataFill)); // POD struct

        for (size_t i = 0; i < MeshCount; ++i)
        {
            const CMeshLayout& lay = MeshLayouts[i];
            FJUSYNCMeshData& UEMesh = OutMeshData[i];
            CMeshDataFill& dst = FillMeshes[i];

            UEMesh.ElementName = UTF8_TO_TCHAR(lay.element_name);
            UEMesh.TypeName = UTF8_TO_TCHAR(lay.type_name);

            if (lay.points_count > 0) UEMesh.Vertices.SetNumUninitialized(static_cast<int32>(lay.points_count));
            if (lay.indices_count > 0) UEMesh.Triangles.SetNumUninitialized(static_cast<int32>(lay.indices_count));
            if (lay.normals_count > 0) UEMesh.Normals.SetNumUninitialized(static_cast<int32>(lay.normals_count));
            if (lay.uvs_count > 0) UEMesh.UVs.SetNumUninitialized(static_cast<int32>(lay.uvs_count));
            if (lay.vertex_colors_count > 0) UEMesh.VertexColors.SetNumUninitialized(static_cast<int32>(lay.vertex_colors_count));

            dst.points = UEMesh.Vertices.Num() > 0 ? reinterpret_cast<double*>(UEMesh.Vertices.GetData()) : nullptr;
            dst.indices = UEMesh.Triangles.Num() > 0 ? UEMesh.Triangles.GetData() : nullptr;
            dst.normals = UEMesh.Normals.Num() > 0 ? reinterpret_cast<double*>(UEMesh.Normals.GetData()) : nullptr;
            dst.uvs = UEMesh.UVs.Num() > 0 ? reinterpret_cast<double*>(UEMesh.UVs.GetData()) : nullptr;
            dst.vertex_colors8 = UEMesh.VertexColors.Num() > 0 ? reinterpret_cast<unsigned char*>(UEMesh.VertexColors.GetData()) : nullptr;
        }

        const int FillResult = FillUSDFull_C(ParseHandle, FillMeshes, MeshCount, nullptr, 0);
        FMemory::Free(FillMeshes);
        if (FillResult != 1)
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC: FillUSDFull_C (meshes) failed for '%s'"), *Filename);
            FreeParsedUSD_C(ParseHandle);
            FreeUSDFullLayouts_C(MeshLayouts, MeshCount, CloudLayouts, CloudCount);
            return false;
        }

        for (int32 i = 0; i < OutMeshData.Num(); ++i)
        {
            TArray<FVector>& Normals = OutMeshData[i].Normals;
            for (int32 j = 0; j < Normals.Num(); ++j)
            {
                Normals[j].Normalize();
            }
        }
    }

    if (CloudCount > 0)
    {
        OutPointCloudData.SetNum(static_cast<int32>(CloudCount));
        CPointCloudDataFill* FillClouds = reinterpret_cast<CPointCloudDataFill*>(FMemory::Malloc(CloudCount * sizeof(CPointCloudDataFill)));
        FMemory::Memzero(FillClouds, CloudCount * sizeof(CPointCloudDataFill)); // POD struct

        for (size_t i = 0; i < CloudCount; ++i)
        {
            const CPointCloudLayout& lay = CloudLayouts[i];
            FJUSYNCPointCloudData& pc = OutPointCloudData[i];
            CPointCloudDataFill& dst = FillClouds[i];

            pc.ElementName = UTF8_TO_TCHAR(lay.element_name);
            pc.TypeName = TEXT("GeomPoints");
            pc.PointCount = static_cast<int32>(lay.point_count);
            pc.bHasColors = lay.has_colors != 0;
            pc.bHasNormals = lay.has_normals != 0;

            const bool bLUTReadyForDirectFill = PCSpawner.IsValid() && PCSpawner->GetGradientLUT().Num() > 0;
            const bool bUseDirectLidarFill = lay.point_count > 0 &&
                (lay.has_colors || !lay.has_widths || bLUTReadyForDirectFill);

            if (bUseDirectLidarFill)
            {
                static_assert(sizeof(FLidarPointCloudPoint) == sizeof(CPointCloudLidarPoint_v1),
                    "FLidarPointCloudPoint layout no longer matches CPointCloudLidarPoint_v1");

                pc.bHasLidarPoints = true;
                pc.LidarPoints.SetNumUninitialized(pc.PointCount);

                dst.lidar_points = pc.LidarPoints.Num() > 0 ? pc.LidarPoints.GetData() : nullptr;
                dst.lidar_point_stride = static_cast<int32_t>(sizeof(FLidarPointCloudPoint));
                dst.lidar_point_version = 1u;
            }
            else
            {
                if (lay.point_count > 0) pc.Positions.SetNumUninitialized(pc.PointCount);
                if (lay.has_colors && lay.point_count > 0) pc.Colors.SetNumUninitialized(pc.PointCount);
            }

            if (lay.has_widths && lay.point_count > 0) pc.Widths.SetNumUninitialized(pc.PointCount);

            dst.positions = pc.Positions.Num() > 0 ? reinterpret_cast<double*>(pc.Positions.GetData()) : nullptr;
            dst.colors8 = pc.Colors.Num() > 0 ? reinterpret_cast<unsigned char*>(pc.Colors.GetData()) : nullptr;
            dst.widths = pc.Widths.Num() > 0 ? pc.Widths.GetData() : nullptr;
        }

        const int FillResult = FillUSDFull_C(ParseHandle, nullptr, 0, FillClouds, CloudCount);
        if (FillResult != 1)
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC: FillUSDFull_C (point clouds) failed for '%s'"), *Filename);
            FMemory::Free(FillClouds);
            FreeParsedUSD_C(ParseHandle);
            FreeUSDFullLayouts_C(MeshLayouts, MeshCount, CloudLayouts, CloudCount);
            return false;
        }

        for (int32 i = 0; i < OutPointCloudData.Num(); ++i)
        {
            const CPointCloudDataFill& dst = FillClouds[i];
            OutPointCloudData[i].BoundingBoxMin = FVector(dst.bounding_box_min[0], dst.bounding_box_min[2], -dst.bounding_box_min[1]);
            OutPointCloudData[i].BoundingBoxMax = FVector(dst.bounding_box_max[0], dst.bounding_box_max[2], -dst.bounding_box_max[1]);
        }
        FMemory::Free(FillClouds);
    }

    FreeParsedUSD_C(ParseHandle);
    FreeUSDFullLayouts_C(MeshLayouts, MeshCount, CloudLayouts, CloudCount);

    UE_LOG(LogTemp, Log, TEXT("JUSYNC: LoadUSDFullFromBufferNoCopy: %d meshes + %d point clouds from '%s' (in-situ fill)"),
           OutMeshData.Num(), OutPointCloudData.Num(), *Filename);
    return true;
#else
    UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadUSDFullFromBufferNoCopy called but middleware not available"));
    return false;
#endif
}

#ifdef WITH_ANARI_USD_MIDDLEWARE
static bool JUSYNCParseRawGeoBuffer(const TArray<uint8>& Buffer, FJUSYNCPointCloudRef& OutCloud)
{
    using namespace anari_usd_protocol;

    if (Buffer.Num() < (int32)sizeof(JusyncRawGeoHeader))
        return false;

    const uint8* Src = Buffer.GetData();
    JusyncRawGeoHeader Hdr;
    FMemory::Memcpy(&Hdr, Src, sizeof(Hdr));
    if (!Hdr.valid((size_t)Buffer.Num()))
        return false;
    if (Hdr.num_points == 0 || Hdr.num_points > (uint64)MAX_int32 || Hdr.num_attributes > 16)
        return false;

    const int64 N = (int64)Hdr.num_points;
    const int64 AttrTableBytes = (int64)Hdr.num_attributes * (int64)sizeof(JusyncRawGeoAttrDesc);
    const JusyncRawGeoAttrDesc* Descs = reinterpret_cast<const JusyncRawGeoAttrDesc*>(Src + sizeof(Hdr));

    uint64 Expected = sizeof(Hdr) + (uint64)AttrTableBytes + (uint64)N * 3 * sizeof(float);
    if (Hdr.color_components)
        Expected += (uint64)N * Hdr.color_components * sizeof(float);
    for (uint32 a = 0; a < Hdr.num_attributes; ++a)
    {
        if (Descs[a].components == 0 || Descs[a].components > 4)
            return false;
        Expected += (uint64)Hdr.attr_uniform_count * Descs[a].components * sizeof(float);
    }
    if ((uint64)Buffer.Num() < Expected)
        return false;

    const float* PosF = reinterpret_cast<const float*>(Src + sizeof(Hdr) + AttrTableBytes);
    const float* ColF = nullptr;
    const float* AttrF = nullptr;
    if (Hdr.color_components)
    {
        ColF = PosF + N * 3;
        AttrF = ColF + N * Hdr.color_components;
    }
    else
    {
        AttrF = PosF + N * 3;
    }

    const float* WidthF = nullptr;
    int32 WidthComps = 0;
    int64 WidthCount = 0;
    {
        const int64 Count = FMath::Min<int64>((int64)Hdr.attr_uniform_count, N);
        const float* Section = AttrF;
        for (uint32 a = 0; a < Hdr.num_attributes; ++a)
        {
            if (strcmp(Descs[a].name, "attribute0") == 0)
            {
                WidthF = Section;
                WidthComps = (int32)Descs[a].components;
                WidthCount = Count;
                break;
            }
            Section += Count * Descs[a].components;
        }
        if (!WidthF && Hdr.num_attributes > 0 &&
            Descs[0].components >= 1 && Descs[0].components <= 4)
        {
            WidthF = AttrF;
            WidthComps = (int32)Descs[0].components;
            WidthCount = Count;
        }
    }

    auto PC = MakeShared<FJUSYNCPointCloudData, ESPMode::ThreadSafe>();
    PC->ElementName = UTF8_TO_TCHAR(Hdr.prim_name);
    PC->TypeName = TEXT("GeomPoints");
    PC->PointCount = (int32)N;
    PC->bHasColors = Hdr.color_components != 0;
    PC->bHasNormals = false;

    static_assert(sizeof(FLidarPointCloudPoint) == sizeof(CPointCloudLidarPoint_v1),
        "FLidarPointCloudPoint layout no longer matches CPointCloudLidarPoint_v1");
    PC->bHasLidarPoints = true;
    PC->LidarPoints.SetNumUninitialized(N);

    if (WidthF && WidthCount > 0)
        PC->Widths.SetNumUninitialized((int32)WidthCount);

    struct FChunkBox { float Min[3]; float Max[3]; };
    const int64 Per = FMath::DivideAndRoundUp<int64>(N, 64);
    const int32 NumChunks = (int32)FMath::Clamp<int64>(
        (N + Per - 1) / Per, 1, (N > 0) ? FMath::Min<int64>(64, N) : 1);
    const int64 ChunkStride = FMath::DivideAndRoundUp<int64>(N, NumChunks);
    TArray<FChunkBox> Boxes;
    Boxes.SetNumUninitialized(NumChunks);

    ParallelFor(NumChunks, [&](int32 c)
    {
        const int64 Begin = (int64)c * ChunkStride;
        const int64 End = FMath::Min<int64>(N, Begin + ChunkStride);
        float MinC[3] = { TNumericLimits<float>::Max(), TNumericLimits<float>::Max(), TNumericLimits<float>::Max() };
        float MaxC[3] = { TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest() };

        for (int64 i = Begin; i < End; ++i)
        {
            const float x = PosF[i * 3 + 0];
            const float y = PosF[i * 3 + 1];
            const float z = PosF[i * 3 + 2];

            MinC[0] = FMath::Min(MinC[0], x); MaxC[0] = FMath::Max(MaxC[0], x);
            MinC[1] = FMath::Min(MinC[1], y); MaxC[1] = FMath::Max(MaxC[1], y);
            MinC[2] = FMath::Min(MinC[2], z); MaxC[2] = FMath::Max(MaxC[2], z);

            float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
            if (ColF)
            {
                const float* cc = ColF + i * Hdr.color_components;
                r = cc[0]; g = cc[1]; b = cc[2];
                if (Hdr.color_components == 4) a = cc[3];
            }

            PC->LidarPoints[i] = FLidarPointCloudPoint(
                (double)x, (double)z, (double)(-y), r, g, b, a);

            if (WidthF && i < WidthCount)
                PC->Widths[(int32)i] = WidthF[i * WidthComps];
        }

        for (int32 k = 0; k < 3; ++k)
        {
            Boxes[c].Min[k] = MinC[k];
            Boxes[c].Max[k] = MaxC[k];
        }
    });

    float MinU[3] = { 0.f, 0.f, 0.f };
    float MaxU[3] = { 0.f, 0.f, 0.f };
    for (const FChunkBox& B : Boxes)
    {
        for (int32 k = 0; k < 3; ++k)
        {
            MinU[k] = FMath::Min(MinU[k], B.Min[k]);
            MaxU[k] = FMath::Max(MaxU[k], B.Max[k]);
        }
    }

    PC->BoundingBoxMin = FVector(MinU[0], MinU[2], -MinU[1]);
    PC->BoundingBoxMax = FVector(MaxU[0], MaxU[2], -MaxU[1]);

    OutCloud = MoveTemp(PC);
    return true;
}
#endif // WITH_ANARI_USD_MIDDLEWARE

// Streamed USD scenes (ParaView/VTK export) are right-handed Y-up. Map compact mesh
// vertices/normals with the same (x,z,-y) rotation the point-cloud path applies.
static TAutoConsoleVariable<int32> CVarJusyncMeshConvertYUp(
    TEXT("jusync.Mesh.ConvertUSDYUp"),
    1,
    TEXT("Compact streamed meshes: map USD (x,y,z) to UE (x,z,-y) like the point clouds. 0 = raw coordinates (legacy behavior)."),
    ECVF_Default);

// (x,z,-y) is a pure rotation, so triangle winding is preserved. If faces still render
// inside-out (culled/wrong-lit), flip this to swap the 2nd/3rd index of every triangle.
static TAutoConsoleVariable<int32> CVarJusyncMeshFlipWinding(
    TEXT("jusync.Mesh.FlipWinding"),
    0,
    TEXT("Swap the last two triangle indices on compact streamed meshes (backface fix knob, no rebuild needed)."),
    ECVF_Default);

bool UJUSYNCSubsystem::LoadUSDFullCompactFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCCompactMeshRef>& OutMeshes, TArray<FJUSYNCPointCloudRef>& OutPointCloudData)
{
#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC: LoadUSDFullCompactFromBuffer called but middleware is not initialized"));
        return false;
    }

    OutMeshes.Empty();
    OutPointCloudData.Empty();

    if (Buffer.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadUSDFullCompactFromBuffer called with empty buffer"));
        return false;
    }

    {
        uint32 MaybeMagic = 0;
        FMemory::Memcpy(&MaybeMagic, Buffer.GetData(), sizeof(MaybeMagic));
        if (MaybeMagic == anari_usd_protocol::JUSYNC_RAWGEO_MAGIC)
        {
            FJUSYNCPointCloudRef Cloud;
            if (JUSYNCParseRawGeoBuffer(Buffer, Cloud))
            {
                OutPointCloudData.Add(MoveTemp(Cloud));
                UE_LOG(LogTemp, Log, TEXT("JUSYNC: RawGeo payload: 1 point cloud (%d points) from '%s'"),
                    OutPointCloudData[0]->PointCount, *Filename);
                return true;
            }
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC: RawGeo magic but malformed payload for '%s' (%d bytes)"),
                *Filename, Buffer.Num());
            return false;
        }
    }

    FTCHARToUTF8 FilenameConverter(*Filename);
    const char* FilenameCStr = FilenameConverter.Get();

    void* ParseHandle = nullptr;
    CMeshLayout* MeshLayouts = nullptr;
    size_t MeshCount = 0;
    CPointCloudLayout* CloudLayouts = nullptr;
    size_t CloudCount = 0;

    int Result = 0;
    {
        FScopeLock Lock(&ParseMutex);
        Result = QueryUSDFullLayout_C(
            Buffer.GetData(), Buffer.Num(), FilenameCStr,
            &ParseHandle, &MeshLayouts, &MeshCount,
            &CloudLayouts, &CloudCount
        );
    }

    if (Result != 1 || !ParseHandle)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadUSDFullCompactFromBuffer failed for '%s' (buffer=%d bytes, C_result=%d MeshCount=%llu CloudCount=%llu)"),
            *Filename, Buffer.Num(), Result, (uint64)MeshCount, (uint64)CloudCount);
        if (ParseHandle) FreeParsedUSD_C(ParseHandle);
        FreeUSDFullLayouts_C(MeshLayouts, MeshCount, CloudLayouts, CloudCount);
        return false;
    }

    if (MeshCount > 0)
    {
        OutMeshes.SetNum(static_cast<int32>(MeshCount));
        CMeshDataFillCompact* FillMeshes = reinterpret_cast<CMeshDataFillCompact*>(FMemory::Malloc(MeshCount * sizeof(CMeshDataFillCompact)));
        FMemory::Memzero(FillMeshes, MeshCount * sizeof(CMeshDataFillCompact));

        for (size_t i = 0; i < MeshCount; ++i)
        {
            const CMeshLayout& lay = MeshLayouts[i];
            OutMeshes[i] = MakeShared<FJUSYNCCompactMeshData, ESPMode::ThreadSafe>();
            FJUSYNCCompactMeshData& Mesh = *OutMeshes[i];
            CMeshDataFillCompact& dst = FillMeshes[i];

            Mesh.ElementName = UTF8_TO_TCHAR(lay.element_name);
            Mesh.TypeName = UTF8_TO_TCHAR(lay.type_name);

            if (lay.points_count > 0) Mesh.Points.SetNumUninitialized(static_cast<int32>(lay.points_count));
            if (lay.indices_count > 0) Mesh.Triangles.SetNumUninitialized(static_cast<int32>(lay.indices_count));
            if (lay.normals_count > 0) Mesh.Normals.SetNumUninitialized(static_cast<int32>(lay.normals_count));
            if (lay.uvs_count > 0) Mesh.UVs.SetNumUninitialized(static_cast<int32>(lay.uvs_count));
            if (lay.vertex_colors_count > 0) Mesh.VertexColors.SetNumUninitialized(static_cast<int32>(lay.vertex_colors_count));

            dst.points = Mesh.Points.Num() > 0 ? reinterpret_cast<float*>(Mesh.Points.GetData()) : nullptr;
            dst.indices = Mesh.Triangles.Num() > 0 ? Mesh.Triangles.GetData() : nullptr;
            dst.normals = Mesh.Normals.Num() > 0 ? reinterpret_cast<float*>(Mesh.Normals.GetData()) : nullptr;
            dst.uvs = Mesh.UVs.Num() > 0 ? reinterpret_cast<float*>(Mesh.UVs.GetData()) : nullptr;
            dst.vertex_colors8 = Mesh.VertexColors.Num() > 0 ? reinterpret_cast<unsigned char*>(Mesh.VertexColors.GetData()) : nullptr;
        }

        const int FillResult = FillUSDFullCompact_C(ParseHandle, FillMeshes, MeshCount, nullptr, 0);
        FMemory::Free(FillMeshes);
        if (FillResult != 1)
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC: FillUSDFullCompact_C (meshes) failed for '%s'"), *Filename);
            FreeParsedUSD_C(ParseHandle);
            FreeUSDFullLayouts_C(MeshLayouts, MeshCount, CloudLayouts, CloudCount);
            return false;
        }

        for (int32 i = 0; i < OutMeshes.Num(); ++i)
        {
            FJUSYNCCompactMeshData& Mesh = *OutMeshes[i];

            // USD/ParaView scenes are right-handed Y-up; the point-cloud path maps
            // (x, y, z) -> (x, z, -y) for UE (see cpc.positions transform above).
            // Compact meshes used to flow through raw and rendered 90 deg off the clouds.
            if (CVarJusyncMeshConvertYUp.GetValueOnAnyThread() != 0)
            {
                const bool bFlipWinding = CVarJusyncMeshFlipWinding.GetValueOnAnyThread() != 0;
                for (FVector3f& P : Mesh.Points)
                {
                    P = FVector3f(P.X, P.Z, -P.Y);
                }
                for (FVector3f& N : Mesh.Normals)
                {
                    N = FVector3f(N.X, N.Z, -N.Y);
                }
                if (bFlipWinding)
                {
                    for (int32 t = 0; t + 2 < Mesh.Triangles.Num(); t += 3)
                    {
                        Mesh.Triangles.Swap(t + 1, t + 2);
                    }
                }
                static TSet<FString> LoggedMeshes;
                if (!LoggedMeshes.Contains(Mesh.ElementName))
                {
                    LoggedMeshes.Add(Mesh.ElementName);
                    UE_LOG(LogJUSYNC, Log, TEXT("Compact mesh '%s': mapped USD Y-up -> UE (x,z,-y), winding=%s"),
                        *Mesh.ElementName, bFlipWinding ? TEXT("flipped") : TEXT("kept"));
                }
            }

            for (int32 j = 0; j < Mesh.Normals.Num(); ++j)
            {
                Mesh.Normals[j] = Mesh.Normals[j].GetSafeNormal();
                if (Mesh.Normals[j].IsNearlyZero())
                {
                    Mesh.Normals[j] = FVector3f(0.0f, 0.0f, 1.0f);
                }
            }
            Mesh.bHasBakedColors = Mesh.VertexColors.Num() > 0;
        }
    }

    if (CloudCount > 0)
    {
        OutPointCloudData.SetNum(static_cast<int32>(CloudCount));
        CPointCloudDataFill* FillClouds = reinterpret_cast<CPointCloudDataFill*>(FMemory::Malloc(CloudCount * sizeof(CPointCloudDataFill)));
        FMemory::Memzero(FillClouds, CloudCount * sizeof(CPointCloudDataFill));

        for (size_t i = 0; i < CloudCount; ++i)
        {
            const CPointCloudLayout& lay = CloudLayouts[i];
            OutPointCloudData[i] = MakeShared<FJUSYNCPointCloudData, ESPMode::ThreadSafe>();
            FJUSYNCPointCloudData& pc = *OutPointCloudData[i];
            CPointCloudDataFill& dst = FillClouds[i];

            pc.ElementName = UTF8_TO_TCHAR(lay.element_name);
            pc.TypeName = TEXT("GeomPoints");
            pc.PointCount = static_cast<int32>(lay.point_count);
            pc.bHasColors = lay.has_colors != 0;
            pc.bHasNormals = lay.has_normals != 0;

            const bool bLUTReadyForDirectFill = PCSpawner.IsValid() && PCSpawner->GetGradientLUT().Num() > 0;
            const bool bUseDirectLidarFill = lay.point_count > 0 &&
                (lay.has_colors || !lay.has_widths || bLUTReadyForDirectFill);

            if (bUseDirectLidarFill)
            {
                static_assert(sizeof(FLidarPointCloudPoint) == sizeof(CPointCloudLidarPoint_v1),
                    "FLidarPointCloudPoint layout no longer matches CPointCloudLidarPoint_v1");

                pc.bHasLidarPoints = true;
                pc.LidarPoints.SetNumUninitialized(pc.PointCount);

                dst.lidar_points = pc.LidarPoints.Num() > 0 ? pc.LidarPoints.GetData() : nullptr;
                dst.lidar_point_stride = static_cast<int32_t>(sizeof(FLidarPointCloudPoint));
                dst.lidar_point_version = 1u;
            }
            else
            {
                if (lay.point_count > 0) pc.Positions.SetNumUninitialized(pc.PointCount);
                if (lay.has_colors && lay.point_count > 0) pc.Colors.SetNumUninitialized(pc.PointCount);
            }

            if (lay.has_widths && lay.point_count > 0) pc.Widths.SetNumUninitialized(pc.PointCount);

            dst.positions = pc.Positions.Num() > 0 ? reinterpret_cast<double*>(pc.Positions.GetData()) : nullptr;
            dst.colors8 = pc.Colors.Num() > 0 ? reinterpret_cast<unsigned char*>(pc.Colors.GetData()) : nullptr;
            dst.widths = pc.Widths.Num() > 0 ? pc.Widths.GetData() : nullptr;
        }

        const int FillResult = FillUSDFullCompact_C(ParseHandle, nullptr, 0, FillClouds, CloudCount);
        if (FillResult != 1)
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC: FillUSDFullCompact_C (point clouds) failed for '%s'"), *Filename);
            FMemory::Free(FillClouds);
            FreeParsedUSD_C(ParseHandle);
            FreeUSDFullLayouts_C(MeshLayouts, MeshCount, CloudLayouts, CloudCount);
            return false;
        }

        for (int32 i = 0; i < OutPointCloudData.Num(); ++i)
        {
            const CPointCloudDataFill& dst = FillClouds[i];
            OutPointCloudData[i]->BoundingBoxMin = FVector(dst.bounding_box_min[0], dst.bounding_box_min[2], -dst.bounding_box_min[1]);
            OutPointCloudData[i]->BoundingBoxMax = FVector(dst.bounding_box_max[0], dst.bounding_box_max[2], -dst.bounding_box_max[1]);
        }
        FMemory::Free(FillClouds);
    }

    FreeParsedUSD_C(ParseHandle);
    FreeUSDFullLayouts_C(MeshLayouts, MeshCount, CloudLayouts, CloudCount);

    UE_LOG(LogTemp, Log, TEXT("JUSYNC: LoadUSDFullCompactFromBuffer: %d compact meshes + %d point clouds from '%s'"),
        OutMeshes.Num(), OutPointCloudData.Num(), *Filename);
    return true;
#else
    UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadUSDFullCompactFromBuffer called but middleware not available"));
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

    CTextureData CTexture = CreateTextureFromBuffer_C(Buffer.GetData(), Buffer.Num());

    if (CTexture.data && CTexture.data_size > 0)
    {
        Result.Width = CTexture.width;
        Result.Height = CTexture.height;
        Result.Channels = CTexture.channels;
        Result.Data.SetNum(CTexture.data_size);
        FMemory::Memcpy(Result.Data.GetData(), CTexture.data, CTexture.data_size);

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

    FTCHARToUTF8 OutputPathConverter(*OutputPath);
    const char* OutputPathCStr = OutputPathConverter.Get();

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

    int Result = GetGradientLineAsPNGBuffer_C(Buffer.GetData(), Buffer.Num(), &PNGData, &PNGSize);

    if (Result == 1 && PNGData && PNGSize > 0)
    {
        OutPNGBuffer.SetNum(PNGSize);
        FMemory::Memcpy(OutPNGBuffer.GetData(), PNGData, PNGSize);

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

void UJUSYNCSubsystem::EnableSpawnedMeshCollision(URealtimeMeshComponent* RealtimeMeshComponent, URealtimeMeshSimple* RealtimeMesh) const
{
    if (!bEnableSpawnedMeshCollision || !RealtimeMeshComponent || !RealtimeMesh)
    {
        return;
    }

    FRealtimeMeshCollisionConfiguration CollisionConfig = RealtimeMesh->GetCollisionConfig();
    CollisionConfig.bUseComplexAsSimpleCollision = true;
    RealtimeMesh->SetCollisionConfig(CollisionConfig);

    RealtimeMeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    RealtimeMeshComponent->SetCollisionObjectType(ECC_WorldDynamic);
    RealtimeMeshComponent->SetCollisionResponseToAllChannels(ECR_Block);
    RealtimeMeshComponent->SetGenerateOverlapEvents(false);
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
        UE_LOG(LogJUSYNC, Verbose, TEXT("JUSYNC: No cached gradient texture in middleware (expected if no PC parsed yet)"));
        return;
    }

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

    int Result = GetImageRowAsPNGBuffer_C(Buffer.GetData(), Buffer.Num(), RowIndex, &out_buffer, &out_size);

    if (Result == 1 && out_buffer && out_size > 0)
    {
        OutPNGBuffer.SetNum(out_size);
        FMemory::Memcpy(OutPNGBuffer.GetData(), out_buffer, out_size);

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

    MeshData.Normals.SetNum(MeshData.Vertices.Num());
    for (int32 i = 0; i < MeshData.Normals.Num(); ++i)
    {
        MeshData.Normals[i] = FVector::ZeroVector;
    }

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

            FVector FaceNormal = FVector::CrossProduct(v2 - v0, v1 - v0).GetSafeNormal();

            MeshData.Normals[i0] += FaceNormal;
            MeshData.Normals[i1] += FaceNormal;
            MeshData.Normals[i2] += FaceNormal;
        }
    }

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

bool UJUSYNCSubsystem::UpdateRealtimeMeshFromJUSYNC(
    const FJUSYNCMeshData& MeshData,
    URealtimeMeshComponent* RealtimeMeshComponent,
    UMaterialInterface* MaterialToApply,
    RealtimeMesh::FRealtimeMeshStreamSet* PrebuiltStreams)
{
    if (!RealtimeMeshComponent || !RealtimeMeshComponent->IsValidLowLevel())
    {
        return false;
    }
    if (!MeshData.IsValid())
    {
        return false;
    }

    const double UpdateStart = FPlatformTime::Seconds();
    RealtimeMesh::FRealtimeMeshStreamSet Streams;
    if (PrebuiltStreams)
    {
        Streams = MoveTemp(*PrebuiltStreams);
    }
    else
    {
        const double BuildStart = FPlatformTime::Seconds();
        if (!JUSYNCBuildRealtimeMeshStreams(MeshData, Streams))
        {
            return false;
        }
        UE_LOG(LogJUSYNC, Verbose, TEXT("JUSYNC PERF BuildStreams '%s' %.3f ms"),
               *MeshData.ElementName, (FPlatformTime::Seconds() - BuildStart) * 1000.0);
    }

    URealtimeMeshSimple* RealtimeMesh = Cast<URealtimeMeshSimple>(RealtimeMeshComponent->GetRealtimeMesh());
    if (!RealtimeMesh)
    {
        RealtimeMesh = RealtimeMeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
    }
    if (!RealtimeMesh)
    {
        return false;
    }

    RealtimeMesh->SetupMaterialSlot(0, TEXT("PrimaryMaterial"));

    UMaterialInterface* ActiveMaterial = MaterialToApply ? MaterialToApply : RealtimeMeshComponent->GetMaterial(0);
    if (!ActiveMaterial)
    {
        ActiveMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"));
        if (!ActiveMaterial)
        {
            UMaterialInterface* DefaultMat = GetCachedMaterial(TEXT("/Engine/EngineMaterials/DefaultMaterial"));
            if (DefaultMat)
            {
                auto* DynMat = UMaterialInstanceDynamic::Create(DefaultMat, RealtimeMeshComponent);
                if (DynMat)
                {
                    DynMat->SetScalarParameterValue(TEXT("UseVertexColor"), 1.0f);
                    ActiveMaterial = DynMat;
                }
            }
        }
    }
    const FRealtimeMeshSectionGroupKey GroupKey = FRealtimeMeshSectionGroupKey::Create(0, TEXT("USDGroup"));
    const FRealtimeMeshSectionKey SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);

    if (RealtimeMesh->GetSectionGroup(GroupKey))
    {
        RealtimeMesh->UpdateSectionGroup(GroupKey, MoveTemp(Streams));
    }
    else
    {
        RealtimeMesh->CreateSectionGroup(GroupKey, MoveTemp(Streams));
    }

    FRealtimeMeshSectionConfig SectionConfig(0);
    SectionConfig.bIsVisible = true;
    SectionConfig.bCastsShadow = true;
    RealtimeMesh->UpdateSectionConfig(SectionKey, SectionConfig, bEnableSpawnedMeshCollision);
    EnableSpawnedMeshCollision(RealtimeMeshComponent, RealtimeMesh);

    if (ActiveMaterial)
    {
        RealtimeMeshComponent->SetMaterial(0, ActiveMaterial);
    }

    UE_LOG(LogJUSYNC, Verbose, TEXT("JUSYNC PERF UpdateRealtimeMesh '%s' %.3f ms"),
           *MeshData.ElementName, (FPlatformTime::Seconds() - UpdateStart) * 1000.0);
    return true;
}

static TAutoConsoleVariable<FString> CVarJUSyncMorphMeshMaterial(
    TEXT("jusync.Morph.MeshMaterial"),
    TEXT("/Game/Materials/M_JusyncMorph.M_JusyncMorph"),
    TEXT("Morph-capable material substituted for streamed mesh updates when the material about to be")
    TEXT(" applied lacks the JusyncMorphT WPO branch. Empty = never override (mesh morphs stay off")
    TEXT(" unless the spawn material itself is morph-capable)."),
    ECVF_Default);

// One morph-MID per streamed component so concurrent meshes ramp independently;
// weak on both ends so the cache never keeps a mesh or material alive.
static TMap<TWeakObjectPtr<URealtimeMeshComponent>, TWeakObjectPtr<UMaterialInstanceDynamic>>
    GJUSyncMorphOverrideMIDs;

static UMaterialInstanceDynamic* GetOrCreateMorphOverrideMID(UJUSYNCSubsystem* Subsystem,
                                                             URealtimeMeshComponent* Comp)
{
    const FString Path = CVarJUSyncMorphMeshMaterial.GetValueOnGameThread();
    if (Path.IsEmpty())
    {
        return nullptr;
    }

    if (GJUSyncMorphOverrideMIDs.Num() > 256)
    {
        for (auto It = GJUSyncMorphOverrideMIDs.CreateIterator(); It; ++It)
        {
            if (!It.Key().IsValid())
            {
                It.RemoveCurrent();
            }
        }
    }

    if (const TWeakObjectPtr<UMaterialInstanceDynamic>* Cached = GJUSyncMorphOverrideMIDs.Find(Comp))
    {
        if (UMaterialInstanceDynamic* MID = Cached->Get())
        {
            return MID;
        }
    }

    UMaterialInterface* MorphBase = Subsystem->GetCachedMaterial(Path);
    if (!MorphBase || !FJUSYNCMeshMorpher::IsMaterialMorphCapable(MorphBase))
    {
        GJUSyncMorphOverrideMIDs.Remove(Comp);
        static FString WarnedPath;
        if (WarnedPath != Path)
        {
            WarnedPath = Path;
            UE_LOG(LogJUSYNC, Warning,
                   TEXT("jusync.Morph.MeshMaterial '%s' not found or missing the JusyncMorphT parameter;")
                   TEXT(" create it with Tools/CreateJusyncMorphMaterial.py — mesh morphing stays off"),
                   *Path);
        }
        return nullptr;
    }

    UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(MorphBase, Comp);
    if (MID)
    {
        MID->SetScalarParameterValue(FJUSYNCMeshMorpher::MorphActiveParameter, 0.f);
        MID->SetScalarParameterValue(FJUSYNCMeshMorpher::MorphTParameter, 1.f);
        GJUSyncMorphOverrideMIDs.Add(Comp, MID);
    }
    return MID;
}

bool UJUSYNCSubsystem::UpdateRealtimeMeshFromJUSYNC_WithCompact(
    const FJUSYNCCompactMeshData& MeshData,
    URealtimeMeshComponent* RealtimeMeshComponent,
    UMaterialInterface* MaterialToApply,
    RealtimeMesh::FRealtimeMeshStreamSet* PrebuiltStreams)
{
    if (!RealtimeMeshComponent || !RealtimeMeshComponent->IsValidLowLevel())
    {
        return false;
    }
    if (!MeshData.IsValid())
    {
        return false;
    }

    const double UpdateStart = FPlatformTime::Seconds();

    bool bMorphBlend = false;
    TArray<FVector3f> MorphPrev;
    UMaterialInterface* EffectiveMaterial = MaterialToApply;
    if (MeshMorpher.IsValid())
    {
        UMaterialInterface* IntentMaterial = MaterialToApply
            ? MaterialToApply
            : static_cast<UMaterialInterface*>(RealtimeMeshComponent->GetMaterial(0));
        if (!FJUSYNCMeshMorpher::IsMaterialMorphCapable(IntentMaterial))
        {
            if (UMaterialInstanceDynamic* MorphMID = GetOrCreateMorphOverrideMID(this, RealtimeMeshComponent))
            {
                IntentMaterial = MorphMID;
                EffectiveMaterial = MorphMID;
            }
        }
        bMorphBlend = (MeshMorpher->BeginUpdate(RealtimeMeshComponent, IntentMaterial,
                                                MeshData, UpdateStart, MorphPrev)
                       == FJUSYNCMeshMorpher::EApplyMode::Blend);
    }

    RealtimeMesh::FRealtimeMeshStreamSet Streams;
    bool bStreamsBuilt = false;
    if (bMorphBlend)
    {
        bStreamsBuilt = JUSYNCBuildRealtimeMeshStreams(MeshData, Streams, &MorphPrev);
        if (!bStreamsBuilt)
        {
            bMorphBlend = false;
        }
    }
    if (!bStreamsBuilt)
    {
        if (PrebuiltStreams)
        {
            Streams = MoveTemp(*PrebuiltStreams);
        }
        else if (!JUSYNCBuildRealtimeMeshStreams(MeshData, Streams))
        {
            return false;
        }
    }

    URealtimeMeshSimple* RealtimeMesh = Cast<URealtimeMeshSimple>(RealtimeMeshComponent->GetRealtimeMesh());
    if (!RealtimeMesh)
    {
        RealtimeMesh = RealtimeMeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
    }
    if (!RealtimeMesh)
    {
        return false;
    }

    RealtimeMesh->SetupMaterialSlot(0, TEXT("PrimaryMaterial"));

    UMaterialInterface* ActiveMaterial = EffectiveMaterial ? EffectiveMaterial : RealtimeMeshComponent->GetMaterial(0);
    if (!ActiveMaterial)
    {
        ActiveMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"));
        if (!ActiveMaterial)
        {
            UMaterialInterface* DefaultMat = GetCachedMaterial(TEXT("/Engine/EngineMaterials/DefaultMaterial"));
            if (DefaultMat)
            {
                auto* DynMat = UMaterialInstanceDynamic::Create(DefaultMat, RealtimeMeshComponent);
                if (DynMat)
                {
                    DynMat->SetScalarParameterValue(TEXT("UseVertexColor"), 1.0f);
                    ActiveMaterial = DynMat;
                }
            }
        }
    }

    const FRealtimeMeshSectionGroupKey GroupKey = FRealtimeMeshSectionGroupKey::Create(0, TEXT("USDGroup"));
    const FRealtimeMeshSectionKey SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);

    if (RealtimeMesh->GetSectionGroup(GroupKey))
    {
        RealtimeMesh->UpdateSectionGroup(GroupKey, MoveTemp(Streams));
    }
    else
    {
        RealtimeMesh->CreateSectionGroup(GroupKey, MoveTemp(Streams));
    }

    FRealtimeMeshSectionConfig SectionConfig(0);
    SectionConfig.bIsVisible = true;
    SectionConfig.bCastsShadow = true;
    RealtimeMesh->UpdateSectionConfig(SectionKey, SectionConfig, bEnableSpawnedMeshCollision);
    EnableSpawnedMeshCollision(RealtimeMeshComponent, RealtimeMesh);

    if (ActiveMaterial)
    {
        RealtimeMeshComponent->SetMaterial(0, ActiveMaterial);
    }

    if (MeshMorpher.IsValid())
    {
        MeshMorpher->Commit(RealtimeMeshComponent, MeshData, FPlatformTime::Seconds(),
                            bMorphBlend, MoveTemp(MorphPrev));
    }

    UE_LOG(LogJUSYNC, Verbose, TEXT("JUSYNC PERF UpdateRealtimeMeshCompact '%s' %.3f ms"),
        *MeshData.ElementName, (FPlatformTime::Seconds() - UpdateStart) * 1000.0);
    return true;
}

bool UJUSYNCSubsystem::CreateRealtimeMeshFromJUSYNC(
    const FJUSYNCMeshData& InMeshData,
    URealtimeMeshComponent* RealtimeMeshComponent)
{
    return CreateRealtimeMeshFromJUSYNC_WithStreams(InMeshData, RealtimeMeshComponent, nullptr);
}

bool UJUSYNCSubsystem::CreateRealtimeMeshFromJUSYNC_WithStreams(
    const FJUSYNCMeshData& InMeshData,
    URealtimeMeshComponent* RealtimeMeshComponent,
    RealtimeMesh::FRealtimeMeshStreamSet* PrebuiltStreams)
{
    if (!RealtimeMeshComponent || !RealtimeMeshComponent->IsValidLowLevel())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid or destroyed RealtimeMeshComponent"));
        return false;
    }

    if (!InMeshData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid mesh data"));
        return false;
    }

    const FJUSYNCMeshData& MeshData = InMeshData;

    if (MeshData.Vertices.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Mesh has no vertices"));
        return false;
    }

    if (MeshData.Triangles.Num() < 3 || (MeshData.Triangles.Num() % 3) != 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid triangle count: %d (must be multiple of 3)"), MeshData.Triangles.Num());
        return false;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Mesh: %d vertices, %d triangles, %d colors"),
           MeshData.Vertices.Num(), MeshData.Triangles.Num() / 3, MeshData.VertexColors.Num());

    const int32 FinalVertexCount = MeshData.Vertices.Num();
    const int32 FinalTriCount = MeshData.Triangles.Num() / 3;

    if (FinalVertexCount > 1000000)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("Very large mesh: %d vertices (consider splitting)"), FinalVertexCount);
    }

    URealtimeMeshSimple* RealtimeMesh = RealtimeMeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
    if (!RealtimeMesh)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to initialize RealtimeMesh"));
        return false;
    }

    RealtimeMesh->SetupMaterialSlot(0, TEXT("PrimaryMaterial"));

    UMaterialInterface* ExistingMaterial = RealtimeMeshComponent->GetMaterial(0);

    ExistingMaterial = JUSYNCResolveMaterialForColoredMesh(this, ExistingMaterial, MeshData.VertexColors,
        RealtimeMeshComponent, TEXT("CreateRealtimeMeshFromJUSYNC"));

    if (!ExistingMaterial)
    {
        UMaterialInterface* VertexColorMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"));
        if (VertexColorMaterial)
        {
            RealtimeMeshComponent->SetMaterial(0, VertexColorMaterial);
            UE_LOG(LogJUSYNC, Log, TEXT("Applied cached M_VertexColor material as fallback"));
        }
        else
        {
            UMaterialInterface* DefaultMat = GetCachedMaterial(TEXT("/Engine/EngineMaterials/DefaultMaterial"));
            if (DefaultMat)
            {
                auto* DynMat = UMaterialInstanceDynamic::Create(DefaultMat, RealtimeMeshComponent);
                DynMat->SetScalarParameterValue(TEXT("UseVertexColor"), 1.0f);
                RealtimeMeshComponent->SetMaterial(0, DynMat);
                UE_LOG(LogJUSYNC, Log, TEXT("Applied enhanced default material as fallback"));
            }
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Using provided material (preserving texture material from Blueprint)"));
    }


    RealtimeMesh::FRealtimeMeshStreamSet Streams;
    if (PrebuiltStreams)
    {
        Streams = MoveTemp(*PrebuiltStreams);
    }
    else if (!JUSYNCBuildRealtimeMeshStreams(MeshData, Streams))
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to build RealtimeMesh streams for '%s'"), *MeshData.ElementName);
        return false;
    }

    const FRealtimeMeshSectionGroupKey GroupKey = FRealtimeMeshSectionGroupKey::Create(0, TEXT("USDGroup"));
    const FRealtimeMeshSectionKey SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);
    RealtimeMesh->CreateSectionGroup(GroupKey, MoveTemp(Streams));
    FRealtimeMeshSectionConfig SectionConfig(0);
    SectionConfig.bIsVisible = true;
    SectionConfig.bCastsShadow = true;
    RealtimeMesh->UpdateSectionConfig(SectionKey, SectionConfig, bEnableSpawnedMeshCollision);
    EnableSpawnedMeshCollision(RealtimeMeshComponent, RealtimeMesh);

    if (ExistingMaterial)
    {
        RealtimeMeshComponent->SetMaterial(0, ExistingMaterial);
        UE_LOG(LogJUSYNC, Log, TEXT("Reapplied existing material after sync mesh creation: %s"),
               *ExistingMaterial->GetName());
    }

    UE_LOG(LogJUSYNC, Log, TEXT("CreateRealtimeMeshFromJUSYNC: Smooth mesh created '%s' (%d verts, %d tris)"),
           *MeshData.ElementName, FinalVertexCount, FinalTriCount);

    return true;
}

bool UJUSYNCSubsystem::CreateRealtimeMeshFromJUSYNC_WithCompact(
    const FJUSYNCCompactMeshData& InMeshData,
    URealtimeMeshComponent* RealtimeMeshComponent,
    RealtimeMesh::FRealtimeMeshStreamSet* PrebuiltStreams)
{
    if (!RealtimeMeshComponent || !RealtimeMeshComponent->IsValidLowLevel())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid or destroyed RealtimeMeshComponent"));
        return false;
    }

    if (!InMeshData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid compact mesh data"));
        return false;
    }

    const FJUSYNCCompactMeshData& MeshData = InMeshData;

    if (MeshData.Points.Num() == 0 ||
        MeshData.Triangles.Num() < 3 ||
        (MeshData.Triangles.Num() % 3) != 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid compact mesh arrays"));
        return false;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("COMPACT REALTIME MESH CREATION: %s (%d verts, %d tris, colors=%d)"),
        *MeshData.ElementName, MeshData.Points.Num(), MeshData.Triangles.Num() / 3, MeshData.HasVertexColors() ? 1 : 0);

    URealtimeMeshSimple* RealtimeMesh = RealtimeMeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
    if (!RealtimeMesh)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to initialize RealtimeMesh"));
        return false;
    }

    RealtimeMesh->SetupMaterialSlot(0, TEXT("PrimaryMaterial"));

    UMaterialInterface* ExistingMaterial = RealtimeMeshComponent->GetMaterial(0);

    ExistingMaterial = JUSYNCResolveMaterialForColoredMesh(this, ExistingMaterial, MeshData.VertexColors,
        RealtimeMeshComponent, TEXT("CreateRealtimeMeshFromJUSYNC_WithCompact"));

    if (!ExistingMaterial)
    {
        UMaterialInterface* VertexColorMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"));
        if (VertexColorMaterial)
        {
            RealtimeMeshComponent->SetMaterial(0, VertexColorMaterial);
        }
        else
        {
            UMaterialInterface* DefaultMat = GetCachedMaterial(TEXT("/Engine/EngineMaterials/DefaultMaterial"));
            if (DefaultMat)
            {
                auto* DynMat = UMaterialInstanceDynamic::Create(DefaultMat, RealtimeMeshComponent);
                if (DynMat)
                {
                    DynMat->SetScalarParameterValue(TEXT("UseVertexColor"), 1.0f);
                    RealtimeMeshComponent->SetMaterial(0, DynMat);
                }
            }
        }
    }

    RealtimeMesh::FRealtimeMeshStreamSet Streams;
    if (PrebuiltStreams)
    {
        Streams = MoveTemp(*PrebuiltStreams);
    }
    else if (!JUSYNCBuildRealtimeMeshStreams(MeshData, Streams))
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to build compact RealtimeMesh streams for '%s'"), *MeshData.ElementName);
        return false;
    }

    const FRealtimeMeshSectionGroupKey GroupKey = FRealtimeMeshSectionGroupKey::Create(0, TEXT("USDGroup"));
    const FRealtimeMeshSectionKey SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);
    RealtimeMesh->CreateSectionGroup(GroupKey, MoveTemp(Streams));
    FRealtimeMeshSectionConfig SectionConfig(0);
    SectionConfig.bIsVisible = true;
    SectionConfig.bCastsShadow = true;
    RealtimeMesh->UpdateSectionConfig(SectionKey, SectionConfig, bEnableSpawnedMeshCollision);
    EnableSpawnedMeshCollision(RealtimeMeshComponent, RealtimeMesh);

    if (ExistingMaterial)
    {
        RealtimeMeshComponent->SetMaterial(0, ExistingMaterial);
    }

    if (MeshMorpher.IsValid())
    {
        MeshMorpher->Seed(RealtimeMeshComponent, MeshData, FPlatformTime::Seconds());
    }

    UE_LOG(LogJUSYNC, Log, TEXT("CreateRealtimeMeshFromJUSYNC_WithCompact: '%s' (%d verts, %d tris)"),
        *MeshData.ElementName, MeshData.Points.Num(), MeshData.Triangles.Num() / 3);
    return true;
}

bool UJUSYNCSubsystem::CreateRealtimeMeshFromJUSYNCWithSplitting(
    const FJUSYNCMeshData& MeshData,
    URealtimeMeshComponent* RealtimeMeshComponent,
    int32 MaxVerticesPerChunk)
{
    if (!RealtimeMeshComponent || !RealtimeMeshComponent->IsValidLowLevel())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid or destroyed RealtimeMeshComponent"));
        return false;
    }

    if (!MeshData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid mesh data"));
        return false;
    }

    const int32 TotalVertices = MeshData.Vertices.Num();
    const int32 TotalTriangles = MeshData.Triangles.Num() / 3;

    UE_LOG(LogJUSYNC, Log, TEXT("Mesh: %d vertices, %d triangles, MaxVerticesPerChunk: %d"),
           TotalVertices, TotalTriangles, MaxVerticesPerChunk);

    if (TotalVertices <= MaxVerticesPerChunk)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Mesh is small enough (%d vertices), using standard creation"), TotalVertices);
        return this->CreateRealtimeMeshFromJUSYNC(MeshData, RealtimeMeshComponent);
    }

    const int32 NumChunks = FMath::CeilToInt((float)TotalVertices / MaxVerticesPerChunk);
    UE_LOG(LogJUSYNC, Log, TEXT("Splitting mesh into %d chunks (each <= %d vertices)"), NumChunks, MaxVerticesPerChunk);

    URealtimeMeshSimple* RealtimeMesh = RealtimeMeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
    if (!RealtimeMesh)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to initialize RealtimeMesh"));
        return false;
    }

    RealtimeMesh->SetupMaterialSlot(0, TEXT("PrimaryMaterial"));

    UMaterialInterface* ProvidedMaterial = JUSYNCResolveMaterialForColoredMesh(this,
        RealtimeMeshComponent->GetMaterial(0), MeshData.VertexColors,
        RealtimeMeshComponent, TEXT("CreateRealtimeMeshFromJUSYNCWithSplitting"));
    if (ProvidedMaterial && RealtimeMeshComponent->GetMaterial(0) != ProvidedMaterial)
    {
        RealtimeMeshComponent->SetMaterial(0, ProvidedMaterial);
    }

    if (!RealtimeMeshComponent->GetMaterial(0))
    {
        UMaterialInterface* VertexColorMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"));
        if (VertexColorMaterial)
        {
            RealtimeMeshComponent->SetMaterial(0, VertexColorMaterial);
            UE_LOG(LogJUSYNC, Log, TEXT("Applied cached M_VertexColor material"));
        }
    }

    TArray<FJUSYNCMeshData> SplitMeshes = this->SplitLargeMeshForRealtimeMesh(MeshData, MaxVerticesPerChunk, true);

    if (SplitMeshes.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to split mesh"));
        return false;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Split mesh into %d chunks"), SplitMeshes.Num());

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

        for (int32 i = 0; i < VertexCount; ++i)
        {
            Builder.AddVertex(FVector3f(ChunkMesh.Vertices[i]));

            FVector3f N = FVector3f(ChunkMesh.Normals.IsValidIndex(i) ? ChunkMesh.Normals[i] : FVector::UpVector);
            Builder.SetNormal(i, N);

            if (ChunkMesh.UVs.IsValidIndex(i))
            {
                Builder.SetTexCoord(i, 0, FVector2DHalf(FVector2f(ChunkMesh.UVs[i])));
            }
            else
            {
                Builder.SetTexCoord(i, 0, FVector2DHalf(FVector2f::ZeroVector));
            }

            if (ChunkMesh.VertexColors.IsValidIndex(i))
            {
                Builder.SetColor(i, ChunkMesh.VertexColors[i]);
            }
            else
            {
                Builder.SetColor(i, FColor::White);
            }
        }

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
                UE_LOG(LogJUSYNC, Error, TEXT("Invalid triangle %d in chunk %d: [%d,%d,%d] vs %d vertices"),
                       Face, ChunkIdx, i0, i1, i2, VertexCount);
            }
        }

        RealtimeMesh->CreateSectionGroup(GroupKey, MoveTemp(Streams));

        FRealtimeMeshSectionConfig SectionConfig(0);
        SectionConfig.bIsVisible = true;
        SectionConfig.bCastsShadow = true;
        RealtimeMesh->UpdateSectionConfig(SectionKey, SectionConfig, bEnableSpawnedMeshCollision);
    EnableSpawnedMeshCollision(RealtimeMeshComponent, RealtimeMesh);

        UE_LOG(LogJUSYNC, Log, TEXT("Created chunk %d: %d vertices, %d triangles"),
               ChunkIdx, VertexCount, TriangleCount);
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Created mesh with %d sections"), SplitMeshes.Num());
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
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot split invalid mesh"));
        return SplitMeshes;
    }

    const int32 TotalVertices = LargeMesh.Vertices.Num();
    const int32 TotalTriangles = LargeMesh.Triangles.Num() / 3;

    UE_LOG(LogJUSYNC, Log, TEXT("Input: %d vertices, %d triangles"), TotalVertices, TotalTriangles);
    UE_LOG(LogJUSYNC, Log, TEXT("Max vertices per chunk: %d, Preserve connectivity: %s"),
           MaxVerticesPerChunk, bPreserveConnectivity ? TEXT("Yes") : TEXT("No"));

    double StartTime = FPlatformTime::Seconds();

    if (TotalVertices <= MaxVerticesPerChunk)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Mesh is already small enough (%d vertices), returning as single chunk"), TotalVertices);
        SplitMeshes.Add(LargeMesh);

        double EndTime = FPlatformTime::Seconds();
        float SplitTime_ms = (EndTime - StartTime) * 1000.0f;
        RecordMeshSplit(TotalVertices, TotalTriangles, 1, SplitTime_ms);

        return SplitMeshes;
    }

    FBox BoundingBox(ForceInit);
    for (const FVector& Vertex : LargeMesh.Vertices)
    {
        BoundingBox += Vertex;
    }

    FVector BoxSize = BoundingBox.GetSize();
    UE_LOG(LogJUSYNC, Log, TEXT("Bounding box: Min=%s, Max=%s, Size=%s"),
           *BoundingBox.Min.ToString(), *BoundingBox.Max.ToString(), *BoxSize.ToString());

    int32 NumChunks = FMath::CeilToInt((float)TotalVertices / MaxVerticesPerChunk);

    int32 GridCellsPerAxis = FMath::CeilToInt(FMath::Pow((float)NumChunks, 1.0f / 3.0f));
    NumChunks = GridCellsPerAxis * GridCellsPerAxis * GridCellsPerAxis;

    UE_LOG(LogJUSYNC, Log, TEXT("Splitting into %d chunks (%dx%dx%d grid)"),
           NumChunks, GridCellsPerAxis, GridCellsPerAxis, GridCellsPerAxis);

    TArray<TArray<int32>> ChunkVertices;  // Vertex indices per chunk
    TArray<TArray<int32>> ChunkTriangles; // Triangle indices per chunk
    TArray<TMap<int32, int32>> VertexRemapping; // Original -> Chunk vertex mapping

    ChunkVertices.SetNum(NumChunks);
    ChunkTriangles.SetNum(NumChunks);
    VertexRemapping.SetNum(NumChunks);

    FVector CellSize = BoxSize / GridCellsPerAxis;
    if (CellSize.X < 1.0f) CellSize.X = 1.0f;
    if (CellSize.Y < 1.0f) CellSize.Y = 1.0f;
    if (CellSize.Z < 1.0f) CellSize.Z = 1.0f;

    for (int32 VertexIdx = 0; VertexIdx < TotalVertices; ++VertexIdx)
    {
        const FVector& Vertex = LargeMesh.Vertices[VertexIdx];

        FVector RelativePos = Vertex - BoundingBox.Min;
        int32 CellX = FMath::Clamp(FMath::FloorToInt(RelativePos.X / CellSize.X), 0, GridCellsPerAxis - 1);
        int32 CellY = FMath::Clamp(FMath::FloorToInt(RelativePos.Y / CellSize.Y), 0, GridCellsPerAxis - 1);
        int32 CellZ = FMath::Clamp(FMath::FloorToInt(RelativePos.Z / CellSize.Z), 0, GridCellsPerAxis - 1);

        int32 ChunkIdx = CellX + CellY * GridCellsPerAxis + CellZ * GridCellsPerAxis * GridCellsPerAxis;

        if (ChunkIdx >= 0 && ChunkIdx < NumChunks)
        {
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

    for (int32 TriIdx = 0; TriIdx < TotalTriangles; ++TriIdx)
    {
        int32 V0 = LargeMesh.Triangles[TriIdx * 3 + 0];
        int32 V1 = LargeMesh.Triangles[TriIdx * 3 + 1];
        int32 V2 = LargeMesh.Triangles[TriIdx * 3 + 2];

        FVector Centroid = (LargeMesh.Vertices[V0] + LargeMesh.Vertices[V1] + LargeMesh.Vertices[V2]) / 3.0f;
        FVector RelativePos = Centroid - BoundingBox.Min;

        int32 CellX = FMath::Clamp(FMath::FloorToInt(RelativePos.X / CellSize.X), 0, GridCellsPerAxis - 1);
        int32 CellY = FMath::Clamp(FMath::FloorToInt(RelativePos.Y / CellSize.Y), 0, GridCellsPerAxis - 1);
        int32 CellZ = FMath::Clamp(FMath::FloorToInt(RelativePos.Z / CellSize.Z), 0, GridCellsPerAxis - 1);

        int32 ChunkIdx = CellX + CellY * GridCellsPerAxis + CellZ * GridCellsPerAxis * GridCellsPerAxis;

        if (ChunkIdx >= 0 && ChunkIdx < NumChunks)
        {
            bool bV0InChunk = VertexRemapping[ChunkIdx].Contains(V0);
            bool bV1InChunk = VertexRemapping[ChunkIdx].Contains(V1);
            bool bV2InChunk = VertexRemapping[ChunkIdx].Contains(V2);

            if (bV0InChunk && bV1InChunk && bV2InChunk)
            {
                ChunkTriangles[ChunkIdx].Add(VertexRemapping[ChunkIdx][V0]);
                ChunkTriangles[ChunkIdx].Add(VertexRemapping[ChunkIdx][V1]);
                ChunkTriangles[ChunkIdx].Add(VertexRemapping[ChunkIdx][V2]);
            }
            else if (bPreserveConnectivity)
            {
                UE_LOG(LogJUSYNC, Verbose, TEXT("Triangle %d spans multiple chunks, connectivity may be broken"), TriIdx);
            }
        }
    }

    int32 ValidChunks = 0;
    for (int32 ChunkIdx = 0; ChunkIdx < NumChunks; ++ChunkIdx)
    {
        if (ChunkVertices[ChunkIdx].Num() == 0 || ChunkTriangles[ChunkIdx].Num() == 0)
        {
            continue;
        }

        FJUSYNCMeshData ChunkMesh;
        ChunkMesh.ElementName = FString::Printf(TEXT("%s_Chunk%d"), *LargeMesh.ElementName, ChunkIdx);
        ChunkMesh.TypeName = LargeMesh.TypeName;

        ChunkMesh.Vertices.Reserve(ChunkVertices[ChunkIdx].Num());
        for (int32 OrigVertexIdx : ChunkVertices[ChunkIdx])
        {
            ChunkMesh.Vertices.Add(LargeMesh.Vertices[OrigVertexIdx]);
        }

        ChunkMesh.Triangles = ChunkTriangles[ChunkIdx];

        if (LargeMesh.Normals.Num() == TotalVertices)
        {
            ChunkMesh.Normals.Reserve(ChunkVertices[ChunkIdx].Num());
            for (int32 OrigVertexIdx : ChunkVertices[ChunkIdx])
            {
                ChunkMesh.Normals.Add(LargeMesh.Normals[OrigVertexIdx]);
            }
        }

        if (LargeMesh.UVs.Num() == TotalVertices)
        {
            ChunkMesh.UVs.Reserve(ChunkVertices[ChunkIdx].Num());
            for (int32 OrigVertexIdx : ChunkVertices[ChunkIdx])
            {
                ChunkMesh.UVs.Add(LargeMesh.UVs[OrigVertexIdx]);
            }
        }

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

            UE_LOG(LogJUSYNC, Log, TEXT("Chunk %d: %d vertices, %d triangles"),
                   ChunkIdx, ChunkMesh.Vertices.Num(), ChunkMesh.Triangles.Num() / 3);
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Split mesh into %d valid chunks (from %d total grid cells)"), ValidChunks, NumChunks);
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
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot check memory limits for invalid mesh"));
        return false;
    }

    const int32 TotalVertices = MeshData.Vertices.Num();
    const int32 TotalTriangles = MeshData.Triangles.Num() / 3;

    UE_LOG(LogJUSYNC, Log, TEXT("Mesh: %d vertices, %d triangles"), TotalVertices, TotalTriangles);
    UE_LOG(LogJUSYNC, Log, TEXT("Safety margin: %.1f%%"), SafetyMarginPercent);

    float BytesPerVertex = 36.0f; // Conservative estimate
    float BytesPerTriangle = 12.0f;

    if (MeshData.Normals.Num() > 0) BytesPerVertex += 12.0f;
    if (MeshData.UVs.Num() > 0) BytesPerVertex += 8.0f;
    if (MeshData.VertexColors.Num() > 0) BytesPerVertex += 4.0f;

    OutRequiredRAM_MB = (TotalVertices * BytesPerVertex + TotalTriangles * BytesPerTriangle) / (1024.0f * 1024.0f);

    OutRequiredVRAM_MB = OutRequiredRAM_MB * 2.5f;

    UE_LOG(LogJUSYNC, Log, TEXT("Estimated RAM usage: %.2f MB"), OutRequiredRAM_MB);
    UE_LOG(LogJUSYNC, Log, TEXT("Estimated VRAM usage: %.2f MB"), OutRequiredVRAM_MB);

    FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
    float AvailableRAMMB = MemoryStats.AvailablePhysical / (1024.0f * 1024.0f);

    float SafeAvailableRAMMB = AvailableRAMMB * (1.0f - SafetyMarginPercent / 100.0f);

    UE_LOG(LogJUSYNC, Log, TEXT("Available RAM: %.2f MB (Safe after %.1f%% margin: %.2f MB)"),
           AvailableRAMMB, SafetyMarginPercent, SafeAvailableRAMMB);

    bool bCanLoad = true;

    if (OutRequiredRAM_MB > SafeAvailableRAMMB)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("Mesh exceeds available RAM (%.2f MB > %.2f MB)"),
               OutRequiredRAM_MB, SafeAvailableRAMMB);
        bCanLoad = false;
    }

    const int32 RecommendedMaxVertices = 32768;
    const int32 RecommendedMaxTriangles = 65536;

    if (TotalVertices > RecommendedMaxVertices)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("Mesh exceeds RMC vertex limit (%d > %d)"),
               TotalVertices, RecommendedMaxVertices);
        bCanLoad = false;
    }

    if (TotalTriangles > RecommendedMaxTriangles)
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("Mesh exceeds RMC triangle limit (%d > %d)"),
               TotalTriangles, RecommendedMaxTriangles);
        bCanLoad = false;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Can load: %s"), bCanLoad ? TEXT("Yes") : TEXT("No"));
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
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot create RMC components for invalid mesh"));
        return CreatedComponents;
    }

    if (!ParentActor)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Parent actor is null"));
        return CreatedComponents;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Large mesh: %d vertices, %d triangles"),
           LargeMesh.Vertices.Num(), LargeMesh.Triangles.Num() / 3);
    UE_LOG(LogJUSYNC, Log, TEXT("Parent actor: %s"), *ParentActor->GetName());
    UE_LOG(LogJUSYNC, Log, TEXT("Max vertices per component: %d"), MaxVerticesPerComponent);

    UMaterialInterface* DefaultMaterial = this->GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"));

    TArray<FJUSYNCMeshData> SplitMeshes = this->SplitLargeMeshForRealtimeMesh(
        LargeMesh,
        MaxVerticesPerComponent,
        true); // Preserve connectivity

    if (SplitMeshes.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to split mesh"));
        return CreatedComponents;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Split mesh into %d chunks"), SplitMeshes.Num());

    for (int32 ChunkIdx = 0; ChunkIdx < SplitMeshes.Num(); ++ChunkIdx)
    {
        const FJUSYNCMeshData& ChunkMesh = SplitMeshes[ChunkIdx];

        FString ComponentName = FString::Printf(TEXT("RMC_%s_Chunk%d"), *LargeMesh.ElementName, ChunkIdx);

        URealtimeMeshComponent* ChunkComponent = NewObject<URealtimeMeshComponent>(ParentActor, FName(*ComponentName));
        if (!ChunkComponent)
        {
            UE_LOG(LogJUSYNC, Error, TEXT("Failed to create RMC component for chunk %d"), ChunkIdx);
            continue;
        }

        ChunkComponent->RegisterComponent();
        ChunkComponent->AttachToComponent(ParentActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);

        if (this->CreateRealtimeMeshFromJUSYNC(ChunkMesh, ChunkComponent))
        {
            if (DefaultMaterial)
            {
                ChunkComponent->SetMaterial(0, DefaultMaterial);
            }

            CreatedComponents.Add(ChunkComponent);

            UE_LOG(LogJUSYNC, Log, TEXT("Created chunk %d: %s (%d vertices, %d triangles)"),
                   ChunkIdx, *ChunkComponent->GetName(),
                   ChunkMesh.Vertices.Num(), ChunkMesh.Triangles.Num() / 3);
        }
        else
        {
            UE_LOG(LogJUSYNC, Error, TEXT("Failed to create mesh for chunk %d"), ChunkIdx);
            ChunkComponent->DestroyComponent();
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Created %d RMC components for large mesh"), CreatedComponents.Num());
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

    const int32 TotalMeshes = MeshDataArray.Num();
    double StartTime = FPlatformTime::Seconds();

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


bool UJUSYNCSubsystem::LoadPointCloudFromBuffer(const TArray<uint8>& Buffer, const FString& Filename, TArray<FJUSYNCPointCloudData>& OutPointCloudData)
{
    if (Buffer.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: LoadPointCloudFromBuffer called with empty buffer"));
        return false;
    }

    OutPointCloudData.Empty();

#ifdef WITH_ANARI_USD_MIDDLEWARE
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

        pc.BoundingBoxMin = FVector(cpc.bounding_box_min[0], cpc.bounding_box_min[2], -cpc.bounding_box_min[1]);
        pc.BoundingBoxMax = FVector(cpc.bounding_box_max[0], cpc.bounding_box_max[2], -cpc.bounding_box_max[1]);

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
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: Cannot spawn point cloud - invalid data"));
        return nullptr;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC: Cannot spawn point cloud - no valid world"));
        return nullptr;
    }

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

    ULidarPointCloudComponent* Comp = SpawnedActor->GetPointCloudComponent();
    if (Comp)
    {
        Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Comp->ColorSource = ELidarPointCloudColorationMode::Data;
        Comp->PointSize = 1.0f;
        Comp->SetPointShape(ELidarPointCloudSpriteShape::Square);
        if (PCSpawner.IsValid())
        {
            PCSpawner->ApplyVisualSettingsToComponent(Comp);
        }
        Comp->MinDepth = 0;
        Comp->MaxDepth = -1;
        Comp->bUseFrustumCulling = true;
    }

    TArray<FVector> Positions = PointCloudData.Positions;
    TArray<FColor> Colors = PointCloudData.Colors;
    bool bHasColors = PointCloudData.HasColors();
    TArray<float> Widths = PointCloudData.Widths;
    int32 PointCount = PointCloudData.PointCount;
    FString ElementNameCopy = PointCloudData.ElementName;

    // Copy gradient LUT for thread-safe use (same as spawner path)

    TArray<FColor> PCLUT = PCSpawner.IsValid() ? PCSpawner->GetGradientLUT() : TArray<FColor>();
    bool bUseGradient = PCLUT.Num() > 0 && !bHasColors && Widths.Num() > 0;
    FJUSYNCPointCloudSpawner* PCSpawnerPtr = PCSpawner.Get();

    TWeakObjectPtr<ALidarPointCloudActor> WeakActor = SpawnedActor;
    TWeakObjectPtr<ULidarPointCloudComponent> WeakComp = Comp;
    FString ElementNameForLog = PointCloudData.ElementName;

    // UObject creation must happen on the game thread. The point array is still built asynchronously.

    ULidarPointCloud* LidarCloud = NewObject<ULidarPointCloud>(SpawnedActor);
    LidarCloud->SetOptimizedForDynamicData(true);
    SpawnedActor->SetActorHiddenInGame(true);

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [PointCount, Positions, Colors, bHasColors, Widths, WeakActor, WeakComp, PCLUT, bUseGradient, ElementNameForLog, LidarCloud, PCSpawnerPtr]()
    {
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
                // Neutral gray instead of white (see FJUSYNCPointCloudSpawner legacy path):
                // white point clouds glare like a glow until recolored.
                col = FColor(128, 128, 128);
            }
            Points[i] = FLidarPointCloudPoint(pos.X, pos.Y, pos.Z, col.R / 255.f, col.G / 255.f, col.B / 255.f, col.A / 255.f);
        }

        {
            if (Points.Num() > 1)
            {
                FVector3f MinBounds(FLT_MAX, FLT_MAX, FLT_MAX);
                FVector3f MaxBounds(-FLT_MAX, -FLT_MAX, -FLT_MAX);

                for (const auto& P : Points)
                {
                    MinBounds.X = FMath::Min(MinBounds.X, P.Location.X);
                    MinBounds.Y = FMath::Min(MinBounds.Y, P.Location.Y);
                    MinBounds.Z = FMath::Min(MinBounds.Z, P.Location.Z);
                    MaxBounds.X = FMath::Max(MaxBounds.X, P.Location.X);
                    MaxBounds.Y = FMath::Max(MaxBounds.Y, P.Location.Y);
                    MaxBounds.Z = FMath::Max(MaxBounds.Z, P.Location.Z);
                }

                FVector3f Extent = MaxBounds - MinBounds;

                if (Extent.X < 0.01f || Extent.Y < 0.01f || Extent.Z < 0.01f)
                {
                    float MaxExtent = FMath::Max3(Extent.X, Extent.Y, Extent.Z);
                    float FallbackExtent = FMath::Max(MaxExtent, 1.0f);

                    for (int32 i = 0; i < Points.Num(); ++i)
                    {
                        FVector3f& Pos = Points[i].Location;
                        float Offset = ((float)i - (float)Points.Num() * 0.5f) * FallbackExtent / (float)FMath::Max(Points.Num(), 1);

                        if (Extent.X < 0.01f) Pos.X += Offset;
                        if (Extent.Y < 0.01f) Pos.Y += Offset;
                        if (Extent.Z < 0.01f) Pos.Z += Offset;
                    }

                    UE_LOG(LogTemp, Warning,
                        TEXT("JUSYNC: Fixed degenerate bounds for '%s' (extent: %.3f, %.3f, %.3f)"),
                        *ElementNameForLog, Extent.X, Extent.Y, Extent.Z);
                }
            }
        }

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakActor, WeakComp, LidarCloud, Points = MoveTemp(Points), PCSpawnerPtr]() mutable
            {
                if (!WeakComp.IsValid() || !LidarCloud)
                {
                    return;
                }

                const bool bDataAccepted = LidarCloud->SetData(Points);
                WeakComp->SetPointCloud(LidarCloud);

                if (WeakActor.IsValid() && bDataAccepted)
                {
                    if (PCSpawnerPtr)
                    {
                        PCSpawnerPtr->RequestNormalCalculation(LidarCloud, WeakActor.Get(), static_cast<int32>(Points.Num()));
                    }
                    WeakActor->SetActorHiddenInGame(false);
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

    if (TSoftObjectPtr<UMaterialInterface>* CachedMaterial = MaterialCache.Find(MaterialPath))
    {
        if (CachedMaterial->IsValid())
        {
            UE_LOG(LogJUSYNC, Verbose, TEXT("Using cached material: %s"), *MaterialPath);
            return CachedMaterial->Get();
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Loading and caching material: %s"), *MaterialPath);
    UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);

    if (Material)
    {
        MaterialCache.Add(MaterialPath, Material);
        UE_LOG(LogJUSYNC, Log, TEXT("Cached material: %s"), *MaterialPath);
    }
    else
    {
        UE_LOG(LogJUSYNC, Warning, TEXT("Failed to load material: %s"), *MaterialPath);
    }

    return Material;
}


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

    ProcessedData.Normals.Reserve(ProcessedData.FinalVertexCount);
    for (int32 i = 0; i < ProcessedData.FinalVertexCount; ++i)
    {
        FVector3f Normal = FVector3f(MeshData.Normals.IsValidIndex(i) ? MeshData.Normals[i] : FVector::UpVector);
        ProcessedData.Normals.Add(Normal);
    }

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

    ProcessedData.Triangles = MeshData.Triangles;

    UE_LOG(LogJUSYNC, Verbose, TEXT("Processed mesh data: %d vertices, %d triangles"),
           ProcessedData.FinalVertexCount, ProcessedData.FinalTriCount);

    return ProcessedData;
}

void UJUSYNCSubsystem::ApplyProcessedMeshToComponent(const FProcessedMeshData& ProcessedData, URealtimeMeshComponent* RealtimeMeshComponent)
{
    if (!RealtimeMeshComponent)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid component for applying processed mesh"));
        return;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Applying processed mesh to component on game thread: %s"), *ProcessedData.ElementName);

    UMaterialInterface* ExistingMaterial = RealtimeMeshComponent->GetMaterial(0);
    ExistingMaterial = JUSYNCResolveMaterialForColoredMesh(this, ExistingMaterial, ProcessedData.Colors,
        RealtimeMeshComponent, TEXT("ApplyProcessedMeshToComponent"));
    bool bHadExistingMaterial = (ExistingMaterial != nullptr);

    if (bHadExistingMaterial)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Storing existing material for reapplication: %s"),
               *ExistingMaterial->GetName());
    }

    // Initialize RealtimeMesh (game-thread only)

    URealtimeMeshSimple* RealtimeMesh = RealtimeMeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
    if (!RealtimeMesh)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to initialize RealtimeMesh"));
        return;
    }

    RealtimeMesh->SetupMaterialSlot(0, TEXT("PrimaryMaterial"));

    if (!bHadExistingMaterial)
    {
        UMaterialInterface* VertexColorMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"));
        if (VertexColorMaterial)
        {
            RealtimeMeshComponent->SetMaterial(0, VertexColorMaterial);
            UE_LOG(LogJUSYNC, Log, TEXT("Applied cached M_VertexColor material as fallback"));
        }
        else
        {
            UMaterialInterface* DefaultMat = GetCachedMaterial(TEXT("/Engine/EngineMaterials/DefaultMaterial"));
            if (DefaultMat)
            {
                auto* DynMat = UMaterialInstanceDynamic::Create(DefaultMat, RealtimeMeshComponent);
                DynMat->SetScalarParameterValue(TEXT("UseVertexColor"), 1.0f);
                RealtimeMeshComponent->SetMaterial(0, DynMat);
                UE_LOG(LogJUSYNC, Log, TEXT("Applied enhanced default material as fallback"));
            }
        }
    }
    else
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Using provided material (preserving texture material from Blueprint)"));
    }

    RealtimeMesh::FRealtimeMeshStreamSet Streams;
    auto Builder = RealtimeMesh::TRealtimeMeshBuilderLocal<uint32>(Streams);
    Builder.EnableTangents();
    Builder.EnableTexCoords();
    Builder.EnableColors();
    Builder.EnablePolyGroups();

    for (int32 i = 0; i < ProcessedData.FinalVertexCount; ++i)
    {
        Builder.AddVertex(ProcessedData.Positions[i]);
        Builder.SetNormal(i, ProcessedData.Normals[i]);
        Builder.SetTexCoord(i, 0, ProcessedData.UVs[i]);
        Builder.SetColor(i, ProcessedData.Colors[i]);
    }

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
            UE_LOG(LogJUSYNC, Error, TEXT("Invalid triangle %d: [%d,%d,%d] vs %d vertices"),
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
    RealtimeMesh->UpdateSectionConfig(SectionKey, SectionConfig, bEnableSpawnedMeshCollision);
    EnableSpawnedMeshCollision(RealtimeMeshComponent, RealtimeMesh);

    RealtimeMeshComponent->MarkRenderStateDirty();

    if (bHadExistingMaterial && ExistingMaterial)
    {
        RealtimeMeshComponent->SetMaterial(0, ExistingMaterial);
        UE_LOG(LogJUSYNC, Log, TEXT("Reapplied existing material after mesh creation: %s"),
               *ExistingMaterial->GetName());

        UMaterialInterface* CurrentMaterial = RealtimeMeshComponent->GetMaterial(0);
        if (CurrentMaterial == ExistingMaterial)
        {
            UE_LOG(LogJUSYNC, Log, TEXT("Material verification passed: Material correctly applied"));
        }
        else
        {
            UE_LOG(LogJUSYNC, Warning, TEXT("Material verification failed: Expected %s, Got %s"),
                   *ExistingMaterial->GetName(),
                   CurrentMaterial ? *CurrentMaterial->GetName() : TEXT("NULL"));
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Applied processed mesh '%s' (%d verts, %d tris)"),
           *ProcessedData.ElementName, ProcessedData.FinalVertexCount, ProcessedData.FinalTriCount);
}

void UJUSYNCSubsystem::CreateRealtimeMeshFromJUSYNC_Async(
    const FJUSYNCMeshData& MeshData,
    URealtimeMeshComponent* RealtimeMeshComponent)
{
    if (!RealtimeMeshComponent || !MeshData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid input to CreateRealtimeMeshFromJUSYNC_Async"));
        return;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Starting async mesh creation for: %s"), *MeshData.ElementName);

    FJUSYNCMeshData MeshDataCopy = MeshData;
    TWeakObjectPtr<URealtimeMeshComponent> ComponentPtr = RealtimeMeshComponent;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, MeshDataCopy, ComponentPtr]()
    {
        UE_LOG(LogJUSYNC, Verbose, TEXT("Processing mesh data on background thread: %s"), *MeshDataCopy.ElementName);

        // CPU-intensive processing (thread-safe)

        FProcessedMeshData ProcessedData = ProcessMeshDataCPU(MeshDataCopy);

        AsyncTask(ENamedThreads::GameThread, [this, ProcessedData, ComponentPtr]()
        {
            if (ComponentPtr.IsValid())
            {
                ApplyProcessedMeshToComponent(ProcessedData, ComponentPtr.Get());
                UE_LOG(LogJUSYNC, Log, TEXT("Async mesh creation complete: %s"), *ProcessedData.ElementName);
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

    UMaterialInterface* FinalBaseMaterial = BaseMaterial;
    if (!FinalBaseMaterial)
    {
        FinalBaseMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_BaseMaterial"));
        if (!FinalBaseMaterial)
        {
            FinalBaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
        }
    }

    FName FinalTextureParameterName = TextureParameterName;
    if (FinalTextureParameterName.IsNone())
    {
        FinalTextureParameterName = TEXT("BaseColor");
    }

    TWeakObjectPtr<UTexture2D> TexturePtr = Texture;
    TWeakObjectPtr<UMaterialInterface> BaseMaterialPtr = FinalBaseMaterial;

    std::function<void(UMaterialInstanceDynamic*)> MaterialCreatedCallback = OnMaterialCreated;
    FName CapturedTextureParameterName = FinalTextureParameterName;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, TexturePtr, BaseMaterialPtr, CapturedTextureParameterName, MaterialCreatedCallback]()
    {
        // On background thread: prepare material data (could do validation here)

        AsyncTask(ENamedThreads::GameThread, [this, TexturePtr, BaseMaterialPtr, CapturedTextureParameterName, MaterialCreatedCallback]()
        {
            if (!TexturePtr.IsValid() || !BaseMaterialPtr.IsValid())
            {
                UE_LOG(LogJUSYNC, Error, TEXT("CreateMaterialFromTexture_Async_Return: Objects no longer valid"));
                return;
            }

            UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(BaseMaterialPtr.Get(), nullptr);
            if (DynamicMaterial)
            {
                DynamicMaterial->SetTextureParameterValue(CapturedTextureParameterName, TexturePtr.Get());

                UE_LOG(LogJUSYNC, Log, TEXT("Created dynamic material from texture (Base: %s, Param: %s)"),
                       *BaseMaterialPtr->GetName(), *CapturedTextureParameterName.ToString());

                if (MaterialCreatedCallback)
                {
                    MaterialCreatedCallback(DynamicMaterial);
                }
            }
            else
            {
                UE_LOG(LogJUSYNC, Error, TEXT("Failed to create dynamic material instance"));
            }
        });
    });
}


AActor* UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(const FJUSYNCMeshData& MeshData, const FVector& SpawnLocation, const FRotator& SpawnRotation, UMaterialInterface* CustomMaterial)
{
    return SpawnRealtimeMeshAtLocation_WithStreams(MeshData, SpawnLocation, SpawnRotation, CustomMaterial, nullptr);
}

AActor* UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation_WithStreams(const FJUSYNCMeshData& MeshData, const FVector& SpawnLocation, const FRotator& SpawnRotation, UMaterialInterface* CustomMaterial, RealtimeMesh::FRealtimeMeshStreamSet* PrebuiltStreams)
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

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

    static std::atomic<int32> GlobalSpawnCounter{0};
    int32 SpawnIdx = GlobalSpawnCounter.fetch_add(1, std::memory_order_relaxed);
    uint32 FilenameHash = GetTypeHash(MeshData.ElementName);
    FString BaseActorName = FString::Printf(TEXT("JUSYNC_H%x_N%d"), FilenameHash, SpawnIdx);

    FName UniqueActorName = MakeUniqueObjectName(World, AActor::StaticClass(), FName(*BaseActorName));

    SpawnParams.Name = UniqueActorName;

    AActor* SpawnedActor = World->SpawnActor<AActor>(SpawnParams);
    if (!SpawnedActor)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to spawn actor"));
        return nullptr;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Spawned actor with final name: %s (original base: %s)"), *SpawnedActor->GetName(), *BaseActorName);

    URealtimeMeshComponent* MeshComp = NewObject<URealtimeMeshComponent>(SpawnedActor);
    SpawnedActor->SetRootComponent(MeshComp);
    MeshComp->RegisterComponent();

    if (CustomMaterial)
    {
        MeshComp->SetMaterial(0, CustomMaterial);
    }

    SpawnedActor->SetActorLocation(SpawnLocation);
    SpawnedActor->SetActorRotation(SpawnRotation);

    bool bSuccess = Subsystem->CreateRealtimeMeshFromJUSYNC_WithStreams(MeshData, MeshComp, PrebuiltStreams);

    if (bSuccess)
    {
        FVector ActualLocation = SpawnedActor->GetActorLocation();
        FString Message = FString::Printf(TEXT("RealtimeMesh spawned: %s at %s"), *MeshData.ElementName, *ActualLocation.ToString());
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


AActor* UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation_WithCompact(const FJUSYNCCompactMeshData& MeshData, const FVector& SpawnLocation, const FRotator& SpawnRotation, UMaterialInterface* CustomMaterial, RealtimeMesh::FRealtimeMeshStreamSet* PrebuiltStreams)
{
    if (!MeshData.IsValid())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Invalid compact mesh data for spawning"));
        return nullptr;
    }

    UJUSYNCSubsystem* Subsystem = GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("No JUSYNC subsystem available for spawning"));
        return nullptr;
    }

    UWorld* World = Subsystem->GetWorld();
    if (!World)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("No valid world context for spawning"));
        return nullptr;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

    static std::atomic<int32> GlobalSpawnCounter{0};
    int32 SpawnIdx = GlobalSpawnCounter.fetch_add(1, std::memory_order_relaxed);
    uint32 FilenameHash = GetTypeHash(MeshData.ElementName);
    FString BaseActorName = FString::Printf(TEXT("JUSYNC_H%x_N%d"), FilenameHash, SpawnIdx);
    FName UniqueActorName = MakeUniqueObjectName(World, AActor::StaticClass(), FName(*BaseActorName));
    SpawnParams.Name = UniqueActorName;

    AActor* SpawnedActor = World->SpawnActor<AActor>(SpawnParams);
    if (!SpawnedActor)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to spawn actor"));
        return nullptr;
    }

    URealtimeMeshComponent* MeshComp = NewObject<URealtimeMeshComponent>(SpawnedActor);
    SpawnedActor->SetRootComponent(MeshComp);
    MeshComp->RegisterComponent();

    if (CustomMaterial)
    {
        MeshComp->SetMaterial(0, CustomMaterial);
    }

    SpawnedActor->SetActorLocation(SpawnLocation);
    SpawnedActor->SetActorRotation(SpawnRotation);

    bool bSuccess = Subsystem->CreateRealtimeMeshFromJUSYNC_WithCompact(MeshData, MeshComp, PrebuiltStreams);
    if (bSuccess)
    {
        return SpawnedActor;
    }

    SpawnedActor->Destroy();
    UE_LOG(LogJUSYNC, Error, TEXT("Failed to create compact RealtimeMesh, destroying actor"));
    return nullptr;
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
    UE_LOG(LogJUSYNC, Log, TEXT("MeshDataArray.Num(): %d"), MeshDataArray.Num());
    UE_LOG(LogJUSYNC, Log, TEXT("SpawnLocations.Num(): %d"), SpawnLocations.Num());
    UE_LOG(LogJUSYNC, Log, TEXT("SpawnRotations.Num(): %d"), SpawnRotations.Num());
    UE_LOG(LogJUSYNC, Log, TEXT("Async Mode: %s"), bUseAsyncSpawning ? TEXT("YES") : TEXT("NO"));

    TArray<FVector> FinalLocations = SpawnLocations;
    if (FinalLocations.Num() == 0)
    {
        FinalLocations = UJUSYNCBlueprintLibrary::GenerateDefaultLocations(MeshDataArray.Num());
        UE_LOG(LogJUSYNC, Log, TEXT("Generated %d default locations"), FinalLocations.Num());
    }
    else if (FinalLocations.Num() != MeshDataArray.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Array size mismatch! Meshes: %d, Locations: %d"),
               MeshDataArray.Num(), FinalLocations.Num());
        return TArray<AActor*>();
    }

    TArray<FRotator> FinalRotations = SpawnRotations;
    if (FinalRotations.Num() == 0)
    {
        FinalRotations = UJUSYNCBlueprintLibrary::GenerateDefaultRotations(MeshDataArray.Num());
        UE_LOG(LogJUSYNC, Log, TEXT("Generated %d default rotations"), FinalRotations.Num());
    }
    else if (FinalRotations.Num() != MeshDataArray.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Rotation array size mismatch! Expected: %d, Got: %d"),
               MeshDataArray.Num(), FinalRotations.Num());
        return TArray<AActor*>();
    }

    if (!bUseAsyncSpawning)
    {
        return BatchSpawnRealtimeMeshesAtLocationsSync(MeshDataArray, SpawnLocations, FinalRotations);
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Starting ASYNC batch spawn with rotations"));
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
        UE_LOG(LogJUSYNC, Error, TEXT("No JUSYNC Subsystem"));
        return SpawnLocations;
    }

    UWorld* World = Subsystem->GetWorld();
    if (!World)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("No World context"));
        return SpawnLocations;
    }

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

    UE_LOG(LogJUSYNC, Log, TEXT("Total spawn points: %d"), SpawnLocations.Num());
    return SpawnLocations;
}


bool UJUSYNCSubsystem::ConnectToBroker(const FString& BrokerEndpoint, int32 TimeoutMs)
{
    FScopeLock Lock(&MiddlewareMutex);

    UE_LOG(LogJUSYNC, Log, TEXT("Connecting to ANARI USD broker"));
    UE_LOG(LogJUSYNC, Log, TEXT("Broker Endpoint: %s"), *BrokerEndpoint);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot connect to broker - middleware not initialized"));
        return false;
    }

    FTCHARToUTF8 EndpointConverter(*BrokerEndpoint);
    const char* EndpointCStr = BrokerEndpoint.IsEmpty() ? "tcp://localhost:5556" : EndpointConverter.Get();

    UE_LOG(LogJUSYNC, Log, TEXT("Calling ConnectToBroker_C..."));
    int Result = ConnectToBroker_C(EndpointCStr, TimeoutMs);

    UE_LOG(LogJUSYNC, Log, TEXT("ConnectToBroker_C returned: %d"), Result);

    if (Result == 1)
    {
        UE_LOG(LogJUSYNC, Log, TEXT("Connected to ANARI USD broker at %s"), *BrokerEndpoint);
        UE_LOG(LogJUSYNC, Log, TEXT("DEALER socket connected through SSH tunnel"));
        UE_LOG(LogJUSYNC, Log, TEXT("Ready for synchronous file requests"));

        RegisterNotificationCallback_C(NotificationCallback_Static);
        RegisterSceneUpdateCallback_C(JUSYNCSceneUpdateCallback_Static);
        RegisterProtocolDiagnosticsCallback_C(JUSYNCProtocolDiagnosticsCallback_Static);
        UE_LOG(LogJUSYNC, Log, TEXT("Notification, scene-update, and protocol-diagnostics callbacks registered"));
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC protocol version: %u"), GetProtocolVersion_C());

        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to connect to broker (Result: %d)"), Result);
        return false;
    }
#endif
    return false;
}

void UJUSYNCSubsystem::DisconnectFromBroker()
{
    FScopeLock Lock(&MiddlewareMutex);

    UE_LOG(LogJUSYNC, Log, TEXT("Disconnecting from ANARI USD broker"));

#ifdef WITH_ANARI_USD_MIDDLEWARE
    DisconnectFromBroker_C();
    UE_LOG(LogJUSYNC, Log, TEXT("Disconnected from ANARI USD broker"));
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

    UE_LOG(LogJUSYNC, Log, TEXT("Requesting file list from broker"));
    UE_LOG(LogJUSYNC, Log, TEXT("Target Rank: %d"), TargetRank);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request file list - middleware not initialized"));
        return false;
    }

    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request file list - not connected to broker"));
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

        FreeFileList_C(FileList, FileCount);

        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved %d files from broker"), OutFiles.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to request file list (Result: %d)"), Result);
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

    int32 AdjustedTimeoutMs = TimeoutMs;
    if (TargetRank == -1) {
        int32 TotalWorkerCount = 1; // Default to single-rank mode
        if (RequestTotalWorkerCount(2000, TotalWorkerCount)) {
            UE_LOG(LogJUSYNC, Log, TEXT("Broadcast request: dynamically detected %d total workers (including rank 0)"), TotalWorkerCount);

            if (TotalWorkerCount == 1) {
                if (TimeoutMs < 3000) {
                    AdjustedTimeoutMs = 3000;
                    UE_LOG(LogJUSYNC, Log, TEXT("Single-rank mode: using 3000 ms timeout for broadcast"));
                } else {
                    AdjustedTimeoutMs = TimeoutMs;
                }
                UE_LOG(LogJUSYNC, Log, TEXT("Single-rank mode: broadcast (-1) will be handled as direct request to rank 0"));
            } else {
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

    UE_LOG(LogJUSYNC, Log, TEXT("Requesting file list (with sizes) from broker"));
    UE_LOG(LogJUSYNC, Log, TEXT("Target Rank: %d"), TargetRank);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms (adjusted from %d ms)"), AdjustedTimeoutMs, TimeoutMs);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request file list - middleware not initialized"));
        return false;
    }

    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request file list - not connected to broker"));
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

        FreeFileListWithSizes_C(FileList, FileSizes, FileCount);

        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved %d files with sizes from broker"), OutFiles.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to request file list with sizes (Result: %d)"), Result);
        if (FileList || FileSizes)
        {
            FreeFileListWithSizes_C(FileList, FileSizes, FileCount);
        }
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestFileListWithSizesAndRanks(int32 TargetRank, int32 TimeoutMs, TArray<FString>& OutFiles, TArray<int64>& OutSizes, TArray<int32>& OutRanks, TArray<uint64>* OutHashLo, TArray<uint64>* OutHashHi)
{

    const int32 AdjustedTimeoutMs = TimeoutMs;

    UE_LOG(LogJUSYNC, Log, TEXT("Requesting file list (with sizes and ranks) from broker"));
    UE_LOG(LogJUSYNC, Log, TEXT("Target Rank: %d"), TargetRank);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), AdjustedTimeoutMs);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request file list - middleware not initialized"));
        return false;
    }

    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request file list - not connected to broker"));
        return false;
    }

    char** FileList = nullptr;
    uint64_t* FileSizes = nullptr;
    int32_t* FileRanks = nullptr;
    uint64_t* FileHashLo = nullptr;
    uint64_t* FileHashHi = nullptr;
    size_t FileCount = 0;

    UE_LOG(LogJUSYNC, Log, TEXT("Calling RequestFileListWithSizesAndRanks_C..."));
    int Result = RequestFileListWithSizesAndRanks_C(TargetRank, &FileList, &FileSizes, &FileRanks, &FileHashLo, &FileHashHi, &FileCount, AdjustedTimeoutMs);

    UE_LOG(LogJUSYNC, Log, TEXT("RequestFileListWithSizesAndRanks_C returned: %d, FileCount: %zu"), Result, FileCount);

    if (Result == 1 && FileList && FileSizes && FileRanks && FileCount > 0)
    {
        OutFiles.Empty();
        OutSizes.Empty();
        OutRanks.Empty();
        OutFiles.Reserve(FileCount);
        OutSizes.Reserve(FileCount);
        OutRanks.Reserve(FileCount);
        if (OutHashLo) OutHashLo->Reserve(FileCount);
        if (OutHashHi) OutHashHi->Reserve(FileCount);

        for (size_t i = 0; i < FileCount; ++i)
        {
            if (FileList[i])
            {
                OutFiles.Add(FString(UTF8_TO_TCHAR(FileList[i])));
                OutSizes.Add(static_cast<int64>(FileSizes[i]));
                OutRanks.Add(static_cast<int32>(FileRanks[i]));
                if (OutHashLo && FileHashLo) OutHashLo->Add(FileHashLo[i]);
                if (OutHashHi && FileHashHi) OutHashHi->Add(FileHashHi[i]);
            }
        }

        FreeFileListWithSizesAndRanks_C(FileList, FileSizes, FileRanks, nullptr, nullptr, FileCount);

        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved %d files with sizes and ranks from broker"), OutFiles.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to request file list with sizes and ranks (Result: %d)"), Result);
        if (FileList || FileSizes || FileRanks)
        {
            FreeFileListWithSizesAndRanks_C(FileList, FileSizes, FileRanks, FileHashLo, FileHashHi, FileCount);
        }
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestFile(const FString& Filename, int32 TargetRank, int32 TimeoutMs, TArray<uint8>& OutData)
{

    if (Filename.IsEmpty())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request file - filename is empty"));
        return false;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Requesting file from broker"));
    UE_LOG(LogJUSYNC, Log, TEXT("Filename: %s"), *Filename);
    UE_LOG(LogJUSYNC, Log, TEXT("Target Rank: %d"), TargetRank);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request file - middleware not initialized"));
        return false;
    }

    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request file - not connected to broker"));
        return false;
    }

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
        UE_LOG(LogJUSYNC, Error, TEXT("Exception caught in RequestFile_C"));
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
        const size_t MAX_REASONABLE_FILE_SIZE = 100ULL * 1024 * 1024 * 1024; // 100GB
        if (FileSize > MAX_REASONABLE_FILE_SIZE)
        {
            UE_LOG(LogJUSYNC, Error, TEXT("File size suspiciously large: %llu bytes (max: %llu)"), FileSize, MAX_REASONABLE_FILE_SIZE);
            FreeBuffer_C(FileData);
            return false;
        }

        OutData.Empty();
        OutData.SetNum(FileSize);

        if (OutData.Num() != static_cast<int32>(FileSize))
        {
            UE_LOG(LogJUSYNC, Error, TEXT("Failed to allocate buffer for file (requested: %llu, got: %d)"), FileSize, OutData.Num());
            FreeBuffer_C(FileData);
            return false;
        }

        uint8* DestPtr = OutData.GetData();
        if (!DestPtr && FileSize > 0)
        {
            UE_LOG(LogJUSYNC, Error, TEXT("Destination buffer is null for non-zero file size"));
            FreeBuffer_C(FileData);
            return false;
        }

        FMemory::Memcpy(DestPtr, FileData, FileSize);

        FreeBuffer_C(FileData);
        FileData = nullptr; // Prevent accidental reuse

        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved file '%s' (%d bytes) from broker"), *Filename, OutData.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to request file (Result: %d)"), Result);
        if (FileData)
        {
            FreeBuffer_C(FileData);
            FileData = nullptr;
        }
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestFileSized(const FString& Filename, int32 TargetRank, int32 TimeoutMs,
                                        TArray<uint8>& OutData, uint64 ExpectedFileSize, uint64 AbortHandle)
{
    if (Filename.IsEmpty())
    {
        return false;
    }

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (bIsInitialized.load() && IsBrokerConnected() && ExpectedFileSize > 0)
    {
        const uint64 MAX_REASONABLE_FILE_SIZE = 100ULL * 1024 * 1024 * 1024; // 100GB
        if (ExpectedFileSize <= MAX_REASONABLE_FILE_SIZE)
        {
            FTCHARToUTF8 FilenameConverter(*Filename);
            const char* FilenameCStr = FilenameConverter.Get();

            OutData.SetNumUninitialized(static_cast<int32>(ExpectedFileSize));
            if (OutData.Num() == static_cast<int32>(ExpectedFileSize))
            {
                size_t Written = 0;
                const int64_t rc = RequestFileIntoBufferAbort_C(
                    FilenameCStr, TargetRank, TimeoutMs,
                    OutData.GetData(), static_cast<size_t>(OutData.Num()), &Written,
                    static_cast<uint64_t>(AbortHandle));

                if (rc > 0)
                {
                    OutData.SetNum(static_cast<int32>(Written));
                    UE_LOG(LogJUSYNC, Log, TEXT("Retrieved file '%s' (%d bytes) from broker (in-situ)"), *Filename, OutData.Num());
                    return true;
                }
                if (rc == -2)
                {
                    UE_LOG(LogJUSYNC, Warning, TEXT("In-situ download overflow for '%s'; using legacy path"), *Filename);
                }
                else
                {
                    if (AbortHandle != 0)
                    {
                        UE_LOG(LogJUSYNC, Verbose, TEXT("In-situ download of '%s' ended early (aborted/failed)"), *Filename);
                    }
                    else
                    {
                        UE_LOG(LogJUSYNC, Error, TEXT("In-situ download failed for '%s'"), *Filename);
                    }
                    return false;
                }
            }
            else
            {
                UE_LOG(LogJUSYNC, Warning, TEXT("In-situ allocation failed for '%s'; using legacy path"), *Filename);
            }
        }
    }
#endif
    return RequestFile(Filename, TargetRank, TimeoutMs, OutData);
}

bool UJUSYNCSubsystem::RequestFilesParallel(const TArray<FString>& Filenames, const TArray<int32>& TargetRanks, int32 TimeoutMs, TArray<FJUSYNCFileData>& OutFiles)
{

    if (Filenames.Num() == 0)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request files - filename list is empty"));
        return false;
    }

    if (Filenames.Num() != TargetRanks.Num())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request files - mismatch between filenames count (%d) and target ranks count (%d)"), Filenames.Num(), TargetRanks.Num());
        return false;
    }

    UE_LOG(LogJUSYNC, Log, TEXT("Requesting %d files in parallel from broker"), Filenames.Num());

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request files - middleware not initialized"));
        return false;
    }

    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request files - not connected to broker"));
        return false;
    }

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

    std::atomic<bool> bDownloadComplete{false};
    std::atomic<bool> bDownloadSuccess{false};
    std::atomic<int> FilesReceived{0};
    std::atomic<int> FilesExpected{static_cast<int>(Filenames.Num())};

    std::vector<const char*> FilenameCStrs;
    std::vector<int32_t> TargetRanksArray;

    FilenameCStrs.reserve(Filenames.Num());
    TargetRanksArray.reserve(TargetRanks.Num());

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

    auto FileReceivedCallback = [](const char* filename, const unsigned char* data, size_t data_size) {
        FScopeLock CallbackLock(&CallbackMutex);
        if (CurrentOutFiles && CurrentFilesReceived) {
            FJUSYNCFileData FileData;
            FileData.Filename = UTF8_TO_TCHAR(filename);
            FileData.Data.Append(data, data_size);
            FileData.FileType = FPaths::GetExtension(FileData.Filename);
            FileData.SourceRank = -1;
            FileData.Hash = TEXT("");
            CurrentOutFiles->Add(FileData);
            (*CurrentFilesReceived)++;
            UE_LOG(LogJUSYNC, Log, TEXT("Parallel download received: %s (%d bytes)"), *FileData.Filename, FileData.Data.Num());
        }
    };

    auto CompletionCallback = []() {
        FScopeLock CallbackLock(&CallbackMutex);
        if (CurrentDownloadComplete && CurrentDownloadSuccess) {
            *CurrentDownloadSuccess = true;
            *CurrentDownloadComplete = true;
            UE_LOG(LogJUSYNC, Log, TEXT("All parallel downloads completed"));
        }
    };

    auto ErrorCallback = [](const char* filename, const char* error_message) {
        FScopeLock CallbackLock(&CallbackMutex);
        FString UE_Filename = UTF8_TO_TCHAR(filename);
        FString UE_Error = UTF8_TO_TCHAR(error_message);
        UE_LOG(LogJUSYNC, Error, TEXT("Parallel download error for %s: %s"), *UE_Filename, *UE_Error);
        if (CurrentDownloadComplete && CurrentDownloadSuccess) {
            *CurrentDownloadSuccess = false;
            *CurrentDownloadComplete = true;
        }
    };

    try
    {
        RequestFilesParallelAsync_C(
            FilenameCStrs.data(),
            FilenameCStrs.size(),
            TargetRanksArray.data(),
            FileReceivedCallback,
            CompletionCallback,
            ErrorCallback,
            TimeoutMs
        );

        UE_LOG(LogJUSYNC, Log, TEXT("RequestFilesParallelAsync_C called - downloads running in background"));


        if (TimeoutMs == 0)
        {
            UE_LOG(LogJUSYNC, Log, TEXT("Pipeline mode: returning immediately, callbacks will process files as they arrive"));
            return true;
        }
    }
    catch (const std::exception& e)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Exception in parallel download: %s"), UTF8_TO_TCHAR(e.what()));
        return false;
    }
    catch (...)
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Unknown exception in parallel download"));
        return false;
    }

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
            UE_LOG(LogJUSYNC, Log, TEXT("Downloaded %d files in parallel"), OutFiles.Num());
            return true;
        }
        else
        {
            UE_LOG(LogJUSYNC, Error, TEXT("Failed to download files in parallel (received %d/%d files)"),
                   FilesReceived.load(), FilesExpected.load());
            return false;
        }
    }
    else
    {
        return true;
    }

#else
    UE_LOG(LogJUSYNC, Error, TEXT("Parallel downloads not available - middleware not compiled"));
    return false;
#endif
}

bool UJUSYNCSubsystem::RequestFrame(int32 FrameNumber, int32 TargetRank, int32 TimeoutMs, TArray<FJUSYNCFileData>& OutFiles)
{

    UE_LOG(LogJUSYNC, Log, TEXT("Requesting frame from broker"));
    UE_LOG(LogJUSYNC, Log, TEXT("Frame Number: %d"), FrameNumber);
    UE_LOG(LogJUSYNC, Log, TEXT("Target Rank: %d"), TargetRank);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request frame - middleware not initialized"));
        return false;
    }

    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request frame - not connected to broker"));
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

        FreeFrameFiles_C(CFrameFiles, FileCount);

        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved frame %d with %d files from broker"), FrameNumber, OutFiles.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to request frame (Result: %d)"), Result);
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

    UE_LOG(LogJUSYNC, Log, TEXT("Requesting worker status from broker"));
    UE_LOG(LogJUSYNC, Log, TEXT("Target Rank: %d"), TargetRank);
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request worker status - middleware not initialized"));
        return false;
    }

    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request worker status - not connected to broker"));
        return false;
    }

    g_CapturedWorkers.Empty();
    int bResult = RequestWorkerListStringCallback_C(WorkerListCallback, TimeoutMs);

    if (bResult == 1 && g_CapturedWorkers.Num() > 0)
    {
        OutWorkerStatus = MoveTemp(g_CapturedWorkers);
        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved worker list: %d workers"), OutWorkerStatus.Num());
        return true;
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve worker list from middleware"));
        OutWorkerStatus.Empty();
        return false;
    }
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestWorkerCount(int32 TimeoutMs, int32& OutWorkerCount)
{

    UE_LOG(LogJUSYNC, Log, TEXT("Requesting worker count from broker"));
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request worker count - middleware not initialized"));
        return false;
    }

    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request worker count - not connected to broker"));
        return false;
    }

    g_CapturedWorkers.Empty();
    int bResult = RequestWorkerListStringCallback_C(WorkerListCallback, TimeoutMs);

    if (bResult == 1)
    {
        OutWorkerCount = g_CapturedWorkers.Num();
        UE_LOG(LogJUSYNC, Log, TEXT("Retrieved worker count: %d workers"), OutWorkerCount);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve worker count from middleware"));
        OutWorkerCount = 0;
    }

    return bResult == 1;
#endif
    return false;
}

bool UJUSYNCSubsystem::RequestTotalWorkerCount(int32 TimeoutMs, int32& OutTotalCount)
{

    UE_LOG(LogJUSYNC, Log, TEXT("Requesting total worker count (including rank 0)"));
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request total worker count - middleware not initialized"));
        return false;
    }

    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request total worker count - not connected to broker"));
        return false;
    }

    uint32_t TotalCount = 0;
    int Result = RequestTotalWorkerCount_C(&TotalCount, TimeoutMs);

    if (Result == 1)
    {
        OutTotalCount = static_cast<int32>(TotalCount);
        UE_LOG(LogJUSYNC, Log, TEXT("Total worker count (including rank 0): %d"), OutTotalCount);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve total worker count"));
        OutTotalCount = 0;
    }

    return Result == 1;
#endif
    return false;
}


bool UJUSYNCSubsystem::RequestWorkerCountExcludingRank0(int32 TimeoutMs, int32& OutWorkerCount)
{

    UE_LOG(LogJUSYNC, Log, TEXT("Requesting worker count (excluding rank 0)"));
    UE_LOG(LogJUSYNC, Log, TEXT("Timeout: %d ms"), TimeoutMs);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitialized.load())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request worker count - middleware not initialized"));
        return false;
    }

    if (!IsBrokerConnected())
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Cannot request worker count - not connected to broker"));
        return false;
    }

    uint32_t WorkerCount = 0;
    int Result = RequestWorkerCountExcludingRank0_C(&WorkerCount, TimeoutMs);

    if (Result == 1)
    {
        OutWorkerCount = static_cast<int32>(WorkerCount);
        UE_LOG(LogJUSYNC, Log, TEXT("Worker count (excluding rank 0): %d"), OutWorkerCount);
    }
    else
    {
        UE_LOG(LogJUSYNC, Error, TEXT("Failed to retrieve worker count"));
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

    TWeakObjectPtr<UTexture2D> TexturePtr = Texture;
    TWeakObjectPtr<URealtimeMeshComponent> ComponentPtr = TargetComponent;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, TexturePtr, ComponentPtr]()
    {
        UMaterialInterface* BaseMaterial = GetCachedMaterial(TEXT("/Game/Materials/M_BaseMaterial"));
        if (!BaseMaterial)
        {
            BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
        }

        AsyncTask(ENamedThreads::GameThread, [this, TexturePtr, ComponentPtr, BaseMaterial]()
        {
            if (!TexturePtr.IsValid() || !ComponentPtr.IsValid() || !BaseMaterial)
            {
                UE_LOG(LogJUSYNC, Error, TEXT("CreateMaterialFromTexture_Async: Objects no longer valid"));
                return;
            }

            UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, nullptr);
            if (DynamicMaterial)
            {
                DynamicMaterial->SetTextureParameterValue(TEXT("BaseColor"), TexturePtr.Get());

                if (URealtimeMeshComponent* Component = ComponentPtr.Get())
                {
                    Component->SetMaterial(0, DynamicMaterial);
                    UE_LOG(LogJUSYNC, Log, TEXT("Applied dynamic material to mesh component"));
                }
            }
            else
            {
                UE_LOG(LogJUSYNC, Error, TEXT("Failed to create dynamic material instance"));
            }
        });
    });
}


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


void UJUSYNCSubsystem::CollectMetrics()
{
    if (!bMetricsCollectionActive.load()) return;

    FScopeLock Lock(&MetricsMutex);
    UpdateMetricsData();
    SaveMetricsToHistory();
}

void UJUSYNCSubsystem::UpdateMetricsData()
{
    CurrentMetrics.Timestamp = FDateTime::Now();

    CurrentMetrics.SystemRAM_Used_GB = GetSystemRAMUsage_GB();
    CurrentMetrics.VRAM_Used_GB = GetVRAMUsage_GB();
    CurrentMetrics.CPUUsage_Percent = GetCPUUsage_Percent();
    CurrentMetrics.GPUUsage_Percent = GetGPUUsage_Percent();

    CurrentMetrics.TotalMeshesProcessed = MetricsAccumulator.MeshSplitCount + MetricsAccumulator.MeshCreationCount;
    CurrentMetrics.SplitMeshes = MetricsAccumulator.MeshSplitCount;
    CurrentMetrics.TotalChunksCreated = MetricsAccumulator.TotalChunksCreated;

    if (MetricsAccumulator.MeshSplitCount > 0)
    {
        CurrentMetrics.AverageSplitTime_ms = MetricsAccumulator.TotalSplitTime_ms / MetricsAccumulator.MeshSplitCount;
        CurrentMetrics.AverageChunksPerSplit = static_cast<float>(MetricsAccumulator.TotalChunksCreated) / MetricsAccumulator.MeshSplitCount;
    }

    if (MetricsAccumulator.MeshCreationCount > 0)
    {
        CurrentMetrics.MeshProcessingTime_ms = MetricsAccumulator.TotalCreationTime_ms / MetricsAccumulator.MeshCreationCount;
    }

    CurrentMetrics.TotalErrors = MetricsAccumulator.ErrorCount;
    CurrentMetrics.MemoryWarnings = MetricsAccumulator.MemoryWarningCount;

    CurrentMetrics.TotalRMCComponents = CountRMCComponents();
    CurrentMetrics.ActiveComponents = CountActiveRMCComponents();
    CurrentMetrics.InstancedComponents = CountInstancedRMCComponents();

    MetricsAccumulator.Reset();
}

void UJUSYNCSubsystem::SaveMetricsToHistory()
{
    MetricsHistory.Add(CurrentMetrics);

    if (MetricsHistory.Num() > MetricsConfig.MaxHistorySize)
    {
        MetricsHistory.RemoveAt(0, MetricsHistory.Num() - MetricsConfig.MaxHistorySize);
    }

    OnMetricsUpdated.Broadcast(CurrentMetrics);
}


float UJUSYNCSubsystem::GetSystemRAMUsage_GB() const
{
    return 0.0f;
}

float UJUSYNCSubsystem::GetVRAMUsage_GB() const
{

    static int64 vramCounter = 1 * 1024 * 1024 * 1024; // 1GB

    vramCounter += 200 * 1024 * 1024;

    return vramCounter / (1024.0f * 1024.0f * 1024.0f);
}

float UJUSYNCSubsystem::GetCPUUsage_Percent() const
{


    static float simulatedCPU = 20.0f;

    simulatedCPU = 15.0f + FMath::FRand() * 30.0f; // Between 15% and 45%

    return FMath::Clamp(simulatedCPU, 0.0f, 100.0f);
}

float UJUSYNCSubsystem::GetGPUUsage_Percent() const
{

    static float SimulatedGPUUsage = 0.0f;

    float VRAMUsageGB = GetVRAMUsage_GB();
    float VRAMBasedUsage = FMath::Clamp(VRAMUsageGB / 8.0f * 100.0f, 0.0f, 100.0f);

    float MeshBasedUsage = FMath::Clamp(
        (MetricsAccumulator.MeshCreationCount + MetricsAccumulator.MeshSplitCount) * 0.5f,
        0.0f, 50.0f
    );

    static float RandomOffset = 0.0f;
    if (FMath::RandBool())
    {
        RandomOffset = FMath::FRandRange(-5.0f, 5.0f);
    }

    SimulatedGPUUsage = FMath::Clamp(VRAMBasedUsage + MeshBasedUsage + RandomOffset, 0.0f, 100.0f);

    return SimulatedGPUUsage;
}


int32 UJUSYNCSubsystem::CountRMCComponents() const
{
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
    int32 Count = 0;
    if (UWorld* World = GetWorld())
    {
        for (TObjectIterator<URealtimeMeshComponent> It; It; ++It)
        {
            if (It->GetWorld() == World)
            {
                FString ComponentName = It->GetName();

                if (ComponentName.Contains(TEXT("Chunk")) ||
                    ComponentName.Contains(TEXT("Split")) ||
                    ComponentName.Contains(TEXT("Part")) ||
                    ComponentName.Contains(TEXT("Segment")) ||
                    ComponentName.Contains(TEXT("Slice")) ||
                    ComponentName.Contains(TEXT("_C")) ||  // Common suffix for chunks
                    (ComponentName.Contains(TEXT("_")) && ComponentName.Contains(TEXT("of")))) // "Mesh_1_of_4"
                {
                    Count++;
                }
            }
        }
    }
    return Count;
}
