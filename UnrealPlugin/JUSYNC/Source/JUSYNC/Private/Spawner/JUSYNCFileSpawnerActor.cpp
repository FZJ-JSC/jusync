#include "JUSYNCFileSpawnerActor.h"
#include <atomic>
#include "JUSYNCSubsystem.h"
#include "RawGeo/JUSYNCRawGeoCurves.h"
#include "JUSYNCPointCloudSpawner.h"
#include "JUSYNCBenchmarkTiming.h"
#include "Platform/JUSYNCVideoMemory.h"
#include "JUSYNCUSDLoader.h"
#include "EngineUtils.h"
#if WITH_ANARI_USD_MIDDLEWARE
#include "AnariUsdMiddleware_C.h"
#include "LidarPointCloudActor.h"
#endif
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "TimerManager.h"
#include "Components/BoxComponent.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterfaceArrayFunctionLibrary.h"
#include "NiagaraSystem.h"

#if !WITH_ANARI_USD_MIDDLEWARE
extern "C"
{
int IsBrokerConnected_C(void);
int RequestTotalWorkerCount_C(unsigned int* out_total_count, int timeout_ms);
int RequestFile_C(const char* filename, int32_t target_rank, unsigned char** out_data, size_t* out_size, int timeout_ms);
void FreeBuffer_C(unsigned char* buffer);
}
#endif

static bool JUSYNCRequestFileToString(const FString& Filename, int32 Rank, FString& OutBody)
{
    unsigned char* Data = nullptr;
    size_t Size = 0;
    const int rc = RequestFile_C(TCHAR_TO_UTF8(*Filename), Rank, &Data, &Size, 8000);
    if (rc != 1 || !Data || Size == 0)
    {
        if (Data)
        {
            FreeBuffer_C(Data);
        }
        return false;
    }

    TArray<ANSICHAR> Chars;
    Chars.SetNumUninitialized(static_cast<int32>(Size) + 1);
    FMemory::Memcpy(Chars.GetData(), Data, Size);
    Chars[static_cast<int32>(Size)] = 0;
    FreeBuffer_C(Data);

    OutBody = FString(Chars.GetData());
    return !OutBody.IsEmpty();
}
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Kismet/GameplayStatics.h"
#include "Templates/UniquePtr.h"
#include "RealtimeMeshComponent.h"
#include "RealtimeMeshSimple.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialParameterCollection.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Async/Async.h"
#include "Tasks/Task.h"
#include "JUSYNCElementNames.h"

static TAutoConsoleVariable<int32> CVarJusyncCollapseVariants(
    TEXT("jusync.Spawner.CollapseVariants"), 0,
    TEXT("Collapse VTK/ParaView display-rebuild duplicates (\"<name>_u<k>\") onto one\n")
    TEXT("logical element, keeping the newest variant. Off (default) accepts every\n")
    TEXT("streamed variant and subset: the client reuses one actor per logical element,\n")
    TEXT("so alternating _<n>/_u<k> notifications update the same mesh instead of\n")
    TEXT("leaving an older sample visible. Turning this on can suppress subset (.geo\n")
    TEXT("_s<k>) streams and freeze part of a cloud."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncPathlineMaxTracks(
    TEXT("jusync.Pathlines.MaxTracks"), 0,
    TEXT("Overrides PathlineMaxTracks on every spawner when > 0."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncPathlineHistorySamples(
    TEXT("jusync.Pathlines.HistorySamples"), 0,
    TEXT("Overrides PathlineHistorySamples on every spawner when > 0."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncPathlineMaxNodes(
    TEXT("jusync.Pathlines.MaxNodes"), 0,
    TEXT("Overrides PathlineMaxNodes on every spawner when > 0. This is the\n")
    TEXT("hard cap on trail geometry nodes (tracks x history) and the main\n")
    TEXT("memory/GPU guard for dense streams."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarJusyncPathlineRadius(
    TEXT("jusync.Pathlines.Radius"), 0.0f,
    TEXT("Overrides PathlineRadius (cm) on every spawner when > 0, so trail\n")
    TEXT("thickness can be tuned live from the console. 0 = use the spawner's\n")
    TEXT("details-panel value (the default: the actor property wins)."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarJusyncPathlineGlow(
    TEXT("jusync.Pathlines.Glow"), 0.1f,
    TEXT("Emissive strength of the pathline ribbons: 0 keeps them matte and\n")
    TEXT("scene-lit only, 20+ makes them bloom. Ribbons are one shared\n")
    TEXT("material, so the value is applied to MPC_Jusync.PathlineGlow and\n")
    TEXT("every trail picks it up. The in-game GLOW slider drives this."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJusyncPathlineTubeSides(
    TEXT("jusync.Pathlines.TubeSides"), 8,
    TEXT("Legacy: trail geometry is now tessellated on the GPU by the Niagara\n")
    TEXT("ribbon renderer, so tube sides no longer change the geometry. Kept so\n")
    TEXT("the in-game pathline panel's SIDES control stays harmless."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarJusyncDeletedFileGrace(
    TEXT("jusync.Spawner.DeletedFileGraceSeconds"), 5.0f,
    TEXT("Seconds to hold actors owned by a deleted clip file before\n")
    TEXT("deciding, so a rollover to the next timestep's filename can\n")
    TEXT("take the actor over. After it expires the last frame is HELD, not\n")
    TEXT("destroyed: the sender's store deletes streamed clips during its own\n")
    TEXT("reconciliation (always on the final commit of a stream), so a clip\n")
    TEXT("name vanishing without a successor is not a delete signal."),
    ECVF_Default);

static bool JUSYNCIsLiveUpdateGeometryFile(const FString& Filename)
{
    if (Filename.EndsWith(TEXT(".geo")))
    {
        return Filename.StartsWith(TEXT("clips/")) &&
               !Filename.Contains(TEXT("manifest"));
    }

    const bool bAnyUsdExt =
        Filename.EndsWith(TEXT(".usda")) ||
        Filename.EndsWith(TEXT(".usd")) ||
        Filename.EndsWith(TEXT(".usdc")) ||
        Filename.EndsWith(TEXT(".usdz"));
    if (!bAnyUsdExt)
    {
        return false;
    }
    if (Filename.StartsWith(TEXT("Session_")) ||
        Filename.Contains(TEXT("manifest")) ||
        Filename.Contains(TEXT("images/")) ||
        Filename.Contains(TEXT("shared/")) ||
        Filename.Contains(TEXT("primstages/")) ||
        Filename.Contains(TEXT("_Light.")) ||
        Filename.Contains(TEXT("_Material.")) ||
        Filename.Contains(TEXT("_Camera.")) ||
        Filename.Contains(TEXT("_Sampler.")))
    {
        return false;
    }
    for (const TCHAR* Ext : { TEXT(".usda"), TEXT(".usd"), TEXT(".usdc"), TEXT(".usdz") })
    {
        if (Filename == FString(TEXT("scene")) + Ext)
        {
            return false;
        }
    }
    return true;
}

static bool JUSYNCIsSubsetStreamFile(const FString& Filename)
{
    if (!Filename.EndsWith(TEXT(".geo"), ESearchCase::IgnoreCase))
    {
        return false;
    }

    const FString Base = FPaths::GetBaseFilename(FPaths::GetCleanFilename(Filename));
    const int32 TimeSep = Base.Find(TEXT("__"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (TimeSep == INDEX_NONE)
    {
        return false;
    }

    const FString Tail = Base.Mid(TimeSep + 2);
    const int32 SubSep = Tail.Find(TEXT("_s"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (SubSep == INDEX_NONE)
    {
        return false;
    }

    const FString Index = Tail.Mid(SubSep + 2);
    if (Index.IsEmpty())
    {
        return false;
    }
    for (const TCHAR C : Index)
    {
        if (!FChar::IsDigit(C))
        {
            return false;
        }
    }
    return true;
}

static FString JUSYNCBuildLogicalActorKey(const FString& Filename)
{
    const JUSYNCElementNames::FVariant Variant = JUSYNCElementNames::Parse(Filename);
    if (Variant.Element.IsEmpty())
    {
        return FString();
    }

    FString TimeMark;
    FString Base = FPaths::GetBaseFilename(FPaths::GetCleanFilename(Filename));
    if (Filename.EndsWith(TEXT(".geo"), ESearchCase::IgnoreCase))
    {
        Base = JUSYNCRawGeoCurves::StripSubsetSuffix(FPaths::GetCleanFilename(Filename));
    }
    const int32 TimeSep = Base.Find(TEXT("__"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (TimeSep != INDEX_NONE)
    {
        TimeMark = Base.Mid(TimeSep + 2);
    }

    const TCHAR* Payload = (Variant.Payload == JUSYNCElementNames::EPayload::RawGeo)
        ? TEXT("geo")
        : (Variant.Payload == JUSYNCElementNames::EPayload::Usd) ? TEXT("usd") : TEXT("other");
    const FString StableElement = JUSYNCElementNames::CanonicalActorIdentity(Variant.Element);
    return FString::Printf(TEXT("%s@%s@%s"), *StableElement, Payload, *TimeMark);
}

FString AJUSYNCFileSpawnerActor::BuildClipStreamKey(const FString& Filename)
{
    const FString Logical = JUSYNCBuildLogicalActorKey(Filename);
    int32 Cut = INDEX_NONE;
    if (Logical.IsEmpty() || !Logical.FindLastChar(TEXT('@'), Cut) ||
        Cut == Logical.Len() - 1)
    {
        return FString();
    }
    return Logical.Left(Cut);
}

int64 AJUSYNCFileSpawnerActor::ComputeCacheBudgetBytes(int64 FreeBytes, int32 Percent, int64 FloorBytes, int64 CapBytes)
{
    const int64 Floor = FMath::Max<int64>(1, FloorBytes);
    const int64 Cap = FMath::Max(Floor, CapBytes);
    if (Percent <= 0 || FreeBytes <= 0)
    {
        return Floor;
    }
    const int64 Desired = FreeBytes / 100 * FMath::Clamp(Percent, 1, 100);
    return FMath::Clamp(Desired, Floor, Cap);
}

bool AJUSYNCFileSpawnerActor::PassesBudgetHysteresis(int64 LastAccepted, int64 Value, int32 PercentToAct)
{
    if (LastAccepted <= 0)
    {
        return Value > 0;
    }
    const int64 Delta = Value > LastAccepted ? Value - LastAccepted : LastAccepted - Value;
    return Delta * 100 > LastAccepted * FMath::Clamp(PercentToAct, 1, 90);
}

int64 AJUSYNCFileSpawnerActor::GetSampleCacheBudgetBytes() const
{
    const int64 FloorBytes = static_cast<int64>(FMath::Max(16, SampleCacheMaxMB)) * 1024 * 1024;
    if (!bDynamicCacheBudget || DynamicSampleCacheBytes <= 0)
    {
        return FloorBytes;
    }
    return FMath::Max(FloorBytes, DynamicSampleCacheBytes);
}

int32 AJUSYNCFileSpawnerActor::GetSampleCacheCountCap() const
{
    const int32 StaticCap = FMath::Clamp(SampleCacheSize, 1, 64);
    if (!bDynamicCacheBudget || DynamicSampleCacheBytes <= 0)
    {
        return StaticCap;
    }
    const int64 AvgBytes = SampleCacheEntryCount > 0
        ? FMath::Max<int64>(1, SampleCacheBytes / SampleCacheEntryCount) : 0;
    if (AvgBytes <= 0)
    {
        return StaticCap;
    }
    return static_cast<int32>(FMath::Clamp<int64>(
        DynamicSampleCacheBytes / AvgBytes, StaticCap, 1024));
}

void AJUSYNCFileSpawnerActor::PollDynamicCacheBudgets(float DeltaSeconds)
{
    if (!bDynamicCacheBudget)
    {
        if (DynamicSampleCacheBytes != 0 || DynamicCloudBudgetBytes != 0)
        {
            DynamicSampleCacheBytes = 0;
            DynamicCloudBudgetBytes = 0;
            if (UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
            {
                if (FJUSYNCPointCloudSpawner* PCSpawner = Subsystem->GetPointCloudSpawner())
                {
                    PCSpawner->SetWarmCloudBudgetBytes(0);  // legacy fixed pool
                }
            }
        }
        CacheBudgetAccum = 0.0f;
        return;
    }

    CacheBudgetAccum += DeltaSeconds;
    if (CacheBudgetAccum < 2.0f)
    {
        return;
    }
    CacheBudgetAccum = 0.0f;

    const FPlatformMemoryStats Mem = FPlatformMemory::GetStats();
    const int64 FreeRamBytes = static_cast<int64>(Mem.AvailablePhysical);
    const int64 FloorBytes = static_cast<int64>(FMath::Max(16, SampleCacheMaxMB)) * 1024 * 1024;
    const int64 CapBytes = static_cast<int64>(FMath::Max(64, CacheBudgetMaxMB)) * 1024 * 1024;
    const int64 RamBudget = ComputeCacheBudgetBytes(FreeRamBytes, CacheRamPercent, FloorBytes, CapBytes);
    const int64 AcceptedRam = DynamicSampleCacheBytes > 0 ? DynamicSampleCacheBytes : FloorBytes;
    if (RamBudget != DynamicSampleCacheBytes && PassesBudgetHysteresis(AcceptedRam, RamBudget))
    {
        DynamicSampleCacheBytes = RamBudget;
        UE_LOG(LogTemp, Display, TEXT(
            "[Spawner] sample-cache budget -> %lld MB (free RAM %lld MB x %d%%, floor %d MB, cap %d MB)"),
            RamBudget / (1024 * 1024), FreeRamBytes / (1024 * 1024),
            CacheRamPercent, FMath::Max(16, SampleCacheMaxMB), FMath::Max(64, CacheBudgetMaxMB));
        TrimSampleCache();

        if (MeshCache && bEnableMeshCache)
        {
            const int64 MeshFloorBytes = static_cast<int64>(FMath::Max(16, MeshCacheMaxMB)) * 1024 * 1024;
            MeshCache->SetLimits(MeshCacheMaxEntries,
                ComputeCacheBudgetBytes(FreeRamBytes, CacheRamPercent, MeshFloorBytes, CapBytes));
        }
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr;
    if (!PCSpawner)
    {
        return;
    }
    int64 FreeVRamBytes = 0;
    int64 TotalVRamBudgetBytes = 0;
    if (!JUSYNCVideoMemory::QueryProcessLocalBudget(FreeVRamBytes, TotalVRamBudgetBytes))
    {
        return;
    }
    const int64 WarmFloorBytes = 64LL * 1024 * 1024;
    const int64 WarmCapBytes = 2048LL * 1024 * 1024;
    const int64 WarmBudget = ComputeCacheBudgetBytes(FreeVRamBytes, CacheVRamPercent, WarmFloorBytes, WarmCapBytes);
    const int64 AcceptedVram = DynamicCloudBudgetBytes > 0 ? DynamicCloudBudgetBytes : WarmFloorBytes;
    if (WarmBudget != DynamicCloudBudgetBytes && PassesBudgetHysteresis(AcceptedVram, WarmBudget))
    {
        DynamicCloudBudgetBytes = WarmBudget;
        PCSpawner->SetWarmCloudBudgetBytes(WarmBudget);
        UE_LOG(LogTemp, Display, TEXT(
            "[Spawner] warm-cloud pool budget -> %lld MB (free VRAM budget %lld MB x %d%%)"),
            WarmBudget / (1024 * 1024), FreeVRamBytes / (1024 * 1024), CacheVRamPercent);
    }
}

static FString JUSYNCCanonicalPointCloudElement(const FString& ElementName)
{
    const JUSYNCElementNames::FVariant Variant = JUSYNCElementNames::ParseElement(ElementName);
    const FString Base = (!Variant.Element.IsEmpty() && Variant.Uniq >= 0) ? Variant.Element : ElementName;
    return JUSYNCElementNames::CanonicalActorIdentity(Base);
}

static FString JUSYNCCanonicalFromRankedElementKey(const FString& Key)
{
    const int32 RankMark = Key.Find(TEXT("_r"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (RankMark == INDEX_NONE)
    {
        return Key;
    }
    const FString RankText = Key.Mid(RankMark + 2);
    if (RankText.IsEmpty())
    {
        return Key;
    }
    for (const TCHAR C : RankText)
    {
        if (!FChar::IsDigit(C))
        {
            return Key;
        }
    }
    return Key.Left(RankMark);
}

static double JUSYNCDecodeSenderTime(uint64 RawTimestamp)
{
    if (RawTimestamp > 0 && RawTimestamp < 1000000000ULL)
    {
        return static_cast<double>(RawTimestamp) / 1000.0;
    }
    return -1.0;
}

static double JUSYNCTimeFromFilename(const FString& Filename)
{
    const FString Clean = FPaths::GetCleanFilename(Filename);
    const FString Base = Clean.EndsWith(TEXT(".geo"), ESearchCase::IgnoreCase)
        ? JUSYNCRawGeoCurves::StripSubsetSuffix(Clean)
        : FPaths::GetBaseFilename(Clean);
    const int32 TimeSep = Base.Find(TEXT("__"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (TimeSep == INDEX_NONE)
    {
        return -1.0;
    }

    const FString TimeText = Base.Mid(TimeSep + 2);
    return TimeText.IsNumeric() ? FCString::Atod(*TimeText) : -1.0;
}

static bool JUSYNCMaterialUsesVertexColor(UMaterialInterface* Material)
{
    if (!Material)
    {
        return false;
    }

#if WITH_EDITORONLY_DATA
    const UMaterial* Mat = Cast<UMaterial>(Material->GetMaterial());
    if (!Mat)
    {
        return false;
    }

    return Mat->HasAnyExpressionsInMaterialAndFunctionsOfType<UMaterialExpressionVertexColor>();
#else
    return false;
#endif
}

static bool JUSYNCFindColorTextureParameter(UMaterialInterface* Material, FName& OutParameterName)
{
    OutParameterName = NAME_None;
    if (!Material)
    {
        return false;
    }

    const UMaterial* Mat = Cast<UMaterial>(Material->GetMaterial());
    if (!Mat)
    {
        return false;
    }

    TArray<FMaterialParameterInfo> Params;
    TArray<FGuid> ParamIds;
    Material->GetAllTextureParameterInfo(Params, ParamIds);
    if (Params.Num() == 0)
    {
        return false;
    }

    auto IsNonColorTextureParam = [](FName Name)
    {
        const FString S = Name.ToString();
        return S.Contains(TEXT("Normal")) ||
            S.Contains(TEXT("Rough")) ||
            S.Contains(TEXT("Metal")) ||
            S.Contains(TEXT("Opacity")) ||
            S.Contains(TEXT("Mask")) ||
            S.Contains(TEXT("AO")) ||
            S.Contains(TEXT("AmbientOcclusion"));
    };

    static const FName PreferredParams[] = {
        TEXT("BaseColor"),
        TEXT("BaseTexture"),
        TEXT("DiffuseColor"),
        TEXT("Albedo"),
        TEXT("Color"),
        TEXT("EmissiveColor")
    };

    for (const FName Preferred : PreferredParams)
    {
        for (const FMaterialParameterInfo& Param : Params)
        {
            if (Param.Name == Preferred && !IsNonColorTextureParam(Param.Name))
            {
                OutParameterName = Param.Name;
                return true;
            }
        }
    }

    for (const FMaterialParameterInfo& Param : Params)
    {
        if (!IsNonColorTextureParam(Param.Name))
        {
            OutParameterName = Param.Name;
            return true;
        }
    }

    return false;
}

AJUSYNCFileSpawnerActor::AJUSYNCFileSpawnerActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;

    BrokerEndpoint = TEXT("tcp://localhost:5556");
    RequestTimeoutMs = 5000;
    BandwidthBytesPerSecond = 10737418240.0f;
    MinimumFileSizeBytes = 1000;
    bFilterUSDOnly = true;
    bClipsOnly = true;
    SpawnTargetActor = nullptr;
    BaseSpawnLocation = FVector::ZeroVector;
    SpawnSpacing = 0.0f;
    SpawnGridColumns = 10;
    SpawnMaterial = nullptr;
    TextureSampleParameterName = TEXT("");
    bMeshTextureLoading = false;
    bMeshTextureReady = false;
    SpawnScale = FVector::OneVector;
    bUseUniformScaling = true;

    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    FitVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("FitVolume"));
    FitVolume->SetupAttachment(RootComponent);
    FitVolume->SetBoxExtent(FVector(200.0));
    FitVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    FitVolume->SetHiddenInGame(true);
    FitVolume->ShapeColor = FColor::Cyan;
    bFitToVolume = false;
    FitMode = EJUSYNCFitMode::Uniform;
    FitPaddingFraction = 0.05f;
    bCenterInVolume = true;
    bAutoStart = false;
    bEnableLiveUpdates = true;
    LiveUpdatePollInterval = 20.0f;
    bAutoRefreshMeshes = true;
    LastCommitCompleteTime = 0.0;
    CommitCompleteCooldownSeconds = 0.15f;
    bSceneDiffInFlight = false;
    bCommitDiffPending = false;
    bInitialSpawnDone = false;

    bMorphEnabled = true;
    MeshBlendMs = 0;
    MeshMaxBlendMs = 2000;
    CloudSteps = 4;  // 0 = legacy instant swap
    CloudBlendMs = 0;
    CloudMaxBlendMs = 200;
    bCloudFixedSize = true;
    CloudScreenSizePct = 2.7f;

    bEnableBenchmarking = true;
    BenchmarkTestName = TEXT("JUSYNC_SpawnerSession");
    BenchmarkOutputDirectory = TEXT("");
    BenchmarkOutputFormat = 2;
    bBenchmarkAppendTimestamp = true;
    BenchmarkInteractiveWindowSeconds = 5.0f;
    bBenchmarkColdStart = true;
    bExportClientBenchmarkReport = true;
    bIncludeClusterBenchmark = true;
    bBenchmarkSessionActive = false;
    bBenchmarkWindowScheduled = false;
    bBenchmarkFinalized = false;
    bSpawnPointClouds = true;
    bUseGradientColors = true;
    GradientPngFilename = TEXT("");

    bEnablePathlines = false;
    PathlineMaxTracks = 1000;
    PathlineHistorySamples = 128;
    PathlineMaxNodes = 524288;
    PathlineTrackExpirySamples = 64;
    PathlineNiagaraSystem = nullptr;
    PathlineRadius = 1.0f;
    PathlineRadiusScale = 1.0f;
    bPathlineUsePointWidth = false;
    PathlineIdAttribute = TEXT("particle_id");
    PathlineScalarAttribute = TEXT("attribute0");
    PathlineScalarComponent = 0;
    bPathlineFilterEnabled = false;
    PathlineFilterMin = 0.0f;
    PathlineFilterMax = 1.0f;
    bPathlineHueFilterEnabled = false;
    PathlineHue = 0.0f;
    PathlineHueTolerance = 30.0f;
    bPathlineColorByScalar = true;
    PathlineColorMin = 0.0f;
    PathlineColorMax = 1.0f;
    PathlineFlatColor = FLinearColor::White;
    PathlineEmissiveStrength = 0.05f;
    PathlineSelectionSeed = 1337;
    PathlinePendingTimeoutSeconds = 2.0f;
    PathlineMaterial = nullptr;
    PathlineActiveTracks = 0;
    PathlineNodeCount = 0;
    PathlineVisibleTracks = 0;
    PathlineDroppedSamples = 0;
    PathlineIgnoredSamples = 0;
    PointCloudSize = 0.1f;
    bPointCloudsVisible = true;
    bPointCloudHueFilterEnabled = false;
    PointCloudHue = 0.0f;
    PointCloudHueTolerance = 30.0f;
    PointShape = EJUSYNCPointShape::Square;
    PointOrientation = EJUSYNCPointOrientation::FacingCamera;
    PointScaling = EJUSYNCPointScaling::FixedScreenSize;
    PointSizeBias = 0.012f;
    GapFillingStrength = 0.0f;
    PointCloudPoolSize = 16;
    bCalculatePointCloudNormals = false;
    PointCloudNormalsMaxPoints = 1000000;
    PointCloudNormalsQuality = 10;
    PointCloudNormalsNoiseTolerance = 0.05f;
    PointCloudNormalsCooldownSeconds = 2.0f;
    bGradientReady = false;
    bGradientDownloadStarted = false;
    bGradientAttempted = false;
    PipelineDepth = 10;
    bEnablePerfLogging = false;
    MaxRetries = 2;
    CurrentRetryCount = 0;
    CurrentState = EJUSYNCSpawnerState::Idle;
    FilesDownloaded = 0;
    FilesTotal = 0;
    ActorsSpawned = 0;
    NextSpawnIndex = 0;
    PendingDownloads = 0;
    PendingAsyncSpawns = 0;
    PendingAsyncPCS = 0;
    PendingParseTasks = 0;
    PipelineNextIndex = 0;
    PipelineActive = 0;
    DownloadStallSince = 0.0;
    DownloadWatchdogAcc = 0.0f;
    bIsCancelled = false;
    MaxSpawnsPerFrame = 3;
    bCollapseElementVariants = false;

    bEnableMeshCache = true;
    MeshCacheMaxEntries = 64;
    MeshCacheMaxMB = 2048;
    bCachePointClouds = true;

    bEnableSampleCache = true;
    SampleCacheSize = 8;
    SampleCacheMaxMB = 512;
    bPauseLiveApplyDuringPlayback = true;
    PlaybackPosition = 1.0f;
    CachedSampleCount = 0;
    PlaybackSampleIndex = 0;
    bSamplePlaybackActive = false;

    bEnableTimeStepAnimation = true;
    TimeStepPlaybackFPS = 1.0f;
    bLoopTimeStepAnimation = true;

    MeshCache = MakeUnique<FJUSYNCMeshCache>();
    ChangeTracker = MakeUnique<FJUSYNCFileChangeTracker>();
    AnimationController = MakeUnique<FJUSYNCAnimationController>();
}

void AJUSYNCFileSpawnerActor::MaybeStartBenchmarkSession()
{
    if (!bEnableBenchmarking || bBenchmarkSessionActive)
    {
        return;
    }

    FJUSYNCBenchmarkConfig Config;
    Config.bEnableBenchmarking = true;
    Config.OutputDirectory = BenchmarkOutputDirectory.IsEmpty()
        ? FPaths::Combine(FPaths::ProjectDir(), TEXT("Benchmark"))
        : BenchmarkOutputDirectory;
    Config.OutputFormat = BenchmarkOutputFormat;
    Config.bAppendTimestamp = bBenchmarkAppendTimestamp;
    Config.InteractiveWindowSeconds = BenchmarkInteractiveWindowSeconds;
    Config.bColdStart = bBenchmarkColdStart;

    UJUSYNCBlueprintLibrary::StartBenchmark(BenchmarkTestName, Config);

    bBenchmarkSessionActive = true;
    bBenchmarkWindowScheduled = false;
    bBenchmarkFinalized = false;
    BenchmarkSessionWallStart = FPlatformTime::Seconds();
    BenchmarkRAMStartBytes = static_cast<int64>(FPlatformMemory::GetStats().UsedPhysical);
    BenchmarkVRAMStartBytes = UJUSYNCBlueprintLibrary::QueryVRAMUsageBytes();
    BenchmarkSceneVertices = 0;
    BenchmarkSceneTriangles = 0;
}

void AJUSYNCFileSpawnerActor::ScheduleBenchmarkInteractiveWindow()
{
    if (!bBenchmarkSessionActive || bBenchmarkFinalized || bBenchmarkWindowScheduled)
    {
        return;
    }
    bBenchmarkWindowScheduled = true;

    FJUSYNCBenchmarkTiming::Get().BeginInteractiveWindow(BenchmarkInteractiveWindowSeconds);
    UE_LOG(LogTemp, Display, TEXT("[JUSYNC Spawner Benchmark] interactive window open for %.1f s after initial spawn"),
        BenchmarkInteractiveWindowSeconds);

    const float FinalizeDelay = BenchmarkInteractiveWindowSeconds + 1.5f;
    GetWorldTimerManager().SetTimer(
        BenchmarkFinalizeTimerHandle,
        FTimerDelegate::CreateUObject(this, &AJUSYNCFileSpawnerActor::FinalizeBenchmarkSession),
        FinalizeDelay, false);
}

void AJUSYNCFileSpawnerActor::FinalizeBenchmarkSession()
{
    if (!bBenchmarkSessionActive || bBenchmarkFinalized)
    {
        return;
    }
    bBenchmarkFinalized = true;
    bBenchmarkWindowScheduled = false;
    GetWorldTimerManager().ClearTimer(BenchmarkFinalizeTimerHandle);

    UJUSYNCBlueprintLibrary::StopInteractiveWindow();

    const double Now = FPlatformTime::Seconds();
    const float TotalTimeMs = static_cast<float>((Now - BenchmarkSessionWallStart) * 1000.0);
    FPlatformMemoryStats StatsNow = FPlatformMemory::GetStats();
    const int64 VRAMNowBytes = UJUSYNCBlueprintLibrary::QueryVRAMUsageBytes();
    const int64 VRAMPeakBytes = FMath::Max(BenchmarkVRAMStartBytes, VRAMNowBytes);

    int32 SplitMeshCount = 0;
    float GPUPercent = 0.0f;
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (Subsystem)
    {
        SplitMeshCount = Subsystem->GetSplitMeshCount();
        GPUPercent = Subsystem->GetGPUUsage_Percent();
    }

    const FString OutDir = BenchmarkOutputDirectory.IsEmpty()
        ? FPaths::Combine(FPaths::ProjectDir(), TEXT("Benchmark"))
        : BenchmarkOutputDirectory;

    if (bExportClientBenchmarkReport)
    {
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        PlatformFile.CreateDirectoryTree(*OutDir);
        UJUSYNCBlueprintLibrary::ExportClientBenchmarkReport(
            FPaths::Combine(OutDir, TEXT("benchmark_client.json")));
    }

    FJUSYNCBenchmarkResult Result = UJUSYNCBlueprintLibrary::CreateBenchmarkResultExtended(
        BenchmarkTestName,
        TotalTimeMs,
        static_cast<int32>(BenchmarkSceneTriangles),
        static_cast<int32>(BenchmarkSceneVertices),
        BenchmarkRAMStartBytes,
        static_cast<int64>(StatsNow.UsedPhysical),
        static_cast<int64>(StatsNow.PeakUsedPhysical),
        static_cast<int64>(StatsNow.UsedPhysical),
        ActorsSpawned,
        0,
        SplitMeshCount,
        UJUSYNCBlueprintLibrary::QueryCPUUsagePercent(),
        BenchmarkVRAMStartBytes,
        VRAMNowBytes,
        VRAMPeakBytes,
        UJUSYNCBlueprintLibrary::QueryActiveThreadCount(),
        GPUPercent,
        0, 0.0f, 0.0f);  // hitches (never synthesised)

    if (bIncludeClusterBenchmark)
    {
        FetchClusterBenchmarkReport(Result);
    }

    ApplyClientReportCounters(OutDir, Result);

    UJUSYNCBlueprintLibrary::RecordBenchmarkResult(Result);

    UJUSYNCBlueprintLibrary::EndBenchmark();
    bBenchmarkSessionActive = false;

    UE_LOG(LogTemp, Display, TEXT("[JUSYNC Spawner Benchmark] session recorded (%d actors, %.1f s session, %lld bytes, %lld chunks, VRAM %.0f MB) -> %s"),
        ActorsSpawned, TotalTimeMs / 1000.0f, Result.BytesReceived, Result.ChunkCount,
        VRAMPeakBytes / (1024.0f * 1024.0f), *OutDir);
}

void AJUSYNCFileSpawnerActor::ApplyClientReportCounters(const FString& OutDir, FJUSYNCBenchmarkResult& Result)
{
    const FString ReportPath = FPaths::Combine(OutDir, TEXT("benchmark_client.json"));
    if (!FPaths::FileExists(ReportPath))
    {
        return;
    }

    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *ReportPath))
    {
        return;
    }

    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonStr), Root) || !Root.IsValid())
    {
        return;
    }

    const TSharedPtr<FJsonObject>* TotalPtr = nullptr;
    if (!Root->TryGetObjectField(TEXT("total"), TotalPtr) || !*TotalPtr)
    {
        return;
    }

    const TSharedPtr<FJsonObject>& Total = *TotalPtr;
    double Value = 0.0;
    if (Total->TryGetNumberField(TEXT("bytes_received"), Value)) { Result.BytesReceived = static_cast<int64>(Value); }
    if (Total->TryGetNumberField(TEXT("chunk_messages"), Value))  { Result.ChunkCount = static_cast<int64>(Value); }
    if (Total->TryGetNumberField(TEXT("files_requested"), Value)) { Result.FilesRequested = static_cast<int64>(Value); }
    if (Total->TryGetNumberField(TEXT("files_completed"), Value)) { Result.FilesCompleted = static_cast<int64>(Value); }

    const TSharedPtr<FJsonObject>* TimingPtr = nullptr;
    if (Root->TryGetObjectField(TEXT("timing_ms"), TimingPtr) && *TimingPtr)
    {
        if ((*TimingPtr)->TryGetNumberField(TEXT("connect_to_last_byte_ms"), Value))
        {
            Result.DownloadTotalMs = static_cast<float>(Value);
        }
    }
}

void AJUSYNCFileSpawnerActor::FetchClusterBenchmarkReport(FJUSYNCBenchmarkResult& Result)
{
    Result.ClusterWorkerCount = 0;
    Result.ClusterCommitCount = 0;
    Result.ClusterRawBytes = 0;
    Result.ClusterWireBytes = 0;
    Result.ClusterServingBytes = 0;
    Result.ClusterServingChunks = 0;
    Result.ClusterSerializeMsTotal = 0.0f;
    Result.ClusterStoreMsTotal = 0.0f;
    Result.ClusterReport = TEXT("{}");

    if (!IsBrokerConnected_C())
    {
        Result.ClusterReport = TEXT("{\"error\":\"broker not connected\"}");
        return;
    }

    unsigned int TotalCount = 0;
    if (RequestTotalWorkerCount_C(&TotalCount, 3000) != 1 || TotalCount == 0)
    {
        Result.ClusterReport = TEXT("{\"error\":\"worker count request failed\"}");
        return;
    }

    const int32 RankCount = FMath::Min<int32>(static_cast<int32>(TotalCount), 4096);

    int64 TotalCommits = 0;
    int64 TotalRaw = 0;
    int64 TotalWire = 0;
    int64 TotalDisk = 0;
    int64 TotalServingBytes = 0;
    int64 TotalServingChunks = 0;
    double TotalSerializeMs = 0.0;
    double TotalStoreMs = 0.0;

    FString WorkersArray = TEXT("[");
    bool bFirst = true;

    for (int32 Rank = 0; Rank < RankCount; ++Rank)
    {
        FString Body;
        if (!JUSYNCRequestFileToString(TEXT("__benchmark_rank__.json"), Rank, Body))
        {
            continue;
        }

        TSharedPtr<FJsonObject> Obj;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Obj) || !Obj.IsValid())
        {
            continue;
        }

        const double WorkerCommits = Obj->GetNumberField(TEXT("commit_count"));
        int64 WorkerServingBytes = 0, WorkerServingChunks = 0, WorkerServingFiles = 0;
        if (const TSharedPtr<FJsonObject>* ServingPtr = nullptr; Obj->TryGetObjectField(TEXT("serving"), ServingPtr) && *ServingPtr)
        {
            WorkerServingBytes = static_cast<int64>((*ServingPtr)->GetNumberField(TEXT("bytes")));
            WorkerServingChunks = static_cast<int64>((*ServingPtr)->GetNumberField(TEXT("chunks")));
            WorkerServingFiles = static_cast<int64>((*ServingPtr)->GetNumberField(TEXT("files")));
        }

        double WorkerRaw = 0.0, WorkerWire = 0.0, WorkerDisk = 0.0;
        double WorkerSerializeMs = 0.0, WorkerStoreMs = 0.0;
        if (const TSharedPtr<FJsonObject>* TotalsPtr = nullptr; Obj->TryGetObjectField(TEXT("totals"), TotalsPtr) && *TotalsPtr)
        {
            WorkerRaw = (*TotalsPtr)->GetNumberField(TEXT("raw_bytes"));
            WorkerWire = (*TotalsPtr)->GetNumberField(TEXT("wire_bytes"));
            WorkerDisk = (*TotalsPtr)->GetNumberField(TEXT("disk_bytes"));
            WorkerSerializeMs = (*TotalsPtr)->GetNumberField(TEXT("serialize_ms_total"));
            WorkerStoreMs = (*TotalsPtr)->GetNumberField(TEXT("store_ms_total"));
        }

        TotalCommits += static_cast<int64>(WorkerCommits);
        TotalRaw += static_cast<int64>(WorkerRaw);
        TotalWire += static_cast<int64>(WorkerWire);
        TotalDisk += static_cast<int64>(WorkerDisk);
        TotalServingBytes += WorkerServingBytes;
        TotalServingChunks += WorkerServingChunks;
        TotalSerializeMs += WorkerSerializeMs;
        TotalStoreMs += WorkerStoreMs;

        if (!bFirst)
        {
            WorkersArray += TEXT(",");
        }
        bFirst = false;
        WorkersArray += FString::Printf(
            TEXT("{\"rank\":%d,\"commits\":%lld,\"serving_bytes\":%lld,\"serving_chunks\":%lld,"
                 "\"serving_files\":%lld,\"raw_bytes\":%lld,\"wire_bytes\":%lld,\"disk_bytes\":%lld,"
                 "\"serialize_ms_total\":%.3f,\"store_ms_total\":%.3f}"),
            Rank,
            static_cast<long long>(static_cast<int64>(WorkerCommits)),
            static_cast<long long>(WorkerServingBytes),
            static_cast<long long>(WorkerServingChunks),
            static_cast<long long>(WorkerServingFiles),
            static_cast<long long>(static_cast<int64>(WorkerRaw)),
            static_cast<long long>(static_cast<int64>(WorkerWire)),
            static_cast<long long>(static_cast<int64>(WorkerDisk)),
            WorkerSerializeMs,
            WorkerStoreMs);
    }
    WorkersArray += TEXT("]");

    FString BrokerJson = TEXT("null");
    {
        FString Body;
        if (JUSYNCRequestFileToString(TEXT("__benchmark_broker__.json"), 0, Body))
        {
            TSharedPtr<FJsonObject> Obj;
            if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Obj) && Obj.IsValid())
            {
                BrokerJson = Body.TrimStartAndEnd();
            }
        }
    }

    Result.ClusterWorkerCount = RankCount;
    Result.ClusterCommitCount = TotalCommits;
    Result.ClusterRawBytes = TotalRaw;
    Result.ClusterWireBytes = TotalWire;
    Result.ClusterServingBytes = TotalServingBytes;
    Result.ClusterServingChunks = TotalServingChunks;
    Result.ClusterSerializeMsTotal = static_cast<float>(TotalSerializeMs);
    Result.ClusterStoreMsTotal = static_cast<float>(TotalStoreMs);

    Result.ClusterReport = FString::Printf(
        TEXT("{\"worker_count\":%d,\"workers\":%s,\"totals\":{\"commits\":%lld,"
             "\"raw_bytes\":%lld,\"wire_bytes\":%lld,\"disk_bytes\":%lld,"
             "\"serving_bytes\":%lld,\"serving_chunks\":%lld,"
             "\"serialize_ms_total\":%.3f,\"store_ms_total\":%.3f},\"broker\":%s}"),
        RankCount,
        *WorkersArray,
        static_cast<long long>(TotalCommits),
        static_cast<long long>(TotalRaw),
        static_cast<long long>(TotalWire),
        static_cast<long long>(TotalDisk),
        static_cast<long long>(TotalServingBytes),
        static_cast<long long>(TotalServingChunks),
        TotalSerializeMs,
        TotalStoreMs,
        *BrokerJson);
}

void AJUSYNCFileSpawnerActor::ApplyMorphSettings()
{
    auto SetCVar = [](const TCHAR* Name, int32 Value)
    {
        if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
        {
            CVar->Set(Value);
        }
    };

    SetCVar(TEXT("jusync.Morph.Enabled"), bMorphEnabled ? 1 : 0);
    SetCVar(TEXT("jusync.Morph.MeshBlendMs"), MeshBlendMs);
    SetCVar(TEXT("jusync.Morph.MeshMaxBlendMs"), MeshMaxBlendMs);
    SetCVar(TEXT("jusync.Morph.CloudSteps"), CloudSteps);
    SetCVar(TEXT("jusync.Morph.CloudBlendMs"), CloudBlendMs);
    SetCVar(TEXT("jusync.Morph.CloudMaxBlendMs"), CloudMaxBlendMs);
    SetCVar(TEXT("jusync.Morph.CloudFixedSize"), bCloudFixedSize ? 1 : 0);
    SetCVar(TEXT("jusync.Spawner.CollapseVariants"), bCollapseElementVariants ? 1 : 0);
    if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("jusync.Morph.CloudScreenSize")))
    {
        CVar->Set(CloudScreenSizePct);
    }

    if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("jusync.Spawner.IgnoreElements")))
    {
        CVar->Set(*FString::Join(IgnoredElementPatterns, TEXT(",")));
    }
}

void AJUSYNCFileSpawnerActor::BeginPlay()
{
    Super::BeginPlay();

    if (!PathlineManager)
    {
        PathlineManager = MakeShared<FJUSYNCPathlineManager>();
    }

    if (!bCollapseVariantsMigrated)
    {
        bCollapseElementVariants = false;
        bCollapseVariantsMigrated = true;
        UE_LOG(LogTemp, Display, TEXT("[Spawner] CollapseVariants migrated to 0 (one actor per logical element; all subsets kept)"));
    }

    CloudMaxBlendMs = FMath::Clamp(CloudMaxBlendMs, 8, 200);

    ApplyMorphSettings();

    UE_LOG(LogTemp, Display, TEXT("[Spawner] CollapseVariants effective=%d (property=%d)"),
        CVarJusyncCollapseVariants.GetValueOnGameThread(), bCollapseElementVariants ? 1 : 0);

    CommitCompleteCooldownSeconds = FMath::Min(CommitCompleteCooldownSeconds, 0.15f);

    if (MeshCache)
    {
        MeshCache->SetLimits(MeshCacheMaxEntries, static_cast<int64>(MeshCacheMaxMB) * 1024 * 1024);
    }
    if (AnimationController)
    {
        AnimationController->SetPlaybackSettings(TimeStepPlaybackFPS, bLoopTimeStepAnimation);
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->OnNotificationReceived.AddDynamic(this, &AJUSYNCFileSpawnerActor::OnBrokerNotification);
    }

    if (bAutoStart)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: auto-starting pipeline"));
        StartSpawning();
    }
}

void AJUSYNCFileSpawnerActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopLiveUpdatePolling();
    FinalizeBenchmarkSession();

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (Subsystem)
    {
        Subsystem->OnNotificationReceived.RemoveAll(this);
    }

    CancelSpawning();
    ClearSpawnedActors();
    PathlineManager.Reset();
    Super::EndPlay(EndPlayReason);
}

void AJUSYNCFileSpawnerActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    EnsurePathlineGlow();
    SyncPathlineManager();
    SweepPendingDeletedFiles();
    PollDynamicCacheBudgets(DeltaSeconds);

    if (bEnableLiveUpdates && LiveUpdatePollInterval > 0.0f &&
        bInitialSpawnDone && CurrentState == EJUSYNCSpawnerState::Complete)
    {
        LiveUpdatePollAccumulator += DeltaSeconds;
        if (LiveUpdatePollAccumulator >= LiveUpdatePollInterval)
        {
            LiveUpdatePollAccumulator = 0.0f;
            OnLiveUpdateTimer();
        }
    }

    if (AnimationController)
    {
        if (bEnableTimeStepAnimation)
        {
            AnimationController->Tick(DeltaSeconds);
        }
        else
        {
            AnimationController->ShowAll();
        }
    }

    if (bInitialSpawnDone)
    {
        FlushHiddenMeshUpdates();
    }

    if (bCommitDiffPending && bInitialSpawnDone && !bSceneDiffInFlight &&
        (!ChangeTracker || !ChangeTracker->HasActiveWork()) &&
        FPlatformTime::Seconds() - LastCommitCompleteTime >= CommitCompleteCooldownSeconds)
    {
        bCommitDiffPending = false;
        HandleCommitCompleteNotification(true);
    }

    if (CurrentState == EJUSYNCSpawnerState::Downloading && FilesTotal > 0)
    {
        DownloadWatchdogAcc += DeltaSeconds;
        if (DownloadWatchdogAcc >= 1.0f)
        {
            DownloadWatchdogAcc = 0.0f;
            CheckAllDownloadsComplete();
        }
    }
}

FJUSYNCPathlineSettings AJUSYNCFileSpawnerActor::BuildPathlineSettings() const
{
    FJUSYNCPathlineSettings Settings;
    Settings.bEnabled = bEnablePathlines;
    Settings.MaxTracks = FMath::Max(1, PathlineMaxTracks);
    Settings.HistorySamples = FMath::Max(2, PathlineHistorySamples);
    Settings.RetentionSamples = FMath::Max(Settings.HistorySamples,
        FMath::Max(64, PathlineHistorySamples * 8));
    Settings.MaxNodes = FMath::Max(0, PathlineMaxNodes);
    if (const int32 TrackCap = CVarJusyncPathlineMaxTracks.GetValueOnGameThread(); TrackCap > 0)
    {
        Settings.MaxTracks = TrackCap;
    }
    if (const int32 HistoryCap = CVarJusyncPathlineHistorySamples.GetValueOnGameThread(); HistoryCap > 2)
    {
        Settings.HistorySamples = HistoryCap;
    }
    if (const int32 NodeCap = CVarJusyncPathlineMaxNodes.GetValueOnGameThread(); NodeCap > 0)
    {
        Settings.MaxNodes = NodeCap;
    }
    Settings.TrackExpirySamples = FMath::Max(0, PathlineTrackExpirySamples);
    Settings.NiagaraSystem = PathlineNiagaraSystem;
    Settings.Radius = FMath::Max(0.001f, PathlineRadius);
    if (const float RadiusOverride = CVarJusyncPathlineRadius.GetValueOnGameThread(); RadiusOverride > 0.0f)
    {
        Settings.Radius = RadiusOverride;
    }
    Settings.RadiusScale = FMath::Max(0.001f, PathlineRadiusScale);
    Settings.bUsePointWidth = bPathlineUsePointWidth;
    Settings.IdAttribute = PathlineIdAttribute;
    Settings.ScalarAttribute = PathlineScalarAttribute;
    Settings.ScalarComponent = FMath::Clamp(PathlineScalarComponent, 0, 3);
    Settings.bFilterEnabled = bPathlineFilterEnabled;
    Settings.FilterMin = FMath::Min(PathlineFilterMin, PathlineFilterMax);
    Settings.FilterMax = FMath::Max(PathlineFilterMin, PathlineFilterMax);
    Settings.bFilterByHue = bPathlineHueFilterEnabled;
    Settings.FilterHue = FMath::Clamp(PathlineHue, 0.0f, 360.0f);
    Settings.FilterHueTolerance = FMath::Clamp(PathlineHueTolerance, 0.0f, 180.0f);
    Settings.bColorByScalar = bPathlineColorByScalar;
    Settings.ColorMin = FMath::Min(PathlineColorMin, PathlineColorMax);
    Settings.ColorMax = FMath::Max(PathlineColorMin, PathlineColorMax);
    Settings.FlatColor = PathlineFlatColor;
    Settings.SelectionSeed = static_cast<uint64>(FMath::Max(0, PathlineSelectionSeed));
    Settings.Material = PathlineMaterial;
    return Settings;
}

namespace
{
	const FName GStreamlinePositions(TEXT("User.Positions"));
	const FName GStreamlineLinkOrders(TEXT("User.LinkOrders"));
	const FName GStreamlineWidths(TEXT("User.Widths"));
	const FName GStreamlineColors(TEXT("User.Colors"));
	const FName GStreamlineScalars(TEXT("User.Scalars"));
	const FName GStreamlineSpawnCount(TEXT("User.SpawnCount"));

	void PushStreamlineArrays(UNiagaraComponent* Component, const FJUSYNCPathlineArrays& Arrays)
	{
		const FTransform ComponentToWorld = Component->GetComponentTransform();
		FBox LocalBounds(ForceInit);
		for (const FVector& Position : Arrays.Positions)
		{
			LocalBounds += ComponentToWorld.InverseTransformPositionNoScale(Position);
		}
		if (LocalBounds.IsValid)
		{
			const double Extent = LocalBounds.GetExtent().GetMax();
			Component->SetSystemFixedBounds(
				LocalBounds.ExpandBy(FMath::Max(1.0, Extent * 0.1)));
		}

		UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayPosition(
			Component, GStreamlinePositions, Arrays.Positions);
		UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayFloat(
			Component, GStreamlineLinkOrders, Arrays.LinkOrders);
		UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayFloat(
			Component, GStreamlineWidths, Arrays.Widths);
		UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayFloat(
			Component, GStreamlineScalars, Arrays.Scalars);
		UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayColor(
			Component, GStreamlineColors, Arrays.Colors);

		Component->SetVariableInt(GStreamlineSpawnCount, Arrays.Num());
		Component->ReinitializeSystem();
		Component->Activate(false);
	}
}

void AJUSYNCFileSpawnerActor::DestroyPathlineComponent(const FString& ElementName)
{
	if (TObjectPtr<UNiagaraComponent>* Found = PathlineComponents.Find(ElementName))
	{
		if (UNiagaraComponent* Component = Found->Get())
		{
			Component->Deactivate();
			Component->DestroyComponent();
		}
		PathlineComponents.Remove(ElementName);
	}
}

void AJUSYNCFileSpawnerActor::DestroyAllPathlineComponents()
{
	for (TPair<FString, TObjectPtr<UNiagaraComponent>>& Pair : PathlineComponents)
	{
		if (UNiagaraComponent* Component = Pair.Value.Get())
		{
			Component->Deactivate();
			Component->DestroyComponent();
		}
	}
	PathlineComponents.Empty();
}

void AJUSYNCFileSpawnerActor::EnsurePathlineGlow()
{
    const float Glow = CVarJusyncPathlineGlow.GetValueOnGameThread();
    if (FMath::IsNearlyEqual(Glow, LastPathlineGlow, 1e-4f))
    {
        return;
    }

    if (!PathlineGlowCollection)
    {
        PathlineGlowCollection = LoadObject<UMaterialParameterCollection>(
            nullptr, TEXT("/Game/Materials/MPC_Jusync.MPC_Jusync"));
        if (!PathlineGlowCollection)
        {
            if (!bPathlineGlowMissingWarned)
            {
                bPathlineGlowMissingWarned = true;
                UE_LOG(LogTemp, Warning, TEXT(
                    "[Spawner] MPC_Jusync is missing, so jusync.Pathlines.Glow "
                    "has no effect on the ribbons."));
            }
            return;
        }
    }

    UKismetMaterialLibrary::SetScalarParameterValue(
        this, PathlineGlowCollection, FName(TEXT("PathlineGlow")), Glow);
    LastPathlineGlow = Glow;
}

void AJUSYNCFileSpawnerActor::SyncPathlineComponents()
{
	if (!bEnablePathlines || !PathlineManager)
	{
		DestroyAllPathlineComponents();
		return;
	}

	TArray<FJUSYNCPathlineManager::FCompletedArrays> Completed;
	PathlineManager->DrainArrays(Completed);

	UNiagaraSystem* System = PathlineNiagaraSystem.LoadSynchronous();
	if (!System && !bPathlineSystemWarned)
	{
		bPathlineSystemWarned = true;
		if (Completed.Num() > 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[Spawner] bEnablePathlines is on but PathlineNiagaraSystem is unset - ")
				TEXT("trails are not drawn. Assign NS_JusyncStreamlines."));
		}
	}

	const TSet<FString> LiveElements(PathlineManager->GetElementKeys());
	for (auto It = PathlineComponents.CreateIterator(); It; ++It)
	{
		UNiagaraComponent* Component = It.Value().Get();
		if (!Component || !LiveElements.Contains(It.Key()))
		{
			if (Component)
			{
				Component->Deactivate();
				Component->DestroyComponent();
			}
			It.RemoveCurrent();
		}
	}

	if (Completed.Num() == 0)
	{
		return;
	}

	UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
	FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr;

	FTransform FallbackTransform(
		FQuat::Identity,
		PCSpawner ? PCSpawner->GetSpawnLocation() : FVector::ZeroVector,
		FVector(PCSpawner ? PCSpawner->GetSpawnScale() : 1.0f));

	for (const FJUSYNCPathlineManager::FCompletedArrays& Result : Completed)
	{
		if (!Result.Arrays.IsValid())
		{
			continue;
		}

		TObjectPtr<UNiagaraComponent>& Slot = PathlineComponents.FindOrAdd(Result.ElementName);
		UNiagaraComponent* Component = Slot.Get();
		if (!Component)
		{
			Component = NewObject<UNiagaraComponent>(this);
			Component->SetAsset(System);
			Component->SetAllowScalability(false);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->bAutoActivate = false;
			Component->SetupAttachment(GetRootComponent());
			AddInstanceComponent(Component);
			Component->RegisterComponent();
			Slot = Component;
		}
		else if (Component->GetAsset() != System)
		{
			Component->SetAsset(System);
		}

		FTransform CloudTransform = FallbackTransform;
		if (PCSpawner)
		{
			if (const AActor* ElementActor = PCSpawner->FindActorByElementKey(Result.ElementName))
			{
				CloudTransform = ElementActor->GetActorTransform();
			}
		}

		Component->SetWorldLocation(CloudTransform.GetLocation());
		Component->SetWorldScale3D(FVector::OneVector);

		if (Result.Arrays->Num() == 0)
		{
			Component->Deactivate();
			continue;
		}
		if (!System)
		{
			continue;
		}

		for (FVector& Position : Result.Arrays->Positions)
		{
			Position = CloudTransform.TransformPosition(Position);
		}

		PushStreamlineArrays(Component, *Result.Arrays);
	}
}

UMaterialInterface* AJUSYNCFileSpawnerActor::CreateEmissiveVertexColorMaterial()
{
#if WITH_EDITOR
	UMaterial* Material = NewObject<UMaterial>(
		GetTransientPackage(), TEXT("M_JusyncTrailVertexColor"));
	if (!Material)
	{
		return nullptr;
	}

	UMaterialExpressionVertexColor* VertexColor =
		NewObject<UMaterialExpressionVertexColor>(Material);

	UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
	EditorData->BaseColor.Expression = VertexColor;

	UMaterialExpressionMultiply* Dim =
		NewObject<UMaterialExpressionMultiply>(Material);
	Dim->A.Expression = VertexColor;
	Dim->ConstB = FMath::Clamp(PathlineEmissiveStrength, 0.0f, 4.0f);
	EditorData->EmissiveColor.Expression = Dim;
	TrailEmissiveExpression = Dim;

	Material->PostEditChange();
	return Material;
#else
	return nullptr;
#endif
}

void AJUSYNCFileSpawnerActor::SyncPathlineManager()
{
    if (!PathlineManager)
    {
        PathlineManager = MakeShared<FJUSYNCPathlineManager>();
    }

    if (!PathlineMaterial && !bPathlineMaterialFallbackTried)
    {
        bPathlineMaterialFallbackTried = true;
        PathlineMaterial = CreateEmissiveVertexColorMaterial();
        if (!PathlineMaterial)
        {
            UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
            PathlineMaterial = Subsystem
                ? Subsystem->GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"))
                : LoadObject<UMaterialInterface>(
                    nullptr, TEXT("/Game/Materials/M_VertexColor.M_VertexColor"));
        }
        UE_LOG(LogTemp, Display, TEXT("[Spawner] Pathline material: %s"),
            PathlineMaterial ? *PathlineMaterial->GetName()
                             : TEXT("none â€” trails keep the default material and lose vertex colours"));
    }

    if (TrailEmissiveExpression)
    {
        const float Wanted = FMath::Clamp(PathlineEmissiveStrength, 0.0f, 4.0f);
        if (!FMath::IsNearlyEqual(TrailEmissiveExpression->ConstB, Wanted, 1e-3f))
        {
            TrailEmissiveExpression->ConstB = Wanted;
#if WITH_EDITOR
            if (UMaterial* TrailMaterial = TrailEmissiveExpression->GetTypedOuter<UMaterial>())
            {
                TrailMaterial->PostEditChange();
            }
#endif
        }
    }

    PathlineManager->SetSettings(BuildPathlineSettings());
    if (PathlineManager->ConsumeResampleRequest() && bEnablePathlines)
    {
        RebuildPathlinesFromSampleCache();
    }

    PathlineManager->TickDisplayBlend();

    SyncPathlineComponents();

    const FJUSYNCPathlineStats Stats = PathlineManager->GetStats();
    PathlineActiveTracks = Stats.ActiveTracks;
    PathlineNodeCount = Stats.TotalNodes;
    PathlineVisibleTracks = Stats.VisibleTracks;
    PathlineDroppedSamples = static_cast<int32>(FMath::Min<int64>(Stats.DroppedSamples, MAX_int32));
    PathlineIgnoredSamples = Stats.IgnoredSamples;

    {
        static double LastStatsLogTime = -1.0;
        const double NowSeconds = FPlatformTime::Seconds();
        if (bEnablePathlines && (LastStatsLogTime < 0.0 || NowSeconds - LastStatsLogTime >= 2.0))
        {
            LastStatsLogTime = NowSeconds;
            UE_LOG(LogTemp, Display,
                TEXT("[Spawner] Pathlines: active=%d visible=%d nodes=%lld ignored=%lld dropped=%lld "
                     "dupIds=%lld elements=%d ingest=%.2fms build=%.2fms display=%llu"),
                Stats.ActiveTracks, Stats.VisibleTracks, Stats.TotalNodes, Stats.IgnoredSamples,
                Stats.DroppedSamples, Stats.DuplicateIds, PathlineManager->GetElementKeys().Num(),
                Stats.LastIngestMs, Stats.LastBuildMs,
                static_cast<unsigned long long>(PathlineManager->GetDisplaySampleId()));
        }
    }
}

void AJUSYNCFileSpawnerActor::RebuildPathlinesFromSampleCache()
{
    if (!PathlineManager || !bEnablePathlines)
    {
        return;
    }

    const uint64 SavedCutoff = PathlineManager->GetDisplaySampleId();
    PathlineManager->Reset();

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr;
    const TArray<FColor> Lut = PCSpawner ? PCSpawner->GetGradientLUT() : TArray<FColor>();

    struct FReplayEntry
    {
        uint64 SampleId = 0;
        uint64 Serial = 0;
        int32 Rank = 0;
        FString Element;
        FJUSYNCPointCloudRef Cloud;
    };

    TArray<FReplayEntry> Entries;
    for (const TPair<FString, TArray<FJUSYNCSampleCacheEntry>>& Pair : SampleCache)
    {
        FString KeyElement;
        Pair.Key.Split(TEXT("|"), &KeyElement, nullptr);
        const FString CanonicalKeyElement = JUSYNCCanonicalPointCloudElement(KeyElement);

        for (const FJUSYNCSampleCacheEntry& Cached : Pair.Value)
        {
            if (!Cached.PointCloud.IsValid() || !Cached.PointCloud->bFromRawGeo ||
                !Cached.PointCloud->bComplete)
            {
                continue;
            }

            FString Element = JUSYNCCanonicalPointCloudElement(Cached.PointCloud->ElementName);
            if (Element.IsEmpty())
            {
                Element = CanonicalKeyElement;
            }
            if (Element.IsEmpty() || (PCSpawner && PCSpawner->IsElementFiltered(Element)))
            {
                continue;
            }

            FReplayEntry Entry;
            Entry.SampleId = Cached.SampleId;
            Entry.Serial = Cached.Serial;
            Entry.Rank = FMath::Max(Cached.Rank, 0);
            Entry.Element = MoveTemp(Element);
            Entry.Cloud = Cached.PointCloud;
            Entries.Add(MoveTemp(Entry));
        }
    }

    Entries.Sort([](const FReplayEntry& A, const FReplayEntry& B)
    {
        if (A.SampleId != B.SampleId)
        {
            return A.SampleId < B.SampleId;
        }
        if (A.Rank != B.Rank)
        {
            return A.Rank < B.Rank;
        }
        if (A.Element != B.Element)
        {
            return A.Element < B.Element;
        }
        return A.Serial < B.Serial;
    });

    for (const FReplayEntry& Entry : Entries)
    {
        PathlineManager->EnqueueSample(Entry.Element, Entry.Cloud, Entry.Rank,
            Entry.SampleId, Lut);
    }

    PathlineManager->SetDisplaySampleId(SavedCutoff);
}

void AJUSYNCFileSpawnerActor::StartSpawning()
{
    if (CurrentState != EJUSYNCSpawnerState::Idle &&
        CurrentState != EJUSYNCSpawnerState::Complete &&
        CurrentState != EJUSYNCSpawnerState::Error)
    {
        UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: already running in state %d"), (int32)CurrentState);
        return;
    }

    ClearSpawnedActors();
    RawFileList.Empty(); RawFileSizes.Empty(); RawFileRanks.Empty();
    FilteredFiles.Empty(); FilteredSizes.Empty(); FilteredRanks.Empty();
    GradientPngRankMap.Empty();
    ParseFailedIndices.Empty();
    GradientPendingMeshes.Empty();
    GradientDeferredMeshes.Empty();
    GradientDeferredReplacementActors.Empty();
    GetWorldTimerManager().ClearTimer(GradientDeferredTimerHandle);
    bGradientReady = false;
    bGradientDownloadStarted = false;
    FilesDownloaded = 0; FilesTotal = 0; ActorsSpawned = 0;
    FailedFileIndices.Empty();
    NextSpawnIndex = 0; PendingDownloads = 0; PendingAsyncSpawns = 0;
    PendingAsyncPCS = 0; PendingParseTasks = 0;
    bIsCancelled = false;
    CurrentRetryCount = 0;
    InitialFileListRetryCount = 0;
    bInitialKickInFlight = false;
    CurrentState = EJUSYNCSpawnerState::Connecting;
    bInitialSpawnDone = false;
    bSceneDiffInFlight = false;
    bCommitDiffPending = false;
    InitialSpawnPendingSince = 0.0;
    PipelineActive = 0;

    MaybeStartBenchmarkSession();

    FJUSYNCBenchmarkTiming::Get().MarkConnectStart();
    if (ChangeTracker) ChangeTracker->Clear();
    AbortAllInFlightDownloads();
    DeferredSpawns.Empty();
    FilenameToPCActors.Empty();
    PCElementToFilename.Empty();
    if (AnimationController) AnimationController->Clear();

    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: starting pipeline"));

    if (UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
    {
        if (FJUSYNCPointCloudSpawner* Spawner = Subsystem->GetPointCloudSpawner())
        {
            Spawner->OnPointCloudSpawned.Clear();
            Spawner->OnPointCloudSpawned.AddUObject(this, &AJUSYNCFileSpawnerActor::OnPointCloudSpawnedHandler);
        }
    }

    ConnectToBroker();
}

void AJUSYNCFileSpawnerActor::CancelSpawning()
{
    bIsCancelled = true;
    PendingAsyncSpawns = 0;
    PendingAsyncPCS = 0;
    PendingParseTasks = 0;
    CurrentState = EJUSYNCSpawnerState::Idle;
    PendingDownloads = 0;
    PipelineActive = 0;
    bSceneDiffInFlight = false;
    if (ChangeTracker) ChangeTracker->Clear();
    AbortAllInFlightDownloads();
    DeferredSpawns.Empty();
    GradientDeferredMeshes.Empty();
    GradientDeferredReplacementActors.Empty();
    GetWorldTimerManager().ClearTimer(GradientDeferredTimerHandle);
}

void AJUSYNCFileSpawnerActor::ClearSpawnedActors()
{
    if (UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
    {
        Subsystem->ClearProcessedFiles();
        if (FJUSYNCPointCloudSpawner* PCSpawner = Subsystem->GetPointCloudSpawner())
        {
            PCSpawner->DestroyAllActors();
        }
    }

    for (AActor* Actor : SpawnedActors)
    {
        if (Actor && Actor->IsValidLowLevel())
        {
            if (AnimationController) AnimationController->RemoveActor(Actor);
            Actor->Destroy();
        }
    }
    SpawnedActors.Empty();
    ActorsSpawned = 0;
    FileToActorMap.Empty();
    FilenameToActors.Empty();
    FilenameToPCActors.Empty();
    PCElementToFilename.Empty();
    ClearSampleCache();
    FileLastSize.Empty();
    FileHashLo.Empty();
    FileHashHi.Empty();
    GradientPendingMeshes.Empty();
    HiddenPendingMeshUpdates.Empty();
    GradientDeferredMeshes.Empty();
    GradientDeferredReplacementActors.Empty();
    GetWorldTimerManager().ClearTimer(GradientDeferredTimerHandle);
    if (AnimationController) AnimationController->Clear();
    ElementVariantWinners.Empty();
    IgnoredVariantFiles.Empty();

    FitUnionBounds = FBox(ForceInit);
    bHasFitUnionBounds = false;
    bFitLocked = false;
    bFitUnionGrew = false;
    LastMeshLocalBounds.Empty();

    if (PathlineManager)
    {
        PathlineManager->Reset();
        DestroyAllPathlineComponents();
        PathlineActiveTracks = 0;
        PathlineNodeCount = 0;
        PathlineVisibleTracks = 0;
        PathlineDroppedSamples = 0;
        PathlineIgnoredSamples = 0;
    }
}

FVector AJUSYNCFileSpawnerActor::GetNextSpawnLocation() const
{
    FVector Origin = BaseSpawnLocation;
    if (SpawnTargetActor && SpawnTargetActor->IsValidLowLevel() && !SpawnTargetActor->HasAnyFlags(RF_ClassDefaultObject))
    {
        Origin = SpawnTargetActor->GetActorLocation();
    }
    else if (GetWorld() && !HasAnyFlags(RF_ClassDefaultObject))
    {
        Origin = GetActorLocation();
    }

    int32 Col = NextSpawnIndex % SpawnGridColumns;
    int32 Row = NextSpawnIndex / SpawnGridColumns;
    return Origin + FVector(Col * SpawnSpacing, Row * SpawnSpacing, 0.0f);
}

FVector AJUSYNCFileSpawnerActor::ComputeFitScale(const FBox& MeshBox) const
{
    if (!bFitToVolume || !FitVolume || !MeshBox.IsValid)
    {
        return FVector::ZeroVector;
    }

    const FVector Target = FitVolume->GetScaledBoxExtent() * 2.0
        * (1.0f - FMath::Clamp(FitPaddingFraction, 0.0f, 0.9f));
    const FVector Size = MeshBox.GetSize();
    if (Target.X <= 0.f || Target.Y <= 0.f || Target.Z <= 0.f)
    {
        return FVector::ZeroVector;
    }

    auto Ratio = [](float TargetLen, float MeshLen)
    {
        return (MeshLen > KINDA_SMALL_NUMBER) ? TargetLen / MeshLen : TNumericLimits<float>::Max();
    };

    const FVector R(Ratio(Target.X, Size.X), Ratio(Target.Y, Size.Y), Ratio(Target.Z, Size.Z));
    FVector S = (FitMode == EJUSYNCFitMode::Uniform)
        ? FVector(FMath::Min3(R.X, R.Y, R.Z))  // biggest uniform scale that still fits
        : R;  // per-axis fill

    if (S.ContainsNaN())
    {
        S = FVector::OneVector;
    }
    return FVector(
        FMath::Clamp(S.X, 1e-6f, 1e6f),
        FMath::Clamp(S.Y, 1e-6f, 1e6f),
        FMath::Clamp(S.Z, 1e-6f, 1e6f));
}

FVector AJUSYNCFileSpawnerActor::ComputeFitLocation(const FBox& MeshBox, const FVector& Scale) const
{
    if (!FitVolume || !MeshBox.IsValid)
    {
        return GetNextSpawnLocation();
    }
    return FitVolume->Bounds.Origin - MeshBox.GetCenter() * Scale;
}

void AJUSYNCFileSpawnerActor::AccumulateFitBounds(const FBox& B)
{
    if (!bFitToVolume || !FitVolume || bFitLocked || !B.IsValid)
    {
        return;
    }
    if (!bHasFitUnionBounds)
    {
        FitUnionBounds = B;
        bHasFitUnionBounds = true;
        bFitUnionGrew = true;
        return;
    }

    const FVector Slack = FitUnionBounds.GetSize() * 0.02f;
    const bool bOutside =
        B.Min.X < FitUnionBounds.Min.X - Slack.X ||
        B.Min.Y < FitUnionBounds.Min.Y - Slack.Y ||
        B.Min.Z < FitUnionBounds.Min.Z - Slack.Z ||
        B.Max.X > FitUnionBounds.Max.X + Slack.X ||
        B.Max.Y > FitUnionBounds.Max.Y + Slack.Y ||
        B.Max.Z > FitUnionBounds.Max.Z + Slack.Z;
    if (bOutside)
    {
        FitUnionBounds += B;
        bFitUnionGrew = true;
    }

    if (bOutside && bInitialSpawnDone)
    {
        RefitSpawnedActorsToVolume();
    }
}

void AJUSYNCFileSpawnerActor::RefitSpawnedActorsToVolume()
{
    if (!bFitToVolume || !FitVolume || !bHasFitUnionBounds)
    {
        return;
    }

    const FVector S = ComputeFitScale(FitUnionBounds);
    if (S.IsZero())
    {
        return;
    }
    const FVector Loc = ComputeFitLocation(FitUnionBounds, S);

    int32 Refitted = 0;
    for (AActor* Actor : SpawnedActors)
    {
        if (!Actor || !Actor->IsValidLowLevel())
        {
            continue;
        }
        if (!LastMeshLocalBounds.Contains(Actor))
        {
            continue;
        }
        Actor->SetActorScale3D(S);
        if (bCenterInVolume)
        {
            Actor->SetActorLocation(Loc);
        }
        Refitted++;
    }

    int32 RefittedPCs = 0;
    if (UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
    {
        if (FJUSYNCPointCloudSpawner* Spawner = Subsystem->GetPointCloudSpawner())
        {
            Spawner->SetSpawnScale(S.X);
            if (bCenterInVolume)
            {
                Spawner->SetSpawnLocation(Loc);
            }
            RefittedPCs = Spawner->GetActiveActorCount();
            Spawner->ReapplyTransformToAllActors();
        }
    }

    UE_LOG(LogTemp, Display, TEXT("[Spawner] FitVolume: refit %d meshes / %d point clouds (union %.1f x %.1f x %.1f)"),
        Refitted, RefittedPCs,
        FitUnionBounds.GetSize().X, FitUnionBounds.GetSize().Y, FitUnionBounds.GetSize().Z);
}

int32 AJUSYNCFileSpawnerActor::CalculateDynamicTimeout(int64 FileSizeBytes) const
{
    const int32 BaseTimeout = 5000;
    const int32 MaxTimeout = 600000;
    float TimeForTransfer = (static_cast<float>(FileSizeBytes) / FMath::Max(1.0f, BandwidthBytesPerSecond)) * 1000.0f;
    int32 DynamicTimeout = BaseTimeout + static_cast<int32>(TimeForTransfer);
    return FMath::Clamp(DynamicTimeout, RequestTimeoutMs, MaxTimeout);
}

void AJUSYNCFileSpawnerActor::ConnectToBroker()
{
    if (bIsCancelled) return;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        CurrentState = EJUSYNCSpawnerState::Error;
        OnError.Broadcast(TEXT("Subsystem not available"));
        return;
    }

    bool bInitialized = Subsystem->InitializeMiddleware();
    if (!bInitialized)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: middleware initialization failed"));
        CurrentState = EJUSYNCSpawnerState::Error;
        OnError.Broadcast(TEXT("Middleware initialization failed"));
        return;
    }

    bool bConnected = Subsystem->ConnectToBroker(BrokerEndpoint, 5000);
    if (!bConnected)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: failed to connect to broker at %s"), *BrokerEndpoint);
        CurrentState = EJUSYNCSpawnerState::Error;
        OnError.Broadcast(TEXT("Failed to connect to broker"));
        return;
    }

    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: connected, requesting file list"));
    CurrentState = EJUSYNCSpawnerState::FetchingList;
    RequestFileList();
}

void AJUSYNCFileSpawnerActor::RequestFileList()
{
    if (bIsCancelled) return;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        OnFileListError(TEXT("Subsystem not available"));
        return;
    }

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;

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
            [WeakThis, Files = MoveTemp(Files), Sizes = MoveTemp(Sizes), Ranks = MoveTemp(Ranks), HashLo = MoveTemp(HashLo), HashHi = MoveTemp(HashHi), bSuccess]()
            {
                if (WeakThis.IsValid())
                {
                    WeakThis->RawHashLo = HashLo;
                    WeakThis->RawHashHi = HashHi;
                    WeakThis->OnFileListReceived_Internal(Files, Sizes, Ranks, bSuccess, HashLo, HashHi);
                }
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
}

void AJUSYNCFileSpawnerActor::FilterFileList(const TArray<FString>& InFiles, const TArray<int64>& InSizes, const TArray<int32>& InRanks,
    const TArray<uint64>& InHashLo, const TArray<uint64>& InHashHi,
    TArray<FString>& OutFiles, TArray<int64>& OutSizes, TArray<int32>& OutRanks,
    TArray<uint64>& OutHashLo, TArray<uint64>& OutHashHi,
    TArray<FString>& OutPngFiles, TArray<int64>& OutPngSizes, TArray<int32>& OutPngRanks) const
{
    OutFiles.Empty();
    OutSizes.Empty();
    OutRanks.Empty();
    OutHashLo.Empty();
    OutHashHi.Empty();
    OutPngFiles.Empty();
    OutPngSizes.Empty();
    OutPngRanks.Empty();

    OutFiles.Reserve(InFiles.Num());
    OutSizes.Reserve(InFiles.Num());
    OutRanks.Reserve(InFiles.Num());
    OutHashLo.Reserve(InFiles.Num());
    OutHashHi.Reserve(InFiles.Num());

    for (int32 i = 0; i < InFiles.Num(); ++i)
    {
        const FString& Fname = InFiles[i];
        int64 Fsize = InSizes.IsValidIndex(i) ? InSizes[i] : 0;
        int32 Frank = InRanks.IsValidIndex(i) ? InRanks[i] : 0;
        uint64 FhLo = InHashLo.IsValidIndex(i) ? InHashLo[i] : 0;
        uint64 FhHi = InHashHi.IsValidIndex(i) ? InHashHi[i] : 0;

        if (Fname.EndsWith(TEXT(".png")) || Fname.EndsWith(TEXT(".PNG")))
        {
            OutPngFiles.Add(Fname);
            OutPngSizes.Add(Fsize);
            OutPngRanks.Add(Frank);
            continue;
        }

        if (bFilterUSDOnly)
        {
            FString Ext = FPaths::GetExtension(Fname).ToLower();
            if (Ext != TEXT("usd") && Ext != TEXT("usda") && Ext != TEXT("usdc") &&
                Ext != TEXT("usdz") && Ext != TEXT("geo"))
                continue;
        }

        if (MinimumFileSizeBytes > 0 && Fsize < MinimumFileSizeBytes)
            continue;

        if (bClipsOnly && !Fname.StartsWith(TEXT("clips/")))
            continue;

        OutFiles.Add(Fname);
        OutSizes.Add(Fsize);
        OutRanks.Add(Frank);
        OutHashLo.Add(FhLo);
        OutHashHi.Add(FhHi);
    }
}

void AJUSYNCFileSpawnerActor::OnFileListReceived_Internal(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks, bool bSuccess, const TArray<uint64>& HashLo, const TArray<uint64>& HashHi)
{
    if (bIsCancelled) return;

    if (!bInitialSpawnDone) bInitialKickInFlight = false;

    if (!bSuccess || FileList.Num() == 0)
    {
        constexpr int32 MaxInitialFileListRetries = 15;
        if (InitialFileListRetryCount < MaxInitialFileListRetries)
        {
            ++InitialFileListRetryCount;
            UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: file list %s (retry %d/%d in 3s) â€” broker may still be prepping scene"),
                   bSuccess ? TEXT("empty") : TEXT("request failed"), InitialFileListRetryCount, MaxInitialFileListRetries);
            CurrentState = EJUSYNCSpawnerState::FetchingList;
            GetWorldTimerManager().SetTimer(
                InitialFileListRetryTimerHandle,
                FTimerDelegate::CreateUObject(this, &AJUSYNCFileSpawnerActor::RequestFileList),
                3.0f, false);
            return;
        }

        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: failed to retrieve file list from broker after %d retries"), MaxInitialFileListRetries);
        CurrentState = EJUSYNCSpawnerState::Error;
        OnError.Broadcast(TEXT("Failed to retrieve file list"));
        return;
    }

    RawFileList = FileList;
    RawFileSizes = FileSizes;
    RawFileRanks = FileRanks;
    RawHashLo = HashLo;
    RawHashHi = HashHi;

    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: received %d files"), FileList.Num());

    TArray<FString> PngFiles;
    TArray<int64> PngSizes;
    TArray<int32> PngRanks;
    TArray<FString> LocalFiles;
    TArray<int64> LocalSizes;
    TArray<int32> LocalRanks;
    TArray<uint64> LocalHashLo;
    TArray<uint64> LocalHashHi;
    FilterFileList(FileList, FileSizes, FileRanks, HashLo, HashHi,
        LocalFiles, LocalSizes, LocalRanks, LocalHashLo, LocalHashHi, PngFiles, PngSizes, PngRanks);

    CollapseElementVariants(LocalFiles, LocalSizes, LocalRanks, LocalHashLo, LocalHashHi);

    for (int32 i = 0; i < PngFiles.Num(); ++i)
    {
        FJUSYNCFileCandidateMeta Meta;
        Meta.Rank = PngRanks.IsValidIndex(i) ? PngRanks[i] : 0;
        Meta.Size = PngSizes.IsValidIndex(i) ? PngSizes[i] : 0;
        GradientPngRankMap.Add(PngFiles[i], Meta);
    }

    RawFileList = MoveTemp(LocalFiles);
    RawFileSizes = MoveTemp(LocalSizes);
    RawFileRanks = MoveTemp(LocalRanks);
    RawHashLo = MoveTemp(LocalHashLo);
    RawHashHi = MoveTemp(LocalHashHi);
    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: single-pass filter: %d geometry files, %d PNGs"), RawFileList.Num(), PngFiles.Num());

    FilteredFiles = RawFileList;
    FilteredSizes = RawFileSizes;
    FilteredRanks = RawFileRanks;
    FilteredHashLo = RawHashLo;
    FilteredHashHi = RawHashHi;
    FilesTotal = FilteredFiles.Num();

    if (FilteredFiles.Num() == 0)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: no files after filtering"));
        bInitialSpawnDone = true;
        CurrentState = EJUSYNCSpawnerState::Complete;
        OnAllComplete.Broadcast(0, false);
        if (bEnableLiveUpdates)
        {
            StartLiveUpdatePolling();
        }
        return;
    }

    if (UJUSYNCSubsystem* GrdSubsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
    {
        MaybeTriggerGradientLoad(FileList, FileSizes, FileRanks, GrdSubsystem);
        LoadMeshTextureAsync(GrdSubsystem);
    }

    CurrentState = EJUSYNCSpawnerState::Downloading;
    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: downloading %d files"), FilesTotal);
    ProcessAndDownloadFiles();
}

void AJUSYNCFileSpawnerActor::OnFileListError(const FString& ErrorMessage)
{
    UE_LOG(LogTemp, Error, TEXT("JUSYNC Spawner: file list error: %s"), *ErrorMessage);
    CurrentState = EJUSYNCSpawnerState::Error;
    OnError.Broadcast(ErrorMessage);
}

void AJUSYNCFileSpawnerActor::ProcessAndDownloadFiles()
{
    if (bIsCancelled || FilteredFiles.Num() == 0) return;

    PendingDownloads = FilteredFiles.Num();
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC Spawner: subsystem not available"));
        return;
    }

    PipelineNextIndex = 0;
    PipelineActive = 0;

    int32 InitialBatch = FMath::Min(PipelineDepth, FilteredFiles.Num());
    for (int32 i = 0; i < InitialBatch; ++i)
    {
        PipelineDownloadNext(Subsystem);
    }
}

void AJUSYNCFileSpawnerActor::PipelineDownloadNext(UJUSYNCSubsystem* Subsystem)
{
    if (!Subsystem || bIsCancelled) return;
    if (PipelineNextIndex >= FilteredFiles.Num()) return;

    const int32 i = PipelineNextIndex++;
    const FString Filename = FilteredFiles[i];
    const int32 TargetRank = FilteredRanks.IsValidIndex(i) ? FilteredRanks[i] : 0;
    const int64 FileSize = FilteredSizes.IsValidIndex(i) ? FilteredSizes[i] : int64(1048576);
    const uint64 HashLo = FilteredHashLo.IsValidIndex(i) ? FilteredHashLo[i] : 0;
    const uint64 HashHi = FilteredHashHi.IsValidIndex(i) ? FilteredHashHi[i] : 0;

    PipelineActive++;
    LoadFileThroughPipeline(Filename, TargetRank, FileSize, HashLo, HashHi, i, true);
}

void AJUSYNCFileSpawnerActor::LoadFileThroughPipeline(const FString& Filename, int32 Rank, int64 Size, uint64 HashLo, uint64 HashHi, int32 FileIndex, bool bIsInitial, uint64 SampleId)
{
    if (bIsCancelled || Filename.IsEmpty())
    {
        PipelineActive = FMath::Max(0, PipelineActive - 1);
        return;
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        PipelineActive = FMath::Max(0, PipelineActive - 1);
        if (bIsInitial && !bInitialSpawnDone)
        {
            FilesDownloaded++;
            OnFileProgress.Broadcast(FilesDownloaded, FilesTotal);
            if (FileIndex >= 0 && !FailedFileIndices.Contains(FileIndex))
                FailedFileIndices.Add(FileIndex);
            CheckAllDownloadsComplete();
        }
        else if (ChangeTracker)
        {
            ChangeTracker->MarkFailed(Filename);
            ChainRefreshNext();
        }
        return;
    }

    TArray<FColor> ParseLUT;
    uint64 ParseLUTVersion = 0;
    if (bGradientReady.load() && !bMeshTextureReady && Subsystem->GetPointCloudSpawner())
    {
        ParseLUT = Subsystem->GetPointCloudSpawner()->GetGradientLUT();
        ParseLUTVersion = Subsystem->GetPointCloudSpawner()->GetLUTVersion();
    }

    const uint64 ExpectedLUTVersion = ParseLUT.Num() > 1 ? ParseLUTVersion : 0;

    if (bEnableMeshCache && MeshCache && (HashLo != 0 || HashHi != 0 || Size > 0))
    {
        TArray<FJUSYNCCompactMeshRef> CachedMeshes;
        TArray<FJUSYNCPointCloudRef> CachedPointClouds;
        TArray<TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet>> CachedStreams;
        if (MeshCache->Get(Filename, HashLo, HashHi, Size, ExpectedLUTVersion, CachedMeshes, CachedPointClouds, &CachedStreams))
        {
            if (!bCachePointClouds)
            {
                CachedPointClouds.Empty();
            }

            UE_LOG(LogTemp, Display, TEXT("[Spawner] Cache hit for '%s' (%d meshes, %d PCs, %d cached streams)"), *Filename, CachedMeshes.Num(), CachedPointClouds.Num(), CachedStreams.Num());

            if (bIsInitial && !bInitialSpawnDone)
            {
                FilesDownloaded++;
                OnFileProgress.Broadcast(FilesDownloaded, FilesTotal);
                if (FileIndex >= 0)
                {
                    NextSpawnIndex = FileIndex;
                }
            }

            PipelineActive = FMath::Max(0, PipelineActive - 1);

            if (ChangeTracker && !bIsInitial)
            {
                ChangeTracker->SetState(Filename, EJUSYNCFileChangeState::Parsing);
            }

            PendingParseTasks++;
            TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
            const uint64 Generation = (ChangeTracker && !bIsInitial) ? ChangeTracker->GetGeneration(Filename) : 0;

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [WeakThis, Filename, FileIndex, Rank, Generation, HashLo, HashHi, SampleId, Size, bIsInitial, ExpectedLUTVersion,
                  CachedMeshes = MoveTemp(CachedMeshes), CachedPointClouds = MoveTemp(CachedPointClouds), CachedStreams = MoveTemp(CachedStreams), ParseLUT = MoveTemp(ParseLUT)]() mutable
                {
                    FJUSYNCParsedFileResult Result;
                    Result.Filename = Filename;
                    Result.FileIndex = FileIndex;
                    Result.Rank = Rank;
                    Result.Generation = Generation;
                    Result.HashLo = HashLo;
                    Result.HashHi = HashHi;
                    Result.SampleId = SampleId;
                    Result.Size = Size;
                    Result.bParsed = true;
                    Result.LUTVersion = ExpectedLUTVersion;
                    Result.CompactMeshes = MoveTemp(CachedMeshes);
                    Result.bHasCompactMeshes = true;
                    Result.CompactPointClouds = MoveTemp(CachedPointClouds);
                    Result.bHasCompactPointClouds = Result.CompactPointClouds.Num() > 0;
                    Result.PrebuiltStreams = MoveTemp(CachedStreams);

                    if (ParseLUT.Num() > 1)
                    {
                        FJUSYNCUSDLoader::EnsureCompactMeshesBaked(Result.CompactMeshes, ParseLUT, ExpectedLUTVersion);
                    }

                    if (Result.bParsed)
                    {
                        Result.PrebuiltStreams.SetNum(Result.CompactMeshes.Num());
                        for (int32 StreamIdx = 0; StreamIdx < Result.CompactMeshes.Num(); ++StreamIdx)
                        {
                            if (Result.PrebuiltStreams.IsValidIndex(StreamIdx) && Result.PrebuiltStreams[StreamIdx])
                            {
                                continue;
                            }
                            if (!Result.CompactMeshes[StreamIdx] || !Result.CompactMeshes[StreamIdx]->IsValid())
                            {
                                continue;
                            }

                            TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet> Streams = MakeUnique<RealtimeMesh::FRealtimeMeshStreamSet>();
                            if (JUSYNCBuildRealtimeMeshStreams(*Result.CompactMeshes[StreamIdx], *Streams))
                            {
                                Result.PrebuiltStreams[StreamIdx] = MoveTemp(Streams);
                            }
                        }
                    }

                    FFunctionGraphTask::CreateAndDispatchWhenReady(
                        [WeakThis, Result = MoveTemp(Result)]() mutable
                        {
                            if (WeakThis.IsValid())
                            {
                                WeakThis->ApplyParsedFileData(MoveTemp(Result));
                            }
                        },
                        TStatId(), nullptr, ENamedThreads::GameThread);
                },
                TStatId(), nullptr, ENamedThreads::AnyBackgroundThreadNormalTask);

            if (bIsInitial)
            {
                PipelineDownloadNext(Subsystem);
            }
            else
            {
                ChainRefreshNext();
            }
            return;
        }
    }

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;
    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    TUniquePtr<TArray<uint8>> FileData = MakeUnique<TArray<uint8>>();
    const int32 DynamicTimeout = CalculateDynamicTimeout(Size);
    const uint64 ExpectedSize = static_cast<uint64>(FMath::Max<int64>(0, Size));

    uint64 AbortHandle = 0;
#if WITH_ANARI_USD_MIDDLEWARE
    if (!bIsInitial && ChangeTracker && ExpectedSize > 0)
    {
        AbortHandle = JusyncAbortCreate_C();
        if (AbortHandle != 0)
        {
            ActiveDownloadAborts.Add(Filename, AbortHandle);
        }
    }
#endif

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis, Filename, Rank, Size, HashLo, HashHi, FileIndex, bIsInitial, SampleId, DynamicTimeout, ExpectedSize, AbortHandle, FileData = MoveTemp(FileData)]() mutable
    {
        if (!WeakSubsystem.IsValid() || !WeakThis.IsValid())
        {
#if WITH_ANARI_USD_MIDDLEWARE
            if (AbortHandle != 0)
            {
                JusyncAbortRelease_C(AbortHandle);  // teardown race: free the token anyway
            }
#endif
            return;
        }

        bool bSuccess = WeakSubsystem->RequestFileSized(Filename, Rank, DynamicTimeout, *FileData, ExpectedSize, AbortHandle);

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakThis, Filename, Rank, Size, HashLo, HashHi, FileIndex, bIsInitial, SampleId, AbortHandle, FileData = MoveTemp(FileData), bSuccess]() mutable
            {
                if (!WeakThis.IsValid()) return;
                AJUSYNCFileSpawnerActor* Self = WeakThis.Get();
                Self->PipelineActive = FMath::Max(0, Self->PipelineActive - 1);

                if (AbortHandle != 0)
                {
                    uint64* Current = Self->ActiveDownloadAborts.Find(Filename);
                    if (Current && *Current == AbortHandle)
                    {
                        Self->ActiveDownloadAborts.Remove(Filename);
                    }
#if WITH_ANARI_USD_MIDDLEWARE
                    JusyncAbortRelease_C(AbortHandle);
#endif
                }

                if (!bSuccess || !FileData || FileData->Num() == 0)
                {
                    UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: [DOWNLOAD FAILED] '%s' (rank %d)"), *Filename, Rank);
                    if (bIsInitial && !Self->bInitialSpawnDone)
                    {
                        Self->FilesDownloaded++;
                        Self->OnFileProgress.Broadcast(Self->FilesDownloaded, Self->FilesTotal);
                        if (FileIndex >= 0 && !Self->FailedFileIndices.Contains(FileIndex))
                            Self->FailedFileIndices.Add(FileIndex);
                        Self->CheckAllDownloadsComplete();
                    }
                    else
                    {
                        if (Self->ChangeTracker)
                        {
                            Self->ChangeTracker->MarkFailed(Filename);
                        }
                        Self->ChainRefreshNext();
                    }
                    return;
                }

                if (!bIsInitial && Self->ChangeTracker)
                {
                    const FJUSYNCFileChangeRequest* Req = Self->ChangeTracker->Find(Filename);
                    if (Req && Req->bSuperseded)
                    {
                        UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: [LATEST-WINS] '%s' superseded during download - skipping stale parse"), *Filename);
                        Self->ChangeTracker->MarkFailed(Filename);
                        Self->ChainRefreshNext();
                        return;
                    }
                }

                if (bIsInitial && !Self->bInitialSpawnDone)
                {
                    Self->FilesDownloaded++;
                    Self->OnFileProgress.Broadcast(Self->FilesDownloaded, Self->FilesTotal);
                    if (FileIndex >= 0)
                    {
                        Self->NextSpawnIndex = FileIndex;
                    }
                }

                if (Self->ChangeTracker && !bIsInitial)
                {
                    Self->ChangeTracker->SetState(Filename, EJUSYNCFileChangeState::Parsing);
                }

                Self->PendingParseTasks++;

                TArray<FColor> ParseLUT;
                uint64 ParseLUTVersion = 0;
                if (Self->bGradientReady.load() && !Self->bMeshTextureReady)
                {
                    if (UJUSYNCSubsystem* GrdSub = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
                    {
                        if (FJUSYNCPointCloudSpawner* PCSpawner = GrdSub->GetPointCloudSpawner())
                        {
                            ParseLUT = PCSpawner->GetGradientLUT();
                            ParseLUTVersion = PCSpawner->GetLUTVersion();
                        }
                    }
                }

                const uint64 Generation = (Self->ChangeTracker && !bIsInitial) ? Self->ChangeTracker->GetGeneration(Filename) : 0;
                TArray<uint8> Buffer = MoveTemp(*FileData);

                FJUSYNCUSDLoader::ParseAsync(
                    UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem(),
                    MoveTemp(Buffer),
                    Filename,
                    MoveTemp(ParseLUT),
                    ParseLUTVersion,
                    true,
                    FileIndex,
                    Rank,
                    Generation,
                    HashLo,
                    HashHi,
                    SampleId,
                    Size,
                    [WeakThis, bIsInitial](FJUSYNCParsedFileResult&& ParsedResult) mutable
                    {
                        if (WeakThis.IsValid())
                        {
                            WeakThis->ApplyParsedFileData(MoveTemp(ParsedResult));
                        }
                    });

                if (bIsInitial)
                {
                    if (UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
                    {
                        Self->PipelineDownloadNext(S);
                    }
                }
                else
                {
                    Self->ChainRefreshNext();
                }
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
}

void AJUSYNCFileSpawnerActor::ApplyParsedFileData(FJUSYNCParsedFileResult&& Result)
{
    const double ApplyStart = FPlatformTime::Seconds();

    if (bIsCancelled)
    {
        PendingParseTasks = FMath::Max(0, PendingParseTasks - 1);
        return;
    }

    const FString Filename = Result.Filename;
    const bool bIsInitial = !bInitialSpawnDone;

    if (!bIsInitial && ChangeTracker && !ChangeTracker->Find(Filename))
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Discarding parsed result for '%s' (request superseded/cancelled)"), *Filename);
        PendingParseTasks = FMath::Max(0, PendingParseTasks - 1);
        ChainRefreshNext();
        return;
    }

    if (bIsInitial && bEnableBenchmarking)
    {
        if (Result.bHasCompactPointClouds)
        {
            for (const FJUSYNCPointCloudRef& PC : Result.CompactPointClouds)
            {
                if (PC) BenchmarkSceneVertices += PC->GetPointCount();
            }
        }
        else
        {
            for (const FJUSYNCPointCloudData& PC : Result.PointClouds)
            {
                BenchmarkSceneVertices += PC.GetPointCount();
            }
        }
        if (Result.bHasCompactMeshes)
        {
            for (const FJUSYNCCompactMeshRef& M : Result.CompactMeshes)
            {
                if (M)
                {
                    BenchmarkSceneVertices += M->GetVertexCount();
                    BenchmarkSceneTriangles += M->GetTriangleCount();
                }
            }
        }
        else
        {
            for (const FJUSYNCMeshData& Mesh : Result.Meshes)
            {
                BenchmarkSceneVertices += Mesh.GetVertexCount();
                BenchmarkSceneTriangles += Mesh.GetTriangleCount();
            }
        }
    }

    if (!bIsInitial && ChangeTracker)
    {
        const FJUSYNCFileChangeRequest* Request = ChangeTracker->Find(Filename);
        if (Request &&
            (Result.HashLo != 0 || Result.HashHi != 0) &&
            (Request->HashLo != Result.HashLo || Request->HashHi != Result.HashHi))
        {
            UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Discarding stale parsed result for '%s' (hash superseded)"), *Filename);
            PendingParseTasks = FMath::Max(0, PendingParseTasks - 1);
            ChangeTracker->MarkCompleted(Filename);
            ChainRefreshNext();
            return;
        }
    }

    if (ChangeTracker && !bIsInitial)
    {
        ChangeTracker->SetState(Filename, EJUSYNCFileChangeState::Spawning);
    }

    const bool bHasTransientMesh = Result.CompactMeshes.ContainsByPredicate(
        [](const FJUSYNCCompactMeshRef& Mesh) { return Mesh.IsValid() && Mesh->bTransient; });

    if (bEnableMeshCache && MeshCache && Result.bParsed && !bHasTransientMesh &&
        (Result.CompactMeshes.Num() > 0 || Result.Meshes.Num() > 0 || Result.CompactPointClouds.Num() > 0) &&
        !(Result.CompactPointClouds.Num() > 0 && !bCachePointClouds))
    {
        MeshCache->Store(Filename, Result.HashLo, Result.HashHi, Result.Size, Result.LUTVersion, Result.CompactMeshes, Result.CompactPointClouds, &Result.PrebuiltStreams);
    }

    const uint64 EffectiveSampleId = CacheParsedSample(Result, bIsInitial);
    IngestPathlineSample(Result, EffectiveSampleId);
    if (PathlineManager && !bSamplePlaybackActive && !bApplyingCachedSample)
    {
        PathlineManager->SetDisplaySampleId(MAX_uint64);
    }

    if (!bIsInitial && bSamplePlaybackActive && bPauseLiveApplyDuringPlayback && !bApplyingCachedSample)
    {
        UE_LOG(LogTemp, Log, TEXT("[SampleCache] Cached '%s' without applying (scrubbing; %d sample(s) cached)"),
            *Filename, CachedSampleCount);
        PendingParseTasks = FMath::Max(0, PendingParseTasks - 1);
        if (ChangeTracker)
        {
            ChangeTracker->MarkCompleted(Filename);
        }
        ChainRefreshNext();
        return;
    }

    TArray<AActor*> OldMeshActors;
    if (TArray<AActor*>* pOldMeshActors = FilenameToActors.Find(Filename))
    {
        OldMeshActors.Append(*pOldMeshActors);
    }

    TArray<AActor*> OldPCActors;
    if (TArray<AActor*>* pOldPCActors = FilenameToPCActors.Find(Filename))
    {
        OldPCActors.Append(*pOldPCActors);
    }

    const bool bIsRefresh = bInitialSpawnDone || OldMeshActors.Num() > 0 || OldPCActors.Num() > 0;

    if (Result.bParsed && Result.CompactMeshes.Num() > 0)
    {
        int32 ValidMeshCount = 0;
        for (const FJUSYNCCompactMeshRef& M : Result.CompactMeshes)
        {
            if (M && M->IsValid()) ValidMeshCount++;
        }

        if (ValidMeshCount > 0)
        {
            TArray<AActor*> ReplacementActors;
            int32 SpawnCount = 0;
            TArray<FJUSYNCCompactMeshRef> RemainingMeshes;
            TArray<TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet>> RemainingStreams;

            for (int32 i = 0; i < Result.CompactMeshes.Num(); ++i)
            {
                if (!Result.CompactMeshes[i] || !Result.CompactMeshes[i]->IsValid())
                {
                    continue;
                }
                if (SpawnCount >= FMath::Max(1, MaxSpawnsPerFrame))
                {
                    RemainingMeshes.Add(MoveTemp(Result.CompactMeshes[i]));
                    if (i < Result.PrebuiltStreams.Num())
                    {
                        RemainingStreams.Add(MoveTemp(Result.PrebuiltStreams[i]));
                    }
                    continue;
                }

                RealtimeMesh::FRealtimeMeshStreamSet* Stream = nullptr;
                if (i < Result.PrebuiltStreams.Num() && Result.PrebuiltStreams[i])
                {
                    Stream = Result.PrebuiltStreams[i].Get();
                }
                AActor* Actor = SpawnOrUpdateMesh(Result.CompactMeshes[i], Filename, bIsRefresh, ReplacementActors, Stream, &OldMeshActors, false, Result.Rank);
                if (Actor)
                {
                    SpawnCount++;
                }
            }

            if (RemainingMeshes.Num() > 0)
            {
                FDeferredSpawnEntry Entry;
                Entry.Meshes = MoveTemp(RemainingMeshes);
                Entry.PrebuiltStreams = MoveTemp(RemainingStreams);
                Entry.Filename = Filename;
                Entry.Rank = Result.Rank;
                Entry.OldActors = MoveTemp(OldMeshActors);
                Entry.ReplacementActors = MoveTemp(ReplacementActors);
                Entry.ExpectedNewMeshes = ValidMeshCount;
                Entry.SpawnedNewMeshes = SpawnCount;
                Entry.Generation = (ChangeTracker && !bIsInitial) ? ChangeTracker->GetGeneration(Filename) : 0;
                DeferredSpawns.Add(MoveTemp(Entry));

                FTimerHandle DummyHandle;
                GetWorldTimerManager().SetTimer(DummyHandle, FTimerDelegate::CreateUObject(this, &AJUSYNCFileSpawnerActor::ProcessDeferredSpawns), 0.02f, false);
            }
            else if (bIsRefresh && OldMeshActors.Num() > 0)
            {
                DestroyUnreplacedMeshActors(Filename, OldMeshActors, ReplacementActors);
            }

            UE_LOG(LogTemp, Verbose, TEXT("[Spawner] %s: spawned/updated %d/%d meshes (%d deferred)"),
                *Filename, SpawnCount, Result.CompactMeshes.Num(), RemainingMeshes.Num());
        }
    }

    TSet<FString> NewPCKeys;
    if (bSpawnPointClouds && Result.CompactPointClouds.Num() > 0)
    {
        int32 ValidPCCount = 0;
        for (const FJUSYNCPointCloudRef& PC : Result.CompactPointClouds)
        {
            if (PC && PC->IsValid()) ValidPCCount++;
        }

        if (ValidPCCount > 0)
        {
            if (UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
            {
                    if (FJUSYNCPointCloudSpawner* Spawner = S->GetPointCloudSpawner())
                    {
                        ApplyPointCloudSettingsToSpawner(Spawner);

                        FVector PCLoc = GetNextSpawnLocation();
                        float PCScale = bUseUniformScaling ? SpawnScale.X : 1.0f;
                        if (bFitToVolume && FitVolume)
                        {
                            for (const FJUSYNCPointCloudRef& PC : Result.CompactPointClouds)
                            {
                                if (PC && PC->IsValid())
                                {
                                    AccumulateFitBounds(FBox(PC->BoundingBoxMin, PC->BoundingBoxMax));
                                }
                            }
                            const FVector FitScale = ComputeFitScale(FitUnionBounds);
                            if (!FitScale.IsZero())
                            {
                                PCScale = FitScale.X;
                                if (bCenterInVolume)
                                {
                                    PCLoc = ComputeFitLocation(FitUnionBounds, FitScale);
                                }
                            }
                        }
                        Spawner->SetSpawnLocation(PCLoc);
                        Spawner->SetSpawnScale(PCScale);

                    for (FJUSYNCPointCloudRef& PC : Result.CompactPointClouds)
                    {
                        if (!PC || !PC->IsValid())
                        {
                            continue;
                        }
                        if (Spawner->IsElementFiltered(PC->ElementName))
                        {
                            UE_LOG(LogTemp, Log, TEXT("[Spawner] SKIPPED filtered PC '%s' (%d points) from '%s'"),
                                *PC->ElementName, PC->PointCount, *Filename);
                            continue;
                        }
                        const FString PCKey = JUSYNCElementNames::MakeRankedElementKey(
                            JUSYNCCanonicalPointCloudElement(PC->ElementName), Result.Rank);
                        NewPCKeys.Add(PCKey);
                        PCElementToFilename.Add(PCKey, Filename);
                        PendingAsyncPCS++;
                        if (!Spawner->EnqueuePointCloud(MoveTemp(PC), Result.Rank))
                        {
                            PendingAsyncPCS = FMath::Max(0, PendingAsyncPCS - 1);
                        }
                    }

                    UE_LOG(LogTemp, Log, TEXT("[Spawner] Dispatched %d PCs from '%s'"), ValidPCCount, *Filename);
                }
            }
        }
    }

    DestroyUnreplacedPCActors(Filename, OldPCActors, NewPCKeys);

    DeduplicateLogicalActors();

    if (!Result.bParsed || (Result.CompactMeshes.Num() == 0 && Result.CompactPointClouds.Num() == 0))
    {
        if (bIsInitial && !bInitialSpawnDone)
        {
            if (Result.FileIndex >= 0 && !ParseFailedIndices.Contains(Result.FileIndex))
            {
                ParseFailedIndices.Add(Result.FileIndex);
            }
            CheckAllDownloadsComplete();
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Parse returned no data for: '%s' â€” keeping existing actors"), *Filename);
        }
    }
    else if (!bIsInitial)
    {
        UE_LOG(LogTemp, Display, TEXT("[Refresh] Applied parsed data for '%s'"), *Filename);
    }

    PendingParseTasks = FMath::Max(0, PendingParseTasks - 1);

    if (bEnablePerfLogging)
    {
        UE_LOG(LogTemp, Display, TEXT("JUSYNC PERF ApplyParsedFile '%s' %d meshes %d PCs %.3f ms"),
                *Filename, Result.CompactMeshes.Num(), Result.CompactPointClouds.Num(), (FPlatformTime::Seconds() - ApplyStart) * 1000.0);
    }

    if (!bIsInitial)
    {
        if (ChangeTracker)
        {
            ChangeTracker->MarkCompleted(Filename);
        }
        ChainRefreshNext();
    }
    else
    {
        CheckAllDownloadsComplete();
    }
}

double AJUSYNCFileSpawnerActor::LookupSampleTime(uint64 InSampleId) const
{
    for (const TPair<FString, TArray<FJUSYNCSampleCacheEntry>>& Pair : SampleCache)
    {
        for (const FJUSYNCSampleCacheEntry& Entry : Pair.Value)
        {
            if (Entry.SampleId == InSampleId && Entry.SampleTime >= 0.0)
            {
                return Entry.SampleTime;
            }
        }
    }

    if (const double* SenderTime = SampleTimeById.Find(InSampleId))
    {
        return *SenderTime;
    }
    return -1.0;
}

void AJUSYNCFileSpawnerActor::GetSampleTimeline(TArray<uint64>& OutSampleIds) const
{
    TSet<uint64> Seen;
    TMap<uint64, double> Times;
    for (const TPair<FString, TArray<FJUSYNCSampleCacheEntry>>& Pair : SampleCache)
    {
        for (const FJUSYNCSampleCacheEntry& Entry : Pair.Value)
        {
            if (Entry.SampleId != 0)
            {
                Seen.Add(Entry.SampleId);
                if (Entry.SampleTime >= 0.0)
                {
                    Times.FindOrAdd(Entry.SampleId, Entry.SampleTime);
                }
            }
        }
    }

    OutSampleIds.Reset(Seen.Num());
    for (uint64 SampleId : Seen)
    {
        OutSampleIds.Add(SampleId);
        if (!Times.Contains(SampleId))
        {
            if (const double* SenderTime = SampleTimeById.Find(SampleId))
            {
                Times.Add(SampleId, *SenderTime);
            }
        }
    }

    OutSampleIds.Sort([&Times](uint64 A, uint64 B)
    {
        const double* TimeA = Times.Find(A);
        const double* TimeB = Times.Find(B);
        const bool bTimedA = TimeA && *TimeA >= 0.0;
        const bool bTimedB = TimeB && *TimeB >= 0.0;
        if (bTimedA != bTimedB)
        {
            return bTimedA;
        }
        if (bTimedA && !FMath::IsNearlyEqual(*TimeA, *TimeB))
        {
            return *TimeA < *TimeB;
        }
        return A < B;
    });
}

int32 AJUSYNCFileSpawnerActor::GetMaxSampleCacheCount() const
{
    TArray<uint64> Timeline;
    GetSampleTimeline(Timeline);
    return Timeline.Num();
}

double AJUSYNCFileSpawnerActor::GetCachedSampleTime(int32 Index) const
{
    TArray<uint64> Timeline;
    GetSampleTimeline(Timeline);
    if (!Timeline.IsValidIndex(Index))
    {
        return -1.0;
    }

    return LookupSampleTime(Timeline[Index]);
}

const AJUSYNCFileSpawnerActor::FJUSYNCSampleCacheEntry* AJUSYNCFileSpawnerActor::FindSampleForPlayback(
    const TArray<FJUSYNCSampleCacheEntry>& List, uint64 SelectedSampleId) const
{
    const FJUSYNCSampleCacheEntry* Best = nullptr;
    for (const FJUSYNCSampleCacheEntry& Entry : List)
    {
        if (Entry.SampleId == 0 || Entry.SampleId > SelectedSampleId)
        {
            continue;
        }

        if (!Best || Entry.SampleId >= Best->SampleId)
        {
            Best = &Entry;
        }
    }

    if (Best && Best->SampleId != SelectedSampleId)
    {
        return nullptr;
    }
    return Best;
}

void AJUSYNCFileSpawnerActor::RefreshPlaybackWindow()
{
    CachedSampleCount = GetMaxSampleCacheCount();

    if (bSamplePlaybackActive && PlaybackAnchorSampleId != 0)
    {
        TArray<uint64> Timeline;
        GetSampleTimeline(Timeline);
        const int32 FoundIndex = Timeline.IndexOfByKey(PlaybackAnchorSampleId);
        if (FoundIndex != INDEX_NONE)
        {
            PlaybackSampleIndex = FoundIndex;
            PlaybackPosition = Timeline.Num() > 1
                ? static_cast<float>(FoundIndex) / static_cast<float>(Timeline.Num() - 1)
                : 0.0f;
            return;
        }

        PlaybackAnchorSampleId = 0;
        PlaybackSampleIndex = 0;
        PlaybackPosition = 0.0f;
        return;
    }

    if (CachedSampleCount > 0)
    {
        PlaybackPosition = 1.0f;
        PlaybackSampleIndex = CachedSampleCount - 1;
    }
    else
    {
        PlaybackPosition = 1.0f;
        PlaybackSampleIndex = 0;
    }
}

void AJUSYNCFileSpawnerActor::ClearSampleCache()
{
    SampleCache.Empty();
    PendingLiveSamples.Empty();
    SampleCacheLiveFilename.Empty();
    SampleTimeById.Empty();
    NewestCachedSampleTime = -1.0;
    bSenderLoopDetected = false;
    SampleCacheBytes = 0;
    SampleCacheEntryCount = 0;
    NextSampleCacheSerial = 0;
    NextPlaybackSampleId = 0;
    LiveSampleId = 0;
    LastClosedLiveSampleId = 0;
    bLiveSampleFrameClosed = true;
    PlaybackAnchorSampleId = 0;
    CachedSampleCount = 0;
    PlaybackSampleIndex = 0;
    PlaybackPosition = 1.0f;
    bSamplePlaybackActive = false;
    bApplyingCachedSample = false;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr)
    {
        PCSpawner->SetPlaybackMode(false);
        PCSpawner->ClearPlaybackCloudReuse();
    }
    if (PathlineManager)
    {
        PathlineManager->SetDisplaySampleId(MAX_uint64);
    }
}

void AJUSYNCFileSpawnerActor::ClearCachedSamples()
{
    ClearSampleCache();
    UE_LOG(LogTemp, Log, TEXT("[SampleCache] cleared recent-sample cache"));
}

void AJUSYNCFileSpawnerActor::SetPointCloudsVisible(bool bVisible)
{
    bPointCloudsVisible = bVisible;
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr)
    {
        PCSpawner->SetPointCloudsHidden(!bVisible);
    }
}

void AJUSYNCFileSpawnerActor::UpdatePointCloudHueFilter()
{
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr)
    {
        PCSpawner->SetPointHueFilter(bPointCloudHueFilterEnabled, PointCloudHue, PointCloudHueTolerance);
    }
}

void AJUSYNCFileSpawnerActor::ApplyPointCloudDisplaySettings(bool bRecalculateNormals)
{
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr;
    if (!PCSpawner)
    {
        return;
    }

    ApplyPointCloudSettingsToSpawner(PCSpawner);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    PCSpawner->ApplySizingToActiveActors();

    if (bRecalculateNormals)
    {
        PCSpawner->ApplyNormalCalculationToActiveActors();
    }
#endif
}

uint64 AJUSYNCFileSpawnerActor::ResolveLiveSampleId(uint64 SenderTimestamp)
{
    if (!bLiveSampleFrameClosed && LiveSampleId != 0)
    {
        return LiveSampleId;
    }

    LiveSampleId = ++NextPlaybackSampleId;
    const double SenderTime = JUSYNCDecodeSenderTime(SenderTimestamp);
    if (SenderTime >= 0.0)
    {
        SampleTimeById.FindOrAdd(LiveSampleId) = SenderTime;
    }
    bLiveSampleFrameClosed = false;
    return LiveSampleId;
}

FString AJUSYNCFileSpawnerActor::GetSampleCacheDebugString() const
{
    TArray<uint64> Timeline;
    GetSampleTimeline(Timeline);

    uint64 SelectedSampleId = 0;
    if (bSamplePlaybackActive && PlaybackAnchorSampleId != 0)
    {
        SelectedSampleId = PlaybackAnchorSampleId;
    }
    else if (Timeline.Num() > 0)
    {
        SelectedSampleId = Timeline[FMath::Clamp(PlaybackSampleIndex, 0, Timeline.Num() - 1)];
    }

    FString Out = FString::Printf(
        TEXT("pos=%.3f idx=%d active=%d anchor=%llu selected=%llu count=%d pending=%d timeline="),
        PlaybackPosition, PlaybackSampleIndex, bSamplePlaybackActive ? 1 : 0,
        static_cast<unsigned long long>(PlaybackAnchorSampleId),
        static_cast<unsigned long long>(SelectedSampleId),
        CachedSampleCount, PendingLiveSamples.Num());
    for (uint64 Id : Timeline)
    {
        Out += FString::Printf(TEXT("%llu "), static_cast<unsigned long long>(Id));
    }
    Out += TEXT("\n");

    for (const TPair<FString, TArray<FJUSYNCSampleCacheEntry>>& Pair : SampleCache)
    {
        Out += FString::Printf(TEXT("KEY %s live=%s\n"),
            *Pair.Key, *SampleCacheLiveFilename.FindRef(Pair.Key));
        for (const FJUSYNCSampleCacheEntry& Entry : Pair.Value)
        {
            Out += FString::Printf(TEXT("  id=%llu t=%.4f file=%s rank=%d pts=%d verts=%d\n"),
                static_cast<unsigned long long>(Entry.SampleId),
                Entry.SampleTime,
                *Entry.Filename,
                Entry.Rank,
                Entry.PointCloud ? Entry.PointCloud->GetPointCount() : 0,
                Entry.Mesh ? Entry.Mesh->GetVertexCount() : 0);
        }

        const FJUSYNCSampleCacheEntry* Chosen = FindSampleForPlayback(Pair.Value, SelectedSampleId);
        const FString ChosenFilename = Chosen ? Chosen->Filename : FString(TEXT("-"));
        Out += FString::Printf(TEXT("  chosen=%s id=%llu file=%s\n"),
            Chosen ? TEXT("yes") : TEXT("none"),
            Chosen ? static_cast<unsigned long long>(Chosen->SampleId) : 0ULL,
            *ChosenFilename);
    }

    int32 ActorLines = 0;
    for (const TPair<FString, AActor*>& Pair : FileToActorMap)
    {
        if (ActorLines++ >= 40)
        {
            break;
        }
        AActor* Actor = Pair.Value;
        Out += FString::Printf(TEXT("ACTOR %s -> %s hidden=%d\n"),
            *Pair.Key,
            Actor && Actor->IsValidLowLevel() ? *Actor->GetName() : TEXT("null"),
            Actor && Actor->IsValidLowLevel() && Actor->IsHidden() ? 1 : 0);
    }
    return Out;
}

void AJUSYNCFileSpawnerActor::TrimSampleCache()
{
    const int64 MaxBytes = GetSampleCacheBudgetBytes();
    while (SampleCacheBytes > MaxBytes && SampleCacheEntryCount > 1)
    {
        FString OldestKey;
        uint64 OldestSerial = TNumericLimits<uint64>::Max();
        for (const TPair<FString, TArray<FJUSYNCSampleCacheEntry>>& Pair : SampleCache)
        {
            if (Pair.Value.Num() == 0)
            {
                continue;
            }
            if (Pair.Value[0].Serial < OldestSerial)
            {
                OldestSerial = Pair.Value[0].Serial;
                OldestKey = Pair.Key;
            }
        }

        if (OldestKey.IsEmpty())
        {
            break;
        }

        TArray<FJUSYNCSampleCacheEntry>* List = SampleCache.Find(OldestKey);
        if (!List || List->Num() == 0)
        {
            break;
        }
        SampleCacheBytes -= (*List)[0].Bytes;
        SampleCacheEntryCount--;
        List->RemoveAt(0);
        if (List->Num() == 0)
        {
            SampleCacheLiveFilename.Remove(OldestKey);
            SampleCache.Remove(OldestKey);
        }
    }

    TArray<uint64> Timeline;
    GetSampleTimeline(Timeline);
    const int32 MaxTimeline = GetSampleCacheCountCap();
    if (Timeline.Num() > MaxTimeline)
    {
        TSet<uint64> DropIds;
        for (int32 Index = 0; Index < Timeline.Num() - MaxTimeline; ++Index)
        {
            DropIds.Add(Timeline[Index]);
        }

        for (auto It = SampleCache.CreateIterator(); It; ++It)
        {
            TArray<FJUSYNCSampleCacheEntry>& List = It.Value();
            for (int32 Index = List.Num() - 1; Index >= 0; --Index)
            {
                if (DropIds.Contains(List[Index].SampleId))
                {
                    SampleCacheBytes -= List[Index].Bytes;
                    SampleCacheEntryCount--;
                    List.RemoveAt(Index);
                }
            }

            if (List.Num() == 0)
            {
                SampleCacheLiveFilename.Remove(It.Key());
                It.RemoveCurrent();
            }
        }
    }

    RefreshPlaybackWindow();
}

void AJUSYNCFileSpawnerActor::AddSampleCacheEntry(const FString& Key, FJUSYNCSampleCacheEntry&& Entry)
{
    if (Key.IsEmpty())
    {
        return;
    }

    if (Entry.SampleId == 0)
    {
        Entry.SampleId = ++NextPlaybackSampleId;
    }

    if (bSamplePlaybackActive && bPauseLiveApplyDuringPlayback)
    {
        PendingLiveSamples.Add(Key, MoveTemp(Entry));
        return;
    }

    TArray<FJUSYNCSampleCacheEntry>& List = SampleCache.FindOrAdd(Key);

    for (int32 Index = 0; Index < List.Num(); ++Index)
    {
        FJUSYNCSampleCacheEntry& Existing = List[Index];
        const bool bSameTime =
            Existing.SampleTime >= 0.0 && Entry.SampleTime >= 0.0 &&
            FMath::IsNearlyEqual(Existing.SampleTime, Entry.SampleTime, 1e-6);
        if (Existing.SampleId != Entry.SampleId && !bSameTime)
        {
            continue;
        }

        const bool bSamePayload =
            (Entry.Mesh && Existing.Mesh == Entry.Mesh) ||
            (Entry.PointCloud && Existing.PointCloud == Entry.PointCloud);
        const bool bSameFileHash =
            Entry.Filename == Existing.Filename &&
            (Entry.HashLo != 0 || Entry.HashHi != 0) &&
            Entry.HashLo == Existing.HashLo && Entry.HashHi == Existing.HashHi;
        if (bSamePayload || bSameFileHash)
        {
            return;
        }

        bool bReplace = true;
        if (Entry.PointCloud && Existing.PointCloud)
        {
            bReplace = Entry.PointCloud->GetPointCount() >= Existing.PointCloud->GetPointCount();
        }
        else if (Entry.Mesh && Existing.Mesh)
        {
            bReplace = Entry.Mesh->GetVertexCount() >= Existing.Mesh->GetVertexCount();
        }

        if (!bReplace)
        {
            return;
        }

        SampleCacheBytes -= Existing.Bytes;
        Entry.Serial = Existing.Serial;
        SampleCacheBytes += Entry.Bytes;
        Existing = MoveTemp(Entry);

        if (Existing.SampleTime > NewestCachedSampleTime)
        {
            NewestCachedSampleTime = Existing.SampleTime;
        }

        if (!bSamplePlaybackActive || !bPauseLiveApplyDuringPlayback)
        {
            SampleCacheLiveFilename.Add(Key, Existing.Filename);
        }

        RefreshPlaybackWindow();
        return;
    }

    if (List.Num() > 0 && List.Last().SampleId == Entry.SampleId)
    {
        const FJUSYNCSampleCacheEntry& Last = List.Last();
        const bool bSamePayload =
            (Entry.Mesh && Last.Mesh == Entry.Mesh) ||
            (Entry.PointCloud && Last.PointCloud == Entry.PointCloud);
        const bool bSameFileHash =
            Entry.Filename == Last.Filename &&
            (Entry.HashLo != 0 || Entry.HashHi != 0) &&
            Entry.HashLo == Last.HashLo && Entry.HashHi == Last.HashHi;
        if (bSamePayload || bSameFileHash)
        {
            return;
        }
    }

    Entry.Serial = ++NextSampleCacheSerial;
    SampleCacheBytes += Entry.Bytes;
    SampleCacheEntryCount++;
    List.Add(MoveTemp(Entry));

    if (List.Last().SampleTime > NewestCachedSampleTime)
    {
        NewestCachedSampleTime = List.Last().SampleTime;
    }

    if (!bSamplePlaybackActive || !bPauseLiveApplyDuringPlayback)
    {
        SampleCacheLiveFilename.Add(Key, List.Last().Filename);
    }

    const int32 MaxEntries = GetSampleCacheCountCap();
    while (List.Num() > MaxEntries)
    {
        SampleCacheBytes -= List[0].Bytes;
        SampleCacheEntryCount--;
        List.RemoveAt(0);
    }

    TrimSampleCache();
}

void AJUSYNCFileSpawnerActor::ConsiderSenderLoop(const FString& Filename, uint64 SenderTimestamp)
{
    if (!bEnableSampleCache)
    {
        return;
    }

    double NotifyTime = JUSYNCDecodeSenderTime(SenderTimestamp);
    if (NotifyTime < 0.0)
    {
        NotifyTime = JUSYNCTimeFromFilename(Filename);
    }
    if (NotifyTime < 0.0)
    {
        return;
    }

    TArray<uint64> Timeline;
    GetSampleTimeline(Timeline);
    if (Timeline.Num() < 8)
    {
        return;
    }

    double OldestTime = TNumericLimits<double>::Max();
    double NewestTime = -1.0;
    for (uint64 Id : Timeline)
    {
        const double Time = LookupSampleTime(Id);
        if (Time >= 0.0)
        {
            OldestTime = FMath::Min(OldestTime, Time);
            NewestTime = FMath::Max(NewestTime, Time);
        }
    }

    const double Span = NewestTime - OldestTime;
    if (Span < 1.0)
    {
        return;
    }

    const bool bRestartedNearStart = NotifyTime <= OldestTime + 0.25 * Span;
    const bool bBehindNewest = (NewestTime - NotifyTime) >= 0.5 * Span;
    if (!bRestartedNearStart || !bBehindNewest)
    {
        return;
    }

    if (!bSenderLoopDetected)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[Playback] Sender loop detected: frame t=%.3fs (already applied) arrived while the newest cached sample is t=%.3fs - cached timeline will loop"),
            NotifyTime, NewestTime);
    }
    bSenderLoopDetected = true;
    SenderLoopDetectedSeconds = FPlatformTime::Seconds();
}

bool AJUSYNCFileSpawnerActor::ConsumeSenderLoopDetected()
{
    const bool bWasDetected = bSenderLoopDetected;
    bSenderLoopDetected = false;
    return bWasDetected;
}

bool AJUSYNCFileSpawnerActor::ConsumeSenderLoopDetectedIfRecent(float MaxAgeSeconds)
{
    if (!bSenderLoopDetected)
    {
        return false;
    }

    const bool bRecent = MaxAgeSeconds <= 0.0f
        || (FPlatformTime::Seconds() - SenderLoopDetectedSeconds) <= MaxAgeSeconds;
    bSenderLoopDetected = false;
    return bRecent;
}

uint64 AJUSYNCFileSpawnerActor::CacheParsedSample(const FJUSYNCParsedFileResult& Result, bool bIsInitial)
{
    if (!Result.bParsed)
    {
        return 0;
    }

    uint64 EffectiveSampleId = Result.SampleId;
    if (EffectiveSampleId == 0)
    {
        if (bIsInitial || !bInitialSpawnDone)
        {
            EffectiveSampleId = 1;
            NextPlaybackSampleId = FMath::Max(NextPlaybackSampleId, static_cast<uint64>(1));
        }
        else
        {
            EffectiveSampleId = ++NextPlaybackSampleId;
        }
    }
    else
    {
        NextPlaybackSampleId = FMath::Max(NextPlaybackSampleId, EffectiveSampleId);
    }

    if (!bEnableSampleCache)
    {
        return EffectiveSampleId;
    }

    double EffectiveSampleTime = -1.0;
    if (const double* SentTime = SampleTimeById.Find(EffectiveSampleId))
    {
        EffectiveSampleTime = *SentTime;
    }
    if (EffectiveSampleTime < 0.0)
    {
        for (const FJUSYNCPointCloudRef& PC : Result.CompactPointClouds)
        {
            if (PC && PC->Time >= 0.0)
            {
                EffectiveSampleTime = PC->Time;
                break;
            }
        }
    }
    if (EffectiveSampleTime < 0.0)
    {
        EffectiveSampleTime = JUSYNCTimeFromFilename(Result.Filename);
    }

    const int32 Rank = FMath::Max(Result.Rank, 0);
    for (const FJUSYNCCompactMeshRef& Mesh : Result.CompactMeshes)
    {
        if (!Mesh || !Mesh->IsValid())
        {
            continue;
        }

        FString Element = JUSYNCCanonicalPointCloudElement(Mesh->ElementName);
        if (Element.IsEmpty())
        {
            Element = Mesh->ElementName;
        }

        FJUSYNCSampleCacheEntry Entry;
        Entry.Mesh = Mesh;
        Entry.Filename = Result.Filename;
        Entry.Rank = Rank;
        Entry.Bytes = Mesh->EstimateBytes();
        Entry.SampleId = EffectiveSampleId;
        Entry.SampleTime = EffectiveSampleTime;
        Entry.HashLo = Result.HashLo;
        Entry.HashHi = Result.HashHi;

        const FString Key = FString::Printf(TEXT("%s|mesh|r%d"), *Element, Rank);
        AddSampleCacheEntry(Key, MoveTemp(Entry));
    }

    if (!bSpawnPointClouds && !bEnablePathlines)
    {
        return EffectiveSampleId;
    }

    for (const FJUSYNCPointCloudRef& PointCloud : Result.CompactPointClouds)
    {
        if (!PointCloud || !PointCloud->IsValid())
        {
            continue;
        }

        FString Element = JUSYNCCanonicalPointCloudElement(PointCloud->ElementName);
        if (Element.IsEmpty())
        {
            Element = PointCloud->ElementName;
        }

        int64 Bytes = 128;
        if (PointCloud->HasLidarPoints())
        {
            Bytes += static_cast<int64>(PointCloud->LidarPoints.Num()) * sizeof(FLidarPointCloudPoint);
        }
        else
        {
            Bytes += static_cast<int64>(PointCloud->Positions.Num()) * sizeof(FVector);
            Bytes += static_cast<int64>(PointCloud->Colors.Num()) * sizeof(FColor);
        }
        Bytes += static_cast<int64>(PointCloud->Widths.Num()) * sizeof(float);

        FJUSYNCSampleCacheEntry Entry;
        Entry.PointCloud = PointCloud;
        Entry.Filename = Result.Filename;
        Entry.Rank = Rank;
        Entry.Bytes = Bytes;
        Entry.SampleId = EffectiveSampleId;
        Entry.SampleTime = EffectiveSampleTime;
        Entry.HashLo = Result.HashLo;
        Entry.HashHi = Result.HashHi;

        const FString Key = FString::Printf(TEXT("%s|pc|r%d"), *Element, Rank);
        AddSampleCacheEntry(Key, MoveTemp(Entry));
    }

    return EffectiveSampleId;
}

void AJUSYNCFileSpawnerActor::IngestPathlineSample(
    const FJUSYNCParsedFileResult& Result, uint64 EffectiveSampleId)
{
    if (!bEnablePathlines || !PathlineManager || !Result.bParsed)
    {
        return;
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr;
    if (!PCSpawner)
    {
        return;
    }

    const TArray<FColor> Lut = PCSpawner->GetGradientLUT();
    const int32 Rank = FMath::Max(Result.Rank, 0);
    for (const FJUSYNCPointCloudRef& PointCloud : Result.CompactPointClouds)
    {
        if (!PointCloud.IsValid() || !PointCloud->IsValid() ||
            !PointCloud->bFromRawGeo || !PointCloud->bComplete)
        {
            continue;
        }

        FString Element = JUSYNCCanonicalPointCloudElement(PointCloud->ElementName);
        if (Element.IsEmpty())
        {
            Element = PointCloud->ElementName;
        }
        if (Element.IsEmpty() || PCSpawner->IsElementFiltered(Element))
        {
            continue;
        }

        PathlineManager->EnqueueSample(Element, PointCloud, Rank, EffectiveSampleId, Lut);
    }
}

void AJUSYNCFileSpawnerActor::FlushPendingLiveSamples()
{
    if (PendingLiveSamples.Num() == 0)
    {
        return;
    }

    TMap<FString, FJUSYNCSampleCacheEntry> Pending = MoveTemp(PendingLiveSamples);
    PendingLiveSamples.Empty();

    for (TPair<FString, FJUSYNCSampleCacheEntry>& Pair : Pending)
    {
        SampleCacheLiveFilename.Add(Pair.Key, Pair.Value.Filename);
        AddSampleCacheEntry(Pair.Key, MoveTemp(Pair.Value));
    }
}

void AJUSYNCFileSpawnerActor::ApplyCachedSample(float NormalizedPosition)
{
    if (!bEnableSampleCache || SampleCache.Num() == 0 || bApplyingCachedSample)
    {
        return;
    }

    TArray<uint64> Timeline;
    GetSampleTimeline(Timeline);
    if (Timeline.Num() == 0)
    {
        return;
    }

    const float Position = FMath::Clamp(NormalizedPosition, 0.0f, 1.0f);
    const int32 TimelineIndex = Timeline.Num() <= 1
        ? 0
        : FMath::Clamp(FMath::RoundToInt(Position * static_cast<float>(Timeline.Num() - 1)), 0, Timeline.Num() - 1);
    const uint64 SelectedSampleId = Timeline[TimelineIndex];
    if (PathlineManager)
    {
        const IConsoleVariable* CloudBlend = IConsoleManager::Get().FindConsoleVariable(
            TEXT("jusync.Morph.CloudBlendMs"));
        const int32 PlaybackBlendMs = CloudBlend ? CloudBlend->GetInt() : 0;
        PathlineManager->SetDisplayTipBlendSeconds(
            (bSamplePlaybackActive && PlaybackBlendMs > 0) ? PlaybackBlendMs / 1000.0f : 0.0f);
        PathlineManager->SetDisplaySampleId(SelectedSampleId);
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr;

    bApplyingCachedSample = true;

    IConsoleVariable* MeshBlendCVar = IConsoleManager::Get().FindConsoleVariable(
        TEXT("jusync.Morph.MeshBlendMs"));
    const int32 SavedMeshBlendMs = MeshBlendCVar ? MeshBlendCVar->GetInt() : 0;
    if (MeshBlendCVar)
    {
        MeshBlendCVar->Set(200, ECVF_SetByCode);
    }

    TSet<AActor*> AppliedActors;

    for (TPair<FString, TArray<FJUSYNCSampleCacheEntry>>& Pair : SampleCache)
    {
        const FJUSYNCSampleCacheEntry* Chosen = FindSampleForPlayback(Pair.Value, SelectedSampleId);
        if (!Chosen)
        {
            if (Pair.Key.Contains(TEXT("|mesh|")))
            {
                const FString* LiveFilename = SampleCacheLiveFilename.Find(Pair.Key);
                const FString ActorFilename = (LiveFilename && !LiveFilename->IsEmpty())
                    ? *LiveFilename
                    : (Pair.Value.Num() > 0 ? Pair.Value.Last().Filename : FString());
                if (!ActorFilename.IsEmpty())
                {
                    const FString LogicalKey = JUSYNCBuildLogicalActorKey(ActorFilename);
                    if (!LogicalKey.IsEmpty())
                    {
                        if (AActor* Existing = FileToActorMap.FindRef(LogicalKey))
                        {
                            if (Existing->IsValidLowLevel())
                            {
                                Existing->SetActorHiddenInGame(true);
                            }
                        }
                    }
                }
            }
            else if (Pair.Key.Contains(TEXT("|pc|")) && PCSpawner && Pair.Value.Num() > 0)
            {
                const FJUSYNCSampleCacheEntry& First = Pair.Value[0];
                if (First.PointCloud)
                {
                    const FString PCKey = JUSYNCElementNames::MakeRankedElementKey(
                        JUSYNCCanonicalPointCloudElement(First.PointCloud->ElementName),
                        FMath::Max(First.Rank, 0));
                    if (AActor* Existing = PCSpawner->FindActorByElementKey(PCKey))
                    {
                        if (Existing->IsValidLowLevel())
                        {
                            Existing->SetActorHiddenInGame(true);
                        }
                    }
                }
            }
            continue;
        }

        if (Chosen->Mesh)
        {
            const FString* LiveFilename = SampleCacheLiveFilename.Find(Pair.Key);
            const FString ActorFilename = (LiveFilename && !LiveFilename->IsEmpty())
                ? *LiveFilename
                : Chosen->Filename;

            const FString LogicalKey = JUSYNCBuildLogicalActorKey(ActorFilename);
            if (!LogicalKey.IsEmpty())
            {
                if (AActor* Existing = FileToActorMap.FindRef(LogicalKey))
                {
                    if (Existing->IsValidLowLevel())
                    {
                        Existing->SetActorHiddenInGame(false);
                    }
                }
            }

            TArray<AActor*> ReplacementActors;
            FJUSYNCCompactMeshRef Mesh = Chosen->Mesh;
            SpawnOrUpdateMesh(Mesh, ActorFilename, true, ReplacementActors, nullptr, nullptr, true, Chosen->Rank, true);

            for (AActor* Applied : ReplacementActors)
            {
                if (Applied)
                {
                    AppliedActors.Add(Applied);
                }
            }
            if (!LogicalKey.IsEmpty())
            {
                if (AActor* Applied = FileToActorMap.FindRef(LogicalKey))
                {
                    AppliedActors.Add(Applied);
                }
            }
        }
        else if (Chosen->PointCloud && bSpawnPointClouds && PCSpawner)
        {
            if (PCSpawner->IsElementFiltered(Chosen->PointCloud->ElementName))
            {
                continue;
            }

            const FString PCKey = JUSYNCElementNames::MakeRankedElementKey(
                JUSYNCCanonicalPointCloudElement(Chosen->PointCloud->ElementName), FMath::Max(Chosen->Rank, 0));
            if (AActor* Existing = PCSpawner->FindActorByElementKey(PCKey))
            {
                if (Existing->IsValidLowLevel())
                {
                    Existing->SetActorHiddenInGame(false);
                }
            }

            if (bSamplePlaybackActive)
            {
                PCSpawner->PreparePointCloudForPlayback(PCKey);
            }

            FJUSYNCPointCloudRef PointCloud = Chosen->PointCloud;
            if (bSamplePlaybackActive)
            {
                PCSpawner->EnqueuePointCloudForPlayback(MoveTemp(PointCloud), Chosen->Rank);
            }
            else
            {
                PCSpawner->EnqueuePointCloud(MoveTemp(PointCloud), Chosen->Rank);
            }

            if (AActor* Applied = PCSpawner->FindActorByElementKey(PCKey))
            {
                AppliedActors.Add(Applied);
            }
        }
    }

    HideUnselectedPlaybackActors(AppliedActors);

    DeduplicateLogicalActors();

    if (MeshBlendCVar)
    {
        MeshBlendCVar->Set(SavedMeshBlendMs, ECVF_SetByCode);
    }

    bApplyingCachedSample = false;
    PlaybackAnchorSampleId = SelectedSampleId;
    PlaybackSampleIndex = TimelineIndex;
    CachedSampleCount = Timeline.Num();
}

void AJUSYNCFileSpawnerActor::HideUnselectedPlaybackActors(const TSet<AActor*>& KeptActors)
{
    for (const TPair<FString, TArray<AActor*>>& Pair : FilenameToActors)
    {
        for (AActor* Actor : Pair.Value)
        {
            if (Actor && Actor->IsValidLowLevel() && !KeptActors.Contains(Actor))
            {
                Actor->SetActorHiddenInGame(true);
            }
        }
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr;
    if (!PCSpawner || !GetWorld())
    {
        return;
    }

#if WITH_ANARI_USD_MIDDLEWARE
    for (TActorIterator<ALidarPointCloudActor> It(GetWorld()); It; ++It)
    {
        ALidarPointCloudActor* Actor = *It;
        if (Actor && !KeptActors.Contains(Actor) && !PCSpawner->GetElementKeyForActor(Actor).IsEmpty())
        {
            Actor->SetActorHiddenInGame(true);
        }
    }
#endif
}

void AJUSYNCFileSpawnerActor::SetPlaybackPosition(float NewPosition)
{
    PlaybackPosition = FMath::Clamp(NewPosition, 0.0f, 1.0f);

    if (!bInitialSpawnDone)
    {
        bSamplePlaybackActive = false;
        return;
    }

    if (!bEnableSampleCache || GetMaxSampleCacheCount() <= 0)
    {
        return;
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr;

    bSamplePlaybackActive = PlaybackPosition < 0.999f;
    if (!bSamplePlaybackActive)
    {
        if (PCSpawner)
        {
            PCSpawner->SetPlaybackMode(false);
        }

        FlushPendingLiveSamples();
    }
    else
    {
        if (PCSpawner)
        {
            PCSpawner->EnterPlaybackMode();
        }

        DeferredSpawns.Empty();
        GradientDeferredMeshes.Empty();
        HiddenPendingMeshUpdates.Empty();
        if (AnimationController)
        {
            AnimationController->Pause();
        }
    }

    ApplyCachedSample(PlaybackPosition);
    if (!bSamplePlaybackActive)
    {
        if (PathlineManager)
        {
            PathlineManager->SetDisplaySampleId(MAX_uint64);
        }
        PlaybackSampleIndex = FMath::Max(0, CachedSampleCount - 1);
    }
}

void AJUSYNCFileSpawnerActor::PlaybackPreviousSample()
{
    const int32 Count = CachedSampleCount > 0 ? CachedSampleCount : GetMaxSampleCacheCount();
    if (Count <= 1)
    {
        return;
    }

    const int32 Index = FMath::Clamp(PlaybackSampleIndex - 1, 0, Count - 1);
    SetPlaybackPosition(static_cast<float>(Index) / static_cast<float>(Count - 1));
}

void AJUSYNCFileSpawnerActor::PlaybackNextSample()
{
    const int32 Count = CachedSampleCount > 0 ? CachedSampleCount : GetMaxSampleCacheCount();
    if (Count <= 1)
    {
        return;
    }

    const int32 Index = FMath::Clamp(PlaybackSampleIndex + 1, 0, Count - 1);
    SetPlaybackPosition(static_cast<float>(Index) / static_cast<float>(Count - 1));
}

void AJUSYNCFileSpawnerActor::FollowLatestSample()
{
    bSamplePlaybackActive = false;
    PlaybackPosition = 1.0f;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr)
    {
        PCSpawner->SetPlaybackMode(false);
    }

    FlushPendingLiveSamples();
    ApplyCachedSample(1.0f);
    if (PathlineManager)
    {
        PathlineManager->SetDisplaySampleId(MAX_uint64);
    }
    PlaybackSampleIndex = FMath::Max(0, CachedSampleCount - 1);
}

#if WITH_EDITOR
void AJUSYNCFileSpawnerActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    const FName PropertyName = PropertyChangedEvent.GetPropertyName();
    if (PropertyName == GET_MEMBER_NAME_CHECKED(AJUSYNCFileSpawnerActor, PlaybackPosition))
    {
        if (bEnableSampleCache && GetMaxSampleCacheCount() > 0)
        {
            SetPlaybackPosition(PlaybackPosition);
        }
    }
    else if (PropertyName == GET_MEMBER_NAME_CHECKED(AJUSYNCFileSpawnerActor, bEnableSampleCache))
    {
        if (!bEnableSampleCache)
        {
            ClearSampleCache();
        }
    }
    else if (PropertyName == GET_MEMBER_NAME_CHECKED(AJUSYNCFileSpawnerActor, bPauseLiveApplyDuringPlayback))
    {
        if (!bPauseLiveApplyDuringPlayback)
        {
            FlushPendingLiveSamples();
        }
    }
    else if (PropertyName == GET_MEMBER_NAME_CHECKED(AJUSYNCFileSpawnerActor, SampleCacheSize))
    {
        const int32 MaxEntries = GetSampleCacheCountCap();
        for (TPair<FString, TArray<FJUSYNCSampleCacheEntry>>& Pair : SampleCache)
        {
            while (Pair.Value.Num() > MaxEntries)
            {
                SampleCacheBytes -= Pair.Value[0].Bytes;
                SampleCacheEntryCount--;
                Pair.Value.RemoveAt(0);
            }
        }
        TrimSampleCache();
        CachedSampleCount = GetMaxSampleCacheCount();
    }
}
#endif

void AJUSYNCFileSpawnerActor::ApplyPointCloudSettingsToSpawner(FJUSYNCPointCloudSpawner* Spawner)
{
    if (!Spawner)
    {
        return;
    }

    Spawner->SetMaxPoolSize(PointCloudPoolSize);
    Spawner->SetPointSize(PointCloudSize);
    Spawner->SetPointSizeBias(PointSizeBias);
    Spawner->SetGapFillingStrength(GapFillingStrength);
    Spawner->SetPerfLogging(bEnablePerfLogging);
    Spawner->SetGradientColorsEnabled(bUseGradientColors);
    Spawner->SetNormalCalculation(
        bCalculatePointCloudNormals,
        PointCloudNormalsMaxPoints,
        PointCloudNormalsQuality,
        PointCloudNormalsNoiseTolerance,
        PointCloudNormalsCooldownSeconds);

#ifdef WITH_ANARI_USD_MIDDLEWARE
    switch (PointShape)
    {
        case EJUSYNCPointShape::Square:
            Spawner->SetPointShape(ELidarPointCloudSpriteShape::Square);
            break;
        case EJUSYNCPointShape::Circle:
        default:
            Spawner->SetPointShape(ELidarPointCloudSpriteShape::Circle);
            break;
    }

    switch (PointOrientation)
    {
        case EJUSYNCPointOrientation::FacingNormal:
            Spawner->SetPointOrientation(ELidarPointCloudSpriteOrientation::PreferFacingNormal);
            break;
        case EJUSYNCPointOrientation::FacingCamera:
        default:
            Spawner->SetPointOrientation(ELidarPointCloudSpriteOrientation::PreferFacingCamera);
            break;
    }

    switch (PointScaling)
    {
        case EJUSYNCPointScaling::PerNode:
            Spawner->SetPointScaling(ELidarPointCloudScalingMethod::PerNode);
            break;
        case EJUSYNCPointScaling::PerPoint:
            Spawner->SetPointScaling(ELidarPointCloudScalingMethod::PerPoint);
            break;
        case EJUSYNCPointScaling::FixedScreenSize:
            Spawner->SetPointScaling(ELidarPointCloudScalingMethod::FixedScreenSize);
            break;
        case EJUSYNCPointScaling::PerNodeAdaptive:
        default:
            Spawner->SetPointScaling(ELidarPointCloudScalingMethod::PerNodeAdaptive);
            break;
    }
#endif
}

void AJUSYNCFileSpawnerActor::FlushHiddenMeshUpdates()
{
    if (HiddenPendingMeshUpdates.Num() == 0)
    {
        return;
    }

    TArray<AActor*> Ready;
    for (const TPair<AActor*, FRecolorMeshEntry>& Pair : HiddenPendingMeshUpdates)
    {
        if (!Pair.Key || !Pair.Key->IsValidLowLevel() || !Pair.Key->IsHidden())
        {
            Ready.Add(Pair.Key);
        }
    }

    if (Ready.Num() == 0)
    {
        return;
    }

    int32 Processed = 0;
    const int32 MaxUpdatesThisFrame = FMath::Max(1, MaxSpawnsPerFrame);
    for (AActor* Actor : Ready)
    {
        if (Processed >= MaxUpdatesThisFrame)
        {
            break;
        }

        FRecolorMeshEntry* PendingPtr = HiddenPendingMeshUpdates.Find(Actor);
        if (!PendingPtr)
        {
            continue;
        }
        FRecolorMeshEntry Pending = MoveTemp(*PendingPtr);
        HiddenPendingMeshUpdates.Remove(Actor);

        if (Actor && Actor->IsValidLowLevel())
        {
            TArray<AActor*> ReplacementActors;
            SpawnOrUpdateMesh(Pending.Mesh, Pending.Filename, true, ReplacementActors, nullptr, nullptr, false, Pending.Rank);
            Processed++;
        }
    }
}

static FORCEINLINE void JUSYNC_DIAG_MARK(const FString& File, int32 Stage, uint64 Bytes = 0)
{
#if WITH_ANARI_USD_MIDDLEWARE
    if (!File.IsEmpty())
    {
        JusyncDiagMark_C(TCHAR_TO_UTF8(*File), Stage, Bytes, 0, 0);
    }
#else
    (void)File; (void)Stage; (void)Bytes;
#endif
}

AActor* AJUSYNCFileSpawnerActor::SpawnOrUpdateMesh(FJUSYNCCompactMeshRef& Mesh, const FString& Filename, bool bIsRefresh, TArray<AActor*>& ReplacementActors, RealtimeMesh::FRealtimeMeshStreamSet* PrebuiltStreams, const TArray<AActor*>* OldMeshActors, bool bForceGradientSpawn, int32 Rank, bool bForceApplyHidden)
{
    if (!Mesh || !Mesh->IsValid())
    {
        return nullptr;
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        return nullptr;
    }

    const FString Key = Mesh->ElementName + TEXT("|") + Filename;
    const FString LogicalKey = JUSYNCBuildLogicalActorKey(Filename);
    AActor* Existing = FileToActorMap.FindRef(Key);
    AActor* LogicalExisting = LogicalKey.IsEmpty() ? nullptr : FileToActorMap.FindRef(LogicalKey);
    bool bCanReuse = bIsRefresh && Existing && Existing->IsValidLowLevel();

    if (LogicalExisting && LogicalExisting->IsValidLowLevel() && LogicalExisting != Existing)
    {
        Existing = LogicalExisting;
        bCanReuse = true;
        UE_LOG(LogTemp, Verbose, TEXT("[Spawner] Using logical actor for '%s' (logical '%s')"),
            *Filename, *LogicalKey);
    }
    else if (!Existing && LogicalExisting && LogicalExisting->IsValidLowLevel())
    {
        Existing = LogicalExisting;
        bCanReuse = true;
        UE_LOG(LogTemp, Verbose, TEXT("[Spawner] Reusing actor for logical '%s' (new key '%s')"),
            *LogicalKey, *Key);
    }

    if (bCanReuse && Existing && Existing->IsValidLowLevel() && Existing->IsHidden())
    {
        if (!bForceApplyHidden)
        {
            RegisterMeshForLUTRecolor(Existing, Mesh, Filename, Existing->GetActorLocation());

            FRecolorMeshEntry Pending;
            Pending.Mesh = MoveTemp(Mesh);
            Pending.Filename = Filename;
            Pending.Rank = Rank;
            Pending.Loc = Existing->GetActorLocation();
            Pending.Rot = Existing->GetActorRotation();
            Pending.Scale = Existing->GetActorScale3D();
            HiddenPendingMeshUpdates.Add(Existing, MoveTemp(Pending));

            if (!ReplacementActors.Contains(Existing))
            {
                ReplacementActors.Add(Existing);
            }

            UE_LOG(LogTemp, Verbose, TEXT("[Spawner] Deferred hidden mesh update for '%s'"), *Filename);
            return Existing;
        }

        Existing->SetActorHiddenInGame(false);
    }

    const bool bGradientExpected = !bForceGradientSpawn &&
        !bMeshTextureReady &&
        !bGradientReady.load() &&
        GradientPngRankMap.Num() > 0 &&
        Mesh->HasUVs();

    if (bGradientExpected)
    {
        if (Existing && Existing->IsValidLowLevel())
        {
            if (!ReplacementActors.Contains(Existing))
            {
                ReplacementActors.Add(Existing);
            }
            TArray<AActor*>& DeferredReplacements = GradientDeferredReplacementActors.FindOrAdd(Filename);
            if (!DeferredReplacements.Contains(Existing))
            {
                DeferredReplacements.Add(Existing);
            }
        }

        FJUSYNCGradientDeferredMeshEntry Deferred;
        Deferred.Mesh = Mesh;
        Deferred.Filename = Filename;
        Deferred.Rank = Rank;
        Deferred.bIsRefresh = bIsRefresh;
        Deferred.OldActors = OldMeshActors ? *OldMeshActors : TArray<AActor*>();
        Deferred.ExpectedNewMeshes = 1;
        Deferred.SpawnedNewMeshes = 0;
        Deferred.Generation = (ChangeTracker && bIsRefresh) ? ChangeTracker->GetGeneration(Filename) : 0;
        Deferred.DeferredTime = FPlatformTime::Seconds();
        if (PrebuiltStreams)
        {
            Deferred.PrebuiltStream = MakeUnique<RealtimeMesh::FRealtimeMeshStreamSet>(*PrebuiltStreams, true);
        }
        GradientDeferredMeshes.Add(MoveTemp(Deferred));

        GetWorldTimerManager().SetTimer(GradientDeferredTimerHandle, FTimerDelegate::CreateUObject(this, &AJUSYNCFileSpawnerActor::ProcessGradientDeferredMeshesTimer), 0.05f, true);

        UE_LOG(LogTemp, Verbose, TEXT("[Spawner] Deferred gradient mesh '%s' until LUT ready"), *Filename);
        return nullptr;
    }

    UMaterialInterface* SpawnMat = nullptr;
    if (!BakeLUTVertexColorCompact(Mesh, SpawnMat))
    {
        SpawnMat = SpawnMaterial;
    }

    AActor* Actor = nullptr;

    if (bCanReuse && Existing && Existing->IsValidLowLevel())
    {
        if (URealtimeMeshComponent* Comp = Existing->GetComponentByClass<URealtimeMeshComponent>())
        {
            if (Subsystem->UpdateRealtimeMeshFromJUSYNC_WithCompact(*Mesh, Comp, SpawnMat, PrebuiltStreams))
            {
                Actor = Existing;

                for (auto It = FilenameToActors.CreateIterator(); It; ++It)
                {
                    if (It.Key() != Filename)
                    {
                        It.Value().Remove(Actor);
                        if (It.Value().Num() == 0)
                        {
                            It.RemoveCurrent();
                        }
                    }
                }

                TArray<AActor*>& FileActors = FilenameToActors.FindOrAdd(Filename);
                if (!FileActors.Contains(Actor))
                {
                    FileActors.Add(Actor);
                }

                FileToActorMap.Add(Key, Actor);
                if (!LogicalKey.IsEmpty())
                {
                    FileToActorMap.Add(LogicalKey, Actor);
                }

                const FBox MeshBox = Mesh->GetLocalBounds();
                AccumulateFitBounds(MeshBox);
                const FVector FitScale = ComputeFitScale(FitUnionBounds);
                if (!FitScale.IsZero())
                {
                    Actor->SetActorScale3D(FitScale);
                    if (bCenterInVolume)
                    {
                        Actor->SetActorLocation(ComputeFitLocation(FitUnionBounds, FitScale));
                    }
                    LastMeshLocalBounds.Add(Actor, MeshBox);
                }
                else if (SpawnScale != FVector::ZeroVector)
                {
                    Actor->SetActorScale3D(bUseUniformScaling ? FVector(SpawnScale.X) : FVector(SpawnScale));
                }

                if (bMeshTextureReady)
                {
                    ApplySenderTextureToActor(Actor);
                }
                RegisterMeshForLUTRecolor(Actor, Mesh, Filename, Actor->GetActorLocation());
            }
        }
    }

    if (!Actor)
    {
        FVector SpawnLoc = GetNextSpawnLocation();

        FVector FitScale = FVector::ZeroVector;
        if (bFitToVolume && FitVolume)
        {
            AccumulateFitBounds(Mesh->GetLocalBounds());
            FitScale = ComputeFitScale(FitUnionBounds);
            if (!FitScale.IsZero() && bCenterInVolume)
            {
                SpawnLoc = ComputeFitLocation(FitUnionBounds, FitScale);
            }
        }

        Actor = UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation_WithCompact(*Mesh, SpawnLoc, FRotator::ZeroRotator, SpawnMat, PrebuiltStreams);
        if (!Actor)
        {
            return nullptr;
        }

        SpawnedActors.Add(Actor);
        ActorsSpawned++;
        FileToActorMap.Add(Key, Actor);
        if (!LogicalKey.IsEmpty())
        {
            FileToActorMap.Add(LogicalKey, Actor);
        }

        FJUSYNCBenchmarkTiming::Get().MarkFirstActorSpawned();
        FilenameToActors.FindOrAdd(Filename).Add(Actor);
        Actor->SetActorEnableCollision(false);

        if (!FitScale.IsZero())
        {
            Actor->SetActorScale3D(FitScale);
            LastMeshLocalBounds.Add(Actor, Mesh->GetLocalBounds());
        }
        else if (SpawnScale != FVector::ZeroVector)
        {
            Actor->SetActorScale3D(bUseUniformScaling ? FVector(SpawnScale.X) : FVector(SpawnScale));
        }

        OnFileComplete.Broadcast(Filename, Actor);
        NextSpawnIndex++;

        if (bMeshTextureReady)
        {
            ApplySenderTextureToActor(Actor);
        }

        RegisterMeshForLUTRecolor(Actor, Mesh, Filename, SpawnLoc);

        if (bEnableTimeStepAnimation && AnimationController)
        {
            const FString AnimKey = JUSYNCElementNames::MakeRankedElementKey(Mesh->ElementName, Rank);
            AnimationController->AddActor(Filename, Actor, AnimKey);
        }
    }

    if (Actor && !ReplacementActors.Contains(Actor))
    {
        ReplacementActors.Add(Actor);
    }

    if (Actor)
    {
        JUSYNC_DIAG_MARK(Filename, JUSYNC_DIAG_SPAWNED);
    }

    return Actor;
}

void AJUSYNCFileSpawnerActor::DestroyUnreplacedMeshActors(const FString& Filename, const TArray<AActor*>& OldMeshActors, const TArray<AActor*>& ReplacementActors)
{
    TSet<AActor*> ReplacementSet;
    ReplacementSet.Reserve(ReplacementActors.Num());
    for (AActor* Actor : ReplacementActors)
    {
        if (Actor && Actor->IsValidLowLevel())
        {
            ReplacementSet.Add(Actor);
        }
    }

    TSet<AActor*> DestroySet;
    for (AActor* Actor : OldMeshActors)
    {
        if (Actor && Actor->IsValidLowLevel() && !ReplacementSet.Contains(Actor))
        {
            DestroySet.Add(Actor);
        }
    }

    for (AActor* Actor : DestroySet)
    {
        SpawnedActors.Remove(Actor);
        if (ActorsSpawned > 0)
        {
            ActorsSpawned--;
        }
        if (AnimationController) AnimationController->RemoveActor(Actor);
        Actor->Destroy();
    }

    for (TPair<FString, TArray<AActor*>>& Pair : FilenameToActors)
    {
        Pair.Value.RemoveAll([&DestroySet](AActor* A) { return DestroySet.Contains(A); });
    }

    for (auto It = FileToActorMap.CreateIterator(); It; ++It)
    {
        if (DestroySet.Contains(It.Value()))
        {
            It.RemoveCurrent();
        }
    }
}

void AJUSYNCFileSpawnerActor::DestroyUnreplacedPCActors(const FString& Filename, const TArray<AActor*>& OldPCActors, const TSet<FString>& NewPCKeys)
{
    if (OldPCActors.Num() == 0)
    {
        return;
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr;

    TSet<AActor*> DestroySet;
    TSet<FString> DestroyedElements;

    for (AActor* Actor : OldPCActors)
    {
        if (!Actor || !Actor->IsValidLowLevel())
        {
            continue;
        }
        const FString Key = PCSpawner ? PCSpawner->GetElementKeyForActor(Actor) : FString();
        if (NewPCKeys.Contains(Key))
        {
            continue;
        }
        if (!Key.IsEmpty())
        {
            const FString KeyFile = PCElementToFilename.FindRef(Key);
            if (!KeyFile.IsEmpty() && KeyFile != Filename)
            {
                continue;
            }
        }
        DestroySet.Add(Actor);
        if (!Key.IsEmpty())
        {
            DestroyedElements.Add(JUSYNCCanonicalFromRankedElementKey(Key));
        }
    }

    for (AActor* Actor : DestroySet)
    {
        SpawnedActors.Remove(Actor);
        if (ActorsSpawned > 0)
        {
            ActorsSpawned--;
        }
        if (AnimationController) AnimationController->RemoveActor(Actor);
        if (PCSpawner)
        {
            PCSpawner->DestroyTrackedActor(Actor);
        }
        else
        {
            Actor->Destroy();
        }
    }

    if (TArray<AActor*>* Arr = FilenameToPCActors.Find(Filename))
    {
        Arr->RemoveAll([&DestroySet](AActor* A) { return DestroySet.Contains(A); });
    }

    for (auto It = FileToActorMap.CreateIterator(); It; ++It)
    {
        if (DestroySet.Contains(It.Value()))
        {
            It.RemoveCurrent();
        }
    }

    if (PathlineManager && DestroyedElements.Num() > 0)
    {
        for (const FString& Element : DestroyedElements)
        {
            bool bRankRemains = false;
            for (const TPair<FString, TArray<AActor*>>& Pair : FilenameToPCActors)
            {
                for (AActor* Remaining : Pair.Value)
                {
                    if (!Remaining || DestroySet.Contains(Remaining) || !Remaining->IsValidLowLevel())
                    {
                        continue;
                    }
                    const FString RemainingKey = PCSpawner
                        ? PCSpawner->GetElementKeyForActor(Remaining) : FString();
                    if (RemainingKey == Element ||
                        RemainingKey.StartsWith(Element + TEXT("_r"), ESearchCase::CaseSensitive))
                    {
                        bRankRemains = true;
                        break;
                    }
                }
                if (bRankRemains)
                {
                    break;
                }
            }

            if (!bRankRemains)
            {
                PathlineManager->RemoveElement(Element);
            }
        }
    }
}

void AJUSYNCFileSpawnerActor::DeduplicateLogicalActors()
{
    TMap<FString, AActor*> KeptByLogical;
    TSet<AActor*> DestroySet;

    for (const TPair<FString, TArray<AActor*>>& Pair : FilenameToActors)
    {
        const FString Logical = JUSYNCBuildLogicalActorKey(Pair.Key);
        if (Logical.IsEmpty())
        {
            continue;
        }

        AActor* Preferred = FileToActorMap.FindRef(Logical);
        AActor*& Kept = KeptByLogical.FindOrAdd(Logical);

        for (AActor* Actor : Pair.Value)
        {
            if (!Actor || !Actor->IsValidLowLevel())
            {
                continue;
            }

            if (!Kept)
            {
                Kept = Actor;
            }
            else if (Kept != Actor)
            {
                if (Preferred == Actor)
                {
                    DestroySet.Add(Kept);
                    Kept = Actor;
                }
                else
                {
                    DestroySet.Add(Actor);
                }
            }
        }

        if (Preferred && Preferred->IsValidLowLevel() && !Kept)
        {
            Kept = Preferred;
        }
    }

    for (AActor* Actor : DestroySet)
    {
        if (!Actor || !Actor->IsValidLowLevel())
        {
            continue;
        }

        UE_LOG(LogTemp, Display, TEXT("[Spawner] Removing stale duplicate logical actor '%s'"), *Actor->GetName());

        SpawnedActors.Remove(Actor);
        if (ActorsSpawned > 0)
        {
            ActorsSpawned--;
        }
        if (AnimationController)
        {
            AnimationController->RemoveActor(Actor);
        }

        for (auto It = FilenameToActors.CreateIterator(); It; ++It)
        {
            It.Value().Remove(Actor);
        }
        for (auto It = FileToActorMap.CreateIterator(); It; ++It)
        {
            if (It.Value() == Actor)
            {
                It.RemoveCurrent();
            }
        }

        Actor->Destroy();
    }
}

void AJUSYNCFileSpawnerActor::DownloadGradientPng(UJUSYNCSubsystem* Subsystem)
{
    if (!Subsystem) return;

    FString PngPath = GradientPngFilename;
    int32 PngRank = 0;
    int64 PngSize = 0;

    if (PngPath.IsEmpty())
    {
        if (GradientPngRankMap.Num() > 0)
        {
            for (auto It = GradientPngRankMap.CreateConstIterator(); It; ++It)
            {
                PngPath = It.Key();
                PngRank = It.Value().Rank;
                PngSize = It.Value().Size;
                break;
            }
        }
    }
    else
    {
        if (const FJUSYNCFileCandidateMeta* Meta = GradientPngRankMap.Find(PngPath))
        {
            PngRank = Meta->Rank;
            PngSize = Meta->Size;
        }
        else if (FilteredRanks.Num() > 0)
        {
            PngRank = FilteredRanks[0];
        }
    }

    if (PngPath.IsEmpty())
    {
        UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: no gradient PNG found in broker"));
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: downloading gradient PNG '%s' from rank %d (size %lld)"), *PngPath, PngRank, static_cast<long long>(PngSize));

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSub = Subsystem;
    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    FString PngCopy = PngPath;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSub, WeakThis, PngCopy, PngRank, PngSize]()
    {
        if (!WeakSub.IsValid() || !WeakThis.IsValid()) return;

        TArray<uint8> PngData;
        bool bOk = false;

        for (int32 Retry = 0; Retry < 3; ++Retry)
        {
            if (Retry > 0)
            {
                UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: retrying gradient PNG download (attempt %d/3)"), Retry + 1);
                FPlatformProcess::Sleep(1.0f * Retry);
            }
            bOk = WeakSub->RequestFileSized(PngCopy, PngRank, 15000, PngData, static_cast<uint64>(FMath::Max<int64>(0, PngSize)));
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
                if (Img->GetRaw(ERGBFormat::BGRA, 8, RawData))
                {
                    const int64 W = Img->GetWidth();
                    const int64 H = Img->GetHeight();
                    if (W > 0 && H > 0 && RawData.Num() >= W * 4)
                    {
                        LUT.Reserve(FMath::Min(W, 256));
                        for (int64 x = 0; x < W && x < 256; ++x)
                        {
                            const uint8* Pixel = RawData.GetData() + x * 4;  // BGRA
                            LUT.Add(FColor(Pixel[2], Pixel[1], Pixel[0], 255));
                        }
                        if (H > 2)
                        {
                            UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: '%s' is %lldx%lld texture, sampled top row for LUT (%d colors)"),
                                *PngCopy, (long long)W, (long long)H, LUT.Num());
                        }
                    }
                }
            }
        }

        if (LUT.Num() == 0)
        {
            UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: PNG decode failed, trying middleware gradient cache"));
            UJUSYNCSubsystem* S = WeakSub.Get();
            if (S)
            {
                S->ApplyCachedGradientToSpawner();
                FJUSYNCPointCloudSpawner* Sp = S->GetPointCloudSpawner();
                if (Sp && Sp->GetGradientLUT().Num() > 0)
                {
                    LUT = Sp->GetGradientLUT();
                    UE_LOG(LogTemp, Log, TEXT("JUSYNC Spawner: fallback gradient cache: %d colors"), LUT.Num());
                }
            }
        }

        if (LUT.Num() == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: failed to obtain gradient after PNG + cache fallback"));
        }

        if (LUT.Num() > 0)
        {
            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [WeakThis, LUT = MoveTemp(LUT)]()
                {
                    if (!WeakThis.IsValid()) return;

                    UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
                    if (!S) return;

                    FJUSYNCPointCloudSpawner* Sp = S->GetPointCloudSpawner();
                    if (Sp)
                    {
                        Sp->SetGradientLUT(LUT);
                        WeakThis->bGradientReady = true;

                        UE_LOG(LogTemp, Log, TEXT("[Spawner] Gradient LUT loaded: %d colors"), LUT.Num());
                        WeakThis->RebuildGradientMaterial();
                        Sp->RecolorGradientPendingActors();
                        WeakThis->RecolorGradientPendingMeshes();
                        WeakThis->ProcessGradientDeferredMeshes(false);
                    }
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: could not decode gradient PNG '%s'"), *PngCopy);
        }
    });
}

void AJUSYNCFileSpawnerActor::MaybeTriggerGradientLoad(const TArray<FString>& Files, const TArray<int64>& Sizes, const TArray<int32>& Ranks, UJUSYNCSubsystem* Subsystem)
{
    if (!Subsystem) return;
    if (bGradientReady.load() || bMeshTextureReady) return;
    if (bGradientDownloadStarted.load()) return;

    int32 Added = 0;
    for (int32 i = 0; i < Files.Num(); ++i)
    {
        const FString& F = Files[i];
        if (!(F.EndsWith(TEXT(".png")) || F.EndsWith(TEXT(".PNG")))) continue;
        if (GradientPngRankMap.Contains(F)) continue;
        FJUSYNCFileCandidateMeta Meta;
        Meta.Rank = Ranks.IsValidIndex(i) ? Ranks[i] : 0;
        Meta.Size = Sizes.IsValidIndex(i) ? Sizes[i] : 0;
        GradientPngRankMap.Add(F, Meta);
        ++Added;
    }

    if (GradientPngRankMap.Num() > 0)
    {
        bGradientDownloadStarted.store(true);
        UE_LOG(LogTemp, Display, TEXT("[Spawner] Gradient PNG present (%d total, %d new) - loading LUT"), GradientPngRankMap.Num(), Added);
        DownloadGradientPng(Subsystem);
    }
}

struct FJUSYNCDecodedPngPayload
{
    FString Filename;
    int32 Width = 0;
    int32 Height = 0;
    bool bHasAlpha = false;
    TArray64<uint8> Pixels;
};

static UTexture2D* JUSYNCCreateTextureFromDecodedPng(const FJUSYNCDecodedPngPayload& Payload)
{
    if (Payload.Width <= 0 || Payload.Height <= 0 || Payload.Pixels.Num() == 0)
    {
        return nullptr;
    }

    const int64 NumPixels = static_cast<int64>(Payload.Width) * static_cast<int64>(Payload.Height);
    const bool bSourceRGBA = Payload.bHasAlpha && Payload.Pixels.Num() >= NumPixels * 4;
    const bool bSourceRGB = !Payload.bHasAlpha && Payload.Pixels.Num() >= NumPixels * 3;
    if (!bSourceRGBA && !bSourceRGB)
    {
        return nullptr;
    }

    UTexture2D* NewTexture = UTexture2D::CreateTransient(Payload.Width, Payload.Height, PF_B8G8R8A8);
    if (!NewTexture || !NewTexture->GetPlatformData() || NewTexture->GetPlatformData()->Mips.Num() == 0)
    {
        return NewTexture;
    }

    FTexture2DMipMap& Mip = NewTexture->GetPlatformData()->Mips[0];
    void* TexturePtr = Mip.BulkData.Lock(LOCK_READ_WRITE);
    if (TexturePtr)
    {
        if (bSourceRGBA)
        {
            FMemory::Memcpy(TexturePtr, Payload.Pixels.GetData(), NumPixels * 4);
        }
        else
        {
            const uint8* Src = Payload.Pixels.GetData();
            uint8* Dst = static_cast<uint8*>(TexturePtr);
            for (int64 Pixel = 0; Pixel < NumPixels; ++Pixel)
            {
                Dst[0] = Src[0];
                Dst[1] = Src[1];
                Dst[2] = Src[2];
                Dst[3] = 255;
                Src += 3;
                Dst += 4;
            }
        }
        Mip.BulkData.Unlock();
        NewTexture->UpdateResource();
    }

    return NewTexture;
}

void AJUSYNCFileSpawnerActor::LoadMeshTextureAsync(UJUSYNCSubsystem* Subsystem)
{
    if (!Subsystem) return;
    if (bMeshTextureLoading.load() || bMeshTextureReady) return;
    if (GradientPngRankMap.Num() == 0) return;

    bMeshTextureLoading.store(true);

    struct FJUSYNCTextureCandidate
    {
        FString Filename;
        int32 Rank = 0;
        int64 Size = 0;
    };

    TArray<FJUSYNCTextureCandidate> Candidates;
    Candidates.Reserve(GradientPngRankMap.Num());
    for (const TPair<FString, FJUSYNCFileCandidateMeta>& Png : GradientPngRankMap)
    {
        FJUSYNCTextureCandidate Candidate;
        Candidate.Filename = Png.Key;
        Candidate.Rank = Png.Value.Rank;
        Candidate.Size = Png.Value.Size;
        Candidates.Add(MoveTemp(Candidate));
    }

    TWeakObjectPtr<UJUSYNCSubsystem> WeakSub = Subsystem;
    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSub, WeakThis, Candidates]()
    {
        if (!WeakSub.IsValid() || !WeakThis.IsValid()) return;
        UJUSYNCSubsystem* S = WeakSub.Get();

        FJUSYNCDecodedPngPayload Decoded;

        IImageWrapperModule& ImgMod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        TArray<uint8> PngData;
        for (const FJUSYNCTextureCandidate& Candidate : Candidates)
        {
            PngData.Reset();
            if (!S->RequestFileSized(Candidate.Filename, Candidate.Rank, 60000, PngData, static_cast<uint64>(FMath::Max<int64>(0, Candidate.Size))) || PngData.Num() == 0)
            {
                continue;
            }

            TSharedPtr<IImageWrapper> Img = ImgMod.CreateImageWrapper(EImageFormat::PNG);
            if (!Img.IsValid() || !Img->SetCompressed(PngData.GetData(), PngData.Num()))
            {
                continue;
            }

            TArray64<uint8> Raw;
            if (!Img->GetRaw(Raw))
            {
                continue;
            }

            const int64 W = Img->GetWidth();
            const int64 H = Img->GetHeight();
            if (W <= 0 || H <= 2 || Raw.Num() < W * H * 3)
            {
                continue;
            }

            Decoded.Filename = Candidate.Filename;
            Decoded.Width = static_cast<int32>(W);
            Decoded.Height = static_cast<int32>(H);
            Decoded.bHasAlpha = (Raw.Num() == W * H * 4);
            Decoded.Pixels = MoveTemp(Raw);
            break;
        }

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakSub, WeakThis, Decoded = MoveTemp(Decoded)]()
            {
                if (!WeakSub.IsValid() || !WeakThis.IsValid()) return;
                UJUSYNCSubsystem* S = WeakSub.Get();
                AJUSYNCFileSpawnerActor* T = WeakThis.Get();
                if (!S || !T) return;

                if (Decoded.Filename.IsEmpty() || Decoded.Pixels.Num() == 0)
                {
                    T->bMeshTextureLoading.store(false);
                    UE_LOG(LogTemp, Log, TEXT("[Spawner] No full-texture PNG found (only gradients / none) â€” meshes keep spawn material"));
                    return;
                }

                UTexture2D* Tex = JUSYNCCreateTextureFromDecodedPng(Decoded);
                if (!Tex)
                {
                    T->bMeshTextureLoading.store(false);
                    return;
                }

                T->MeshTextureCache.Add(Decoded.Filename, Tex);
                T->ActiveMeshTexture = Tex;
                T->bMeshTextureReady = true;
                T->bMeshTextureLoading.store(false);

                UE_LOG(LogTemp, Log, TEXT("[Spawner] Mesh texture ready: %s (%dx%d)"), *Decoded.Filename, Decoded.Width, Decoded.Height);

                for (AActor* A : T->SpawnedActors)
                {
                    T->ApplySenderTextureToActor(A);
                }
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
}

void AJUSYNCFileSpawnerActor::ApplySenderTextureToActor(AActor* Actor)
{
    if (!Actor) return;
    URealtimeMeshComponent* Comp = Actor->GetComponentByClass<URealtimeMeshComponent>();
    if (!Comp) return;
    UTexture2D* Tex = ActiveMeshTexture.Get();
    if (!Tex) return;
    ApplySenderTextureToComponent(Comp, Tex);
}

UMaterialInstanceDynamic* AJUSYNCFileSpawnerActor::GetOrCreateSenderMID(URealtimeMeshComponent* Comp, UTexture2D* SenderTex)
{
    if (!Comp || !SenderTex)
    {
        return nullptr;
    }

    UMaterial* BaseMat = SpawnMaterial ? SpawnMaterial->GetMaterial() : nullptr;
    if (!BaseMat)
    {
        if (UMaterialInterface* Cur = Comp->GetMaterial(0))
        {
            BaseMat = Cur->GetMaterial();
        }
    }
    if (!BaseMat)
    {
        BaseMat = LoadObject<UMaterial>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    }
    if (!BaseMat)
    {
        return nullptr;
    }

    const FName ParamName = !TextureSampleParameterName.IsEmpty() ? *TextureSampleParameterName : TEXT("BaseColor");

    if (TWeakObjectPtr<UMaterialInstanceDynamic>* Cached = SenderMIDCache.Find(Comp))
    {
        if (UMaterialInstanceDynamic* MID = Cached->Get())
        {
            if (MID->GetBaseMaterial() == BaseMat)
            {
                MID->SetTextureParameterValue(ParamName, SenderTex);
                return MID;
            }
        }
    }

    UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMat, Comp);
    if (MID)
    {
        MID->SetTextureParameterValue(ParamName, SenderTex);
        SenderMIDCache.Add(Comp, MID);
    }
    return MID;
}

void AJUSYNCFileSpawnerActor::ApplySenderTextureToComponent(URealtimeMeshComponent* Comp, UTexture2D* SenderTex)
{
    if (!Comp || !SenderTex) return;

    UMaterialInstanceDynamic* MID = GetOrCreateSenderMID(Comp, SenderTex);
    if (!MID) return;

    Comp->SetMaterial(0, MID);
    Comp->MarkRenderStateDirty();
}

void AJUSYNCFileSpawnerActor::RebuildGradientMaterial()
{
    GradientMaterial = nullptr;
    GradientTextureParameterName = NAME_None;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem || !Subsystem->GetPointCloudSpawner())
    {
        return;
    }

    const TArray<FColor> LUT = Subsystem->GetPointCloudSpawner()->GetGradientLUT();
    if (LUT.Num() <= 1)
    {
        return;
    }

    if (SpawnMaterial && JUSYNCMaterialUsesVertexColor(SpawnMaterial))
    {
        UE_LOG(LogTemp, Display, TEXT("[Spawner] SpawnMaterial uses vertex colors; skipping texture gradient material"));
        return;
    }

    TArray<UMaterialInterface*> Candidates;
    if (SpawnMaterial)
    {
        Candidates.Add(SpawnMaterial);
    }
    Candidates.Add(Subsystem->GetCachedMaterial(TEXT("/Game/Base_Material")));
    Candidates.Add(Subsystem->GetCachedMaterial(TEXT("/Game/Materials/M_BaseMaterial")));
    Candidates.Add(Subsystem->GetCachedMaterial(TEXT("/Engine/EngineMaterials/EmissiveTexturedMaterial")));
    Candidates.Add(Subsystem->GetCachedMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial")));

    UMaterialInterface* ChosenBase = nullptr;
    FName ChosenParameter = NAME_None;
    for (UMaterialInterface* Candidate : Candidates)
    {
        if (!Candidate)
        {
            continue;
        }

        FName ParameterName;
        if (JUSYNCFindColorTextureParameter(Candidate, ParameterName))
        {
            ChosenBase = Candidate;
            ChosenParameter = ParameterName;
            break;
        }
    }

    if (!ChosenBase)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Spawner] No valid color texture parameter found for gradient material; falling back to vertex-color path"));
        return;
    }

    const int32 Width = FMath::Clamp(LUT.Num(), 1, 1024);
    UTexture2D* LUTTexture = UTexture2D::CreateTransient(Width, 1, PF_B8G8R8A8);
    if (!LUTTexture || !LUTTexture->GetPlatformData() || LUTTexture->GetPlatformData()->Mips.Num() == 0)
    {
        return;
    }

    FTexture2DMipMap& Mip = LUTTexture->GetPlatformData()->Mips[0];
    void* TexturePtr = Mip.BulkData.Lock(LOCK_READ_WRITE);
    if (!TexturePtr)
    {
        return;
    }

    uint8* Dst = static_cast<uint8*>(TexturePtr);
    for (int32 X = 0; X < Width; ++X)
    {
        const FColor& C = LUT[X < LUT.Num() ? X : (LUT.Num() - 1)];
        Dst[0] = C.B;
        Dst[1] = C.G;
        Dst[2] = C.R;
        Dst[3] = C.A;
        Dst += 4;
    }
    Mip.BulkData.Unlock();

    LUTTexture->AddressX = TA_Clamp;
    LUTTexture->AddressY = TA_Clamp;
    LUTTexture->SRGB = true;
    LUTTexture->UpdateResource();

    GradientLUTTexture = LUTTexture;
    UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(ChosenBase, this);
    if (MID)
    {
        MID->SetTextureParameterValue(ChosenParameter, LUTTexture);
        GradientMaterial = MID;
        GradientTextureParameterName = ChosenParameter;
        UE_LOG(LogTemp, Display, TEXT("[Spawner] Gradient material ready: %dx1 texture on %s param=%s"),
            Width, *ChosenBase->GetName(), *ChosenParameter.ToString());
    }
}

bool AJUSYNCFileSpawnerActor::BakeLUTVertexColorCompact(FJUSYNCCompactMeshRef& Mesh, UMaterialInterface*& OutVertexMaterial)
{
    OutVertexMaterial = nullptr;

    if (!Mesh || !Mesh->IsValid())
    {
        return false;
    }
    if (bMeshTextureReady)
    {
        return false;
    }
    if (!bGradientReady.load())
    {
        return false;
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem || !Subsystem->GetPointCloudSpawner())
    {
        return false;
    }

    const int32 VertexCount = Mesh->GetVertexCount();
    const bool bHasUVs = VertexCount > 0 && Mesh->UVs.Num() == VertexCount;
    const bool bHasVertexColors = Mesh->HasVertexColors();

    if (SpawnMaterial && JUSYNCMaterialUsesVertexColor(SpawnMaterial))
    {
        if (!bHasVertexColors && !bHasUVs)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Spawner] SpawnMaterial %s uses vertex colors, but mesh has no vertex colors or UVs"), *SpawnMaterial->GetName());
            OutVertexMaterial = SpawnMaterial;
            return true;
        }

        if (!bHasVertexColors)
        {
            const TArray<FColor> LUT = Subsystem->GetPointCloudSpawner()->GetGradientLUT();
            if (LUT.Num() > 1)
            {
                const uint64 LUTVersion = Subsystem->GetPointCloudSpawner()->GetLUTVersion();
                FJUSYNCCompactMeshRef MutableMesh = MakeShared<FJUSYNCCompactMeshData, ESPMode::ThreadSafe>(*Mesh);
                FJUSYNCUSDLoader::BakeLUTIntoCompactMesh(*MutableMesh, LUT);
                MutableMesh->LUTVersion = LUTVersion;
                Mesh = MoveTemp(MutableMesh);
            }
        }

        OutVertexMaterial = SpawnMaterial;
        UE_LOG(LogTemp, Display, TEXT("[Spawner] Mesh color: using actor SpawnMaterial %s (vertex colors)"), *SpawnMaterial->GetName());
        return true;
    }

    FName SpawnTextureParameter;
    if (SpawnMaterial && JUSYNCFindColorTextureParameter(SpawnMaterial, SpawnTextureParameter))
    {
        if (!bHasUVs)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Spawner] SpawnMaterial %s uses a texture parameter, but mesh has no UVs; falling back to vertex colors"), *SpawnMaterial->GetName());
        }
        else if (!GradientMaterial.IsValid() || GradientMaterial.Get()->GetMaterial() != SpawnMaterial->GetMaterial())
        {
            RebuildGradientMaterial();
        }

        if (GradientMaterial.IsValid())
        {
            OutVertexMaterial = GradientMaterial.Get();
            UE_LOG(LogTemp, Display, TEXT("[Spawner] Mesh color: using actor SpawnMaterial %s (texture param=%s)"),
                *SpawnMaterial->GetName(), *SpawnTextureParameter.ToString());
            return true;
        }
    }

    UMaterialInterface* VertexMaterial = Subsystem->GetCachedMaterial(TEXT("/Game/Materials/M_JusyncMesh"));
    if (!VertexMaterial)
    {
        VertexMaterial = Subsystem->GetCachedMaterial(TEXT("/Game/Materials/M_VertexColor"));
    }
    if (!VertexMaterial)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Spawner] M_VertexColor material not found; mesh LUT color path unavailable"));
        return false;
    }

    if (!bHasVertexColors)
    {
        if (!bHasUVs)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Spawner] Mesh has no vertex colors or UVs; cannot bake LUT vertex colors"));
            return false;
        }

        const TArray<FColor> LUT = Subsystem->GetPointCloudSpawner()->GetGradientLUT();
        if (LUT.Num() <= 1)
        {
            return false;
        }

        const uint64 LUTVersion = Subsystem->GetPointCloudSpawner()->GetLUTVersion();
        FJUSYNCCompactMeshRef MutableMesh = MakeShared<FJUSYNCCompactMeshData, ESPMode::ThreadSafe>(*Mesh);
        FJUSYNCUSDLoader::BakeLUTIntoCompactMesh(*MutableMesh, LUT);
        MutableMesh->LUTVersion = LUTVersion;
        Mesh = MoveTemp(MutableMesh);
    }

    OutVertexMaterial = VertexMaterial;
    UE_LOG(LogTemp, Display, TEXT("[Spawner] Mesh color: using %s (%d verts)"), *VertexMaterial->GetName(), Mesh->GetVertexCount());
    return true;
}

void AJUSYNCFileSpawnerActor::RegisterMeshForLUTRecolor(AActor* Spawned, FJUSYNCCompactMeshRef& Mesh, const FString& Filename, const FVector& Loc)
{
    if (!Spawned || !Spawned->IsValidLowLevel())
    {
        return;
    }
    if (bMeshTextureReady || bMeshTextureLoading.load() || bGradientReady.load())
    {
        return;
    }
    if (!Mesh || Mesh->GetVertexCount() == 0 || Mesh->UVs.Num() != Mesh->GetVertexCount())
    {
        return;
    }

    FRecolorMeshEntry Entry;
    Entry.Mesh = Mesh;
    Entry.Filename = Filename;
    Entry.Loc = Loc;
    Entry.Scale = Spawned->GetActorScale3D();
    GradientPendingMeshes.Add(Spawned, MoveTemp(Entry));
}

void AJUSYNCFileSpawnerActor::RecolorGradientPendingMeshes()
{
    if (GradientPendingMeshes.Num() == 0) return;
    if (!bGradientReady.load()) return;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem || !Subsystem->GetPointCloudSpawner()) return;
    const TArray<FColor> LUT = Subsystem->GetPointCloudSpawner()->GetGradientLUT();
    const uint64 LUTVersion = Subsystem->GetPointCloudSpawner()->GetLUTVersion();
    if (LUT.Num() <= 1) return;

    int32 Recolored = 0;
    TArray<AActor*> Done;
    for (auto It = GradientPendingMeshes.CreateIterator(); It; ++It)
    {
        AActor* OldActor = It.Key();
        FRecolorMeshEntry& Entry = It.Value();

        if (!OldActor || !OldActor->IsValidLowLevel() || !Entry.Mesh || !Entry.Mesh->IsValid())
        {
            Done.Add(OldActor);
            continue;
        }

        UMaterialInterface* SpawnMat = nullptr;
        FJUSYNCUSDLoader::EnsureCompactMeshBaked(Entry.Mesh, LUT, LUTVersion);
        if (!BakeLUTVertexColorCompact(Entry.Mesh, SpawnMat) || !SpawnMat)
        {
            Done.Add(OldActor);
            continue;
        }

        URealtimeMeshComponent* Comp = OldActor->GetComponentByClass<URealtimeMeshComponent>();
        if (Comp && Subsystem->UpdateRealtimeMeshFromJUSYNC_WithCompact(*Entry.Mesh, Comp, SpawnMat))
        {
            if (bMeshTextureReady)
            {
                ApplySenderTextureToActor(OldActor);
            }
            Recolored++;
            Done.Add(OldActor);
            continue;
        }

        AActor* NewActor = UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation_WithCompact(*Entry.Mesh, Entry.Loc, Entry.Rot, SpawnMat, nullptr);
        if (!NewActor)
        {
            Done.Add(OldActor);
            continue;
        }
        NewActor->SetActorEnableCollision(false);
        NewActor->SetActorScale3D(Entry.Scale);

        SpawnedActors.Remove(OldActor);
        SpawnedActors.Add(NewActor);
        const FString Key = Entry.Mesh->ElementName + TEXT("|") + Entry.Filename;
        if (FileToActorMap.Contains(Key))
        {
            FileToActorMap.Add(Key, NewActor);
        }
        if (TArray<AActor*>* Arr = FilenameToActors.Find(Entry.Filename))
        {
            Arr->Remove(OldActor);
            Arr->Add(NewActor);
        }

        OldActor->Destroy();
        Recolored++;
        Done.Add(OldActor);
    }

    for (AActor* A : Done)
    {
        GradientPendingMeshes.Remove(A);
    }

    if (Recolored > 0)
    {
        UE_LOG(LogTemp, Display, TEXT("[Spawner] Recolored %d mesh actor(s) with LUT gradient"), Recolored);
    }
}

void AJUSYNCFileSpawnerActor::CheckAllDownloadsComplete()
{
    if (bInCheckAllDownloads)
    {
        return;
    }
    TGuardValue<bool> CheckGuard(bInCheckAllDownloads, true);

    UE_LOG(LogTemp, Verbose, TEXT("[COMPLETION] ENTRY: FilesDownloaded=%d, FilesTotal=%d, PipelineActive=%d, PendingParseTasks=%d, PendingAsyncPCS=%d, Deferred=%d, bInitialSpawnDone=%d"),
        FilesDownloaded, FilesTotal, PipelineActive, PendingParseTasks, PendingAsyncPCS, DeferredSpawns.Num(), bInitialSpawnDone ? 1 : 0);

    {
        const double Now = FPlatformTime::Seconds();
        const bool bOnlyCloudSpawnsPending =
            PipelineActive <= 0 && PendingParseTasks <= 0 && PendingAsyncSpawns <= 0 &&
            DeferredSpawns.Num() == 0 && PendingAsyncPCS > 0;
        if (!bOnlyCloudSpawnsPending)
        {
            InitialSpawnPendingSince = 0.0;
        }
        else if (InitialSpawnPendingSince <= 0.0)
        {
            InitialSpawnPendingSince = Now;
        }
        else if (Now - InitialSpawnPendingSince > 1.5)
        {
            UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: %d point-cloud spawn(s) did not complete within %.1fs; completing initial spawn so live updates can start"),
                PendingAsyncPCS, Now - InitialSpawnPendingSince);
            PendingAsyncPCS = 0;
            InitialSpawnPendingSince = 0.0;
        }
    }

    const bool bAllDispatched = (PipelineNextIndex >= FilteredFiles.Num());
    const bool bPipelineIdle =
        PipelineActive <= 0 && PendingParseTasks <= 0 && PendingAsyncSpawns <= 0 &&
        PendingAsyncPCS <= 0 && DeferredSpawns.Num() == 0;
    if (FilesDownloaded >= FilesTotal || (bAllDispatched && bPipelineIdle))
    {
        int32 TotalFailed = FailedFileIndices.Num() + ParseFailedIndices.Num();
        if (TotalFailed > 0 && CurrentRetryCount < MaxRetries)
        {
            UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: %d files failed (dl=%d, parse=%d), retrying (%d/%d)..."),
                TotalFailed, FailedFileIndices.Num(), ParseFailedIndices.Num(), CurrentRetryCount + 1, MaxRetries);
            CurrentRetryCount++;
            FilesDownloaded = FilesTotal - TotalFailed;
            PipelineActive = 0;
            PipelineNextIndex = FilesTotal;
            RetryFailedDownloads();
            return;
        }

        if (UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
        {
            if (FJUSYNCPointCloudSpawner* PCSpawner = Subsystem->GetPointCloudSpawner())
            {
                PCSpawner->DrainReadyQueue();
            }
        }

        if (PendingAsyncSpawns > 0 || PendingAsyncPCS > 0 || PendingParseTasks > 0 || DeferredSpawns.Num() > 0)
        {
            TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
            FTimerHandle WaitSpawnHandle;
            FTimerDelegate WaitSpawnDelay;
            WaitSpawnDelay.BindLambda([WeakThis]()
            {
                TWeakObjectPtr<AJUSYNCFileSpawnerActor> Wt = WeakThis;
                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [Wt]() { if (Wt.IsValid()) Wt->CheckAllDownloadsComplete(); },
                    TStatId(), nullptr, ENamedThreads::GameThread);
            });
            if (GWorld)
                GWorld->GetTimerManager().SetTimer(WaitSpawnHandle, WaitSpawnDelay, 0.2f, false);
            return;
        }

        CurrentState = EJUSYNCSpawnerState::Complete;
        bool bSuccess = ActorsSpawned > 0;
        int32 DownloadFailed = FailedFileIndices.Num();
        int32 ParseFailed = ParseFailedIndices.Num();
        int32 FailedCount = FilesTotal - ActorsSpawned - DownloadFailed - ParseFailed;
        int32 ActualNotSpawned = FMath::Max(0, FailedCount);
        UE_LOG(LogTemp, Display, TEXT("JUSYNC Spawner: pipeline complete. Spawned %d actors from %d files (%d not spawned, %d download failed, %d parse failed)"),
            ActorsSpawned, FilesTotal, ActualNotSpawned, DownloadFailed, ParseFailed);

        for (int32 idx : FailedFileIndices)
        {
            if (FilteredFiles.IsValidIndex(idx))
            {
                UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: FAILED FILE: %s (rank %d)"),
                    *FilteredFiles[idx], FilteredRanks.IsValidIndex(idx) ? FilteredRanks[idx] : -1);
            }
        }

        for (int32 idx : ParseFailedIndices)
        {
            if (FilteredFiles.IsValidIndex(idx))
            {
                UE_LOG(LogTemp, Warning, TEXT("JUSYNC Spawner: PARSE FAILED: %s (rank %d)"),
                    *FilteredFiles[idx], FilteredRanks.IsValidIndex(idx) ? FilteredRanks[idx] : -1);
            }
        }

        FailedFileIndices.Empty();
        ParseFailedIndices.Empty();

        RefitSpawnedActorsToVolume();
        bFitLocked = true;

        OnAllComplete.Broadcast(ActorsSpawned, bSuccess);

        bInitialSpawnDone = true;

        FJUSYNCBenchmarkTiming::Get().MarkFullScene();

        if (bSuccess)
        {
            ScheduleBenchmarkInteractiveWindow();
        }

        if (bEnableLiveUpdates)
        {
            StartLiveUpdatePolling();
        }
    }
}

void AJUSYNCFileSpawnerActor::RetryFailedDownloads()
{
    int32 TotalFailed = FailedFileIndices.Num() + ParseFailedIndices.Num();
    if (TotalFailed == 0)
    {
        CheckAllDownloadsComplete();
        return;
    }

    for (int32 idx : ParseFailedIndices)
    {
        if (!FailedFileIndices.Contains(idx))
            FailedFileIndices.Add(idx);
    }
    ParseFailedIndices.Empty();

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem) return;

    int32 AvailableSpots = FMath::Max(0, PipelineDepth - PipelineActive);
    int32 Spawned = 0;

    for (int32 i = 0; i < FailedFileIndices.Num() && Spawned < AvailableSpots; ++i)
    {
        int32 idx = FailedFileIndices[i];
        if (bIsCancelled || !FilteredFiles.IsValidIndex(idx)) continue;

        const FString Filename = FilteredFiles[idx];
        const int32 TargetRank = FilteredRanks.IsValidIndex(idx) ? FilteredRanks[idx] : 0;
        const int64 FileSize = FilteredSizes.IsValidIndex(idx) ? FilteredSizes[idx] : int64(2097152);
        const uint64 HashLo = FilteredHashLo.IsValidIndex(idx) ? FilteredHashLo[idx] : 0;
        const uint64 HashHi = FilteredHashHi.IsValidIndex(idx) ? FilteredHashHi[idx] : 0;

        FailedFileIndices.RemoveAt(i);
        --i;
        PipelineActive++;
        Spawned++;
        LoadFileThroughPipeline(Filename, TargetRank, FileSize, HashLo, HashHi, idx, true);
    }
}

void AJUSYNCFileSpawnerActor::OnPointCloudSpawnedHandler(const FString& EleName, AActor* Spawned)
{
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr;
    const FString FileName = PCElementToFilename.FindRef(EleName);

    auto DestroyPC = [this, PCSpawner](AActor* Actor)
    {
        if (!Actor)
        {
            return;
        }
        if (PCSpawner)
        {
            PCSpawner->DestroyTrackedActor(Actor);
        }
        else if (Actor->IsValidLowLevel())
        {
            Actor->Destroy();
        }
    };

    AActor* Existing = FileToActorMap.FindRef(EleName);

    if (AnimationController && Spawned)
    {
        AnimationController->MarkActorLive(Spawned);
    }

    if (Spawned && Existing && Existing == Spawned)
    {
        if (!FileName.IsEmpty())
        {
            for (TPair<FString, TArray<AActor*>>& Pair : FilenameToPCActors)
            {
                if (Pair.Key != FileName)
                {
                    Pair.Value.Remove(Spawned);
                }
            }
            TArray<AActor*>& Arr = FilenameToPCActors.FindOrAdd(FileName);
            if (!Arr.Contains(Spawned))
            {
                Arr.Add(Spawned);
            }
        }
        if (bEnableTimeStepAnimation && AnimationController && !FileName.IsEmpty())
        {
            AnimationController->AddActor(FileName, Spawned, EleName);
        }
        JUSYNC_DIAG_MARK(FileName, JUSYNC_DIAG_SPAWNED);
        PendingAsyncPCS = FMath::Max(0, PendingAsyncPCS - 1);
        return;
    }

    if (Spawned)
    {
        if (!bInitialSpawnDone && Existing && Existing != Spawned)
        {
            SpawnedActors.Remove(Spawned);
            if (ActorsSpawned > 0) ActorsSpawned--;
            if (!FileName.IsEmpty())
            {
                if (TArray<AActor*>* pPCList = FilenameToPCActors.Find(FileName))
                {
                    pPCList->Remove(Spawned);
                }
            }
            DestroyPC(Spawned);
            PendingAsyncPCS = FMath::Max(0, PendingAsyncPCS - 1);
            return;
        }

        SpawnedActors.Add(Spawned);
        ActorsSpawned++;
        Spawned->SetActorEnableCollision(false);

        FJUSYNCBenchmarkTiming::Get().MarkFirstActorSpawned();
#if WITH_EDITORONLY_DATA
        if (!EleName.IsEmpty())
        {
            Spawned->SetActorLabel(EleName);
        }
#endif

        if (!FileName.IsEmpty())
        {
            for (TPair<FString, TArray<AActor*>>& Pair : FilenameToPCActors)
            {
                if (Pair.Key != FileName)
                {
                    Pair.Value.Remove(Spawned);
                }
            }
            FilenameToPCActors.FindOrAdd(FileName).Add(Spawned);
        }

        if (!EleName.IsEmpty())
        {
            if (Existing && Existing != Spawned && Existing->IsValidLowLevel())
            {
                SpawnedActors.Remove(Existing);
                if (ActorsSpawned > 0) ActorsSpawned--;
                if (!FileName.IsEmpty())
                {
                    if (TArray<AActor*>* pPCList = FilenameToPCActors.Find(FileName))
                    {
                        pPCList->Remove(Existing);
                    }
                }
                DestroyPC(Existing);
            }
            FileToActorMap.Add(EleName, Spawned);
        }

        if (bEnableTimeStepAnimation && AnimationController && !FileName.IsEmpty())
        {
            AnimationController->AddActor(FileName, Spawned, EleName);
        }

        OnFileComplete.Broadcast(EleName, Spawned);
        NextSpawnIndex++;
        JUSYNC_DIAG_MARK(FileName, JUSYNC_DIAG_SPAWNED);
    }

    PendingAsyncPCS = FMath::Max(0, PendingAsyncPCS - 1);

    if (!bInitialSpawnDone)
    {
        CheckAllDownloadsComplete();
    }
}

void AJUSYNCFileSpawnerActor::ProcessDeferredSpawns()
{
    if (DeferredSpawns.Num() == 0) return;

    int32 Processed = 0;
    TArray<FDeferredSpawnEntry> Remaining;
    Remaining.Reserve(DeferredSpawns.Num());

    for (auto& Entry : DeferredSpawns)
    {
        const uint64 CurrentGeneration = (ChangeTracker) ? ChangeTracker->GetGeneration(Entry.Filename) : 0;
        if (Entry.Generation != CurrentGeneration)
        {
            UE_LOG(LogTemp, Log, TEXT("[DeferredSpawn] Discarding stale deferred spawn for '%s' (generation %llu != %llu)"),
                *Entry.Filename, (unsigned long long)Entry.Generation, (unsigned long long)CurrentGeneration);
            continue;
        }

        int32 CanSpawn = FMath::Max(0, FMath::Max(1, MaxSpawnsPerFrame) - Processed);
        if (CanSpawn <= 0)
        {
            Remaining.Add(MoveTemp(Entry));
            continue;
        }

        int32 SpawnCount = 0;
        TArray<FJUSYNCCompactMeshRef> Leftovers;
        Leftovers.Reserve(Entry.Meshes.Num());
        TArray<TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet>> LeftoverStreams;
        LeftoverStreams.Reserve(Entry.Meshes.Num());

        for (int32 i = 0; i < Entry.Meshes.Num(); ++i)
        {
            if (!Entry.Meshes[i] || !Entry.Meshes[i]->IsValid()) continue;
            if (SpawnCount >= CanSpawn)
            {
                Leftovers.Add(MoveTemp(Entry.Meshes[i]));
                if (i < Entry.PrebuiltStreams.Num())
                {
                    LeftoverStreams.Add(MoveTemp(Entry.PrebuiltStreams[i]));
                }
                continue;
            }

            RealtimeMesh::FRealtimeMeshStreamSet* Stream = nullptr;
            if (i < Entry.PrebuiltStreams.Num() && Entry.PrebuiltStreams[i])
            {
                Stream = Entry.PrebuiltStreams[i].Get();
            }
            AActor* Actor = SpawnOrUpdateMesh(Entry.Meshes[i], Entry.Filename, true, Entry.ReplacementActors, Stream, &Entry.OldActors, false, Entry.Rank);
            if (Actor)
            {
                SpawnCount++;
            }
        }

        Entry.SpawnedNewMeshes += SpawnCount;
        Processed += SpawnCount;

        if (Leftovers.Num() == 0)
        {
            if (Entry.OldActors.Num() > 0 && Entry.SpawnedNewMeshes >= Entry.ExpectedNewMeshes)
            {
                DestroyUnreplacedMeshActors(Entry.Filename, Entry.OldActors, Entry.ReplacementActors);
            }
        }
        else
        {
            Entry.Meshes = MoveTemp(Leftovers);
            Entry.PrebuiltStreams = MoveTemp(LeftoverStreams);
            Remaining.Add(MoveTemp(Entry));
        }
    }

    DeferredSpawns = MoveTemp(Remaining);
    if (DeferredSpawns.Num() > 0)
    {
        FTimerHandle DummyHandle;
        GetWorldTimerManager().SetTimer(DummyHandle, FTimerDelegate::CreateUObject(this, &AJUSYNCFileSpawnerActor::ProcessDeferredSpawns), 0.02f, false);
    }
    else if (!bInitialSpawnDone)
    {
        CheckAllDownloadsComplete();
    }
}

void AJUSYNCFileSpawnerActor::ProcessGradientDeferredMeshesTimer()
{
    ProcessGradientDeferredMeshes(false);
}

void AJUSYNCFileSpawnerActor::ProcessGradientDeferredMeshes(bool bForce)
{
    if (GradientDeferredMeshes.Num() == 0)
    {
        return;
    }

    const double Now = FPlatformTime::Seconds();
    bool bAnyTimedOut = false;
    if (!bForce && !bGradientReady.load())
    {
        for (const FJUSYNCGradientDeferredMeshEntry& Entry : GradientDeferredMeshes)
        {
            if ((Now - Entry.DeferredTime) > static_cast<double>(GradientDeferredTimeoutSeconds))
            {
                bAnyTimedOut = true;
                break;
            }
        }
        if (!bAnyTimedOut)
        {
            return;
        }
        bForce = true;
    }

    TArray<FJUSYNCGradientDeferredMeshEntry> Remaining;
    Remaining.Reserve(GradientDeferredMeshes.Num());

    for (FJUSYNCGradientDeferredMeshEntry& Entry : GradientDeferredMeshes)
    {
        const uint64 CurrentGeneration = (ChangeTracker) ? ChangeTracker->GetGeneration(Entry.Filename) : 0;
        if (Entry.Generation != CurrentGeneration)
        {
            UE_LOG(LogTemp, Log, TEXT("[GradientDeferred] Discarding stale gradient-deferred mesh for '%s'"), *Entry.Filename);
            continue;
        }

        if (!Entry.Mesh || !Entry.Mesh->IsValid())
        {
            continue;
        }

        bool bShouldForceSpawn = bForce || bGradientReady.load();
        if (!bShouldForceSpawn && (Now - Entry.DeferredTime) > static_cast<double>(GradientDeferredTimeoutSeconds))
        {
            bShouldForceSpawn = true;
        }

        TArray<AActor*> ReplacementActors;
        RealtimeMesh::FRealtimeMeshStreamSet* Stream = Entry.PrebuiltStream ? Entry.PrebuiltStream.Get() : nullptr;
        AActor* Actor = SpawnOrUpdateMesh(Entry.Mesh, Entry.Filename, true, ReplacementActors, Stream, &Entry.OldActors, bShouldForceSpawn, Entry.Rank);

        if (Actor)
        {
            Entry.SpawnedNewMeshes++;
            TArray<AActor*>& DeferredReplacements = GradientDeferredReplacementActors.FindOrAdd(Entry.Filename);
            if (!DeferredReplacements.Contains(Actor))
            {
                DeferredReplacements.Add(Actor);
            }
        }
        else
        {
            Remaining.Add(MoveTemp(Entry));
            continue;
        }

        bool bAnyRemainingSameFile = false;
        for (const FJUSYNCGradientDeferredMeshEntry& Other : Remaining)
        {
            if (Other.Filename == Entry.Filename)
            {
                bAnyRemainingSameFile = true;
                break;
            }
        }
        if (!bAnyRemainingSameFile)
        {
            bool bAnyFrameDeferredSameFile = false;
            for (const FDeferredSpawnEntry& Other : DeferredSpawns)
            {
                if (Other.Filename == Entry.Filename)
                {
                    bAnyFrameDeferredSameFile = true;
                    break;
                }
            }

            if (Entry.OldActors.Num() > 0 && !bAnyFrameDeferredSameFile)
            {
                const TArray<AActor*>* DeferredReplacements = GradientDeferredReplacementActors.Find(Entry.Filename);
                DestroyUnreplacedMeshActors(Entry.Filename, Entry.OldActors, DeferredReplacements ? *DeferredReplacements : TArray<AActor*>());
            }
            GradientDeferredReplacementActors.Remove(Entry.Filename);
        }
    }

    GradientDeferredMeshes = MoveTemp(Remaining);

    if (GradientDeferredMeshes.Num() > 0)
    {
        GetWorldTimerManager().SetTimer(GradientDeferredTimerHandle, FTimerDelegate::CreateUObject(this, &AJUSYNCFileSpawnerActor::ProcessGradientDeferredMeshesTimer), 0.05f, true);
    }
    else
    {
        GetWorldTimerManager().ClearTimer(GradientDeferredTimerHandle);
    }
}

void AJUSYNCFileSpawnerActor::OnBrokerNotification(const FJUSYNCNotification& Notification)
{
    const char* TypeStr = (Notification.Type == EJUSYNCNotificationType::FileUpdateV2) ? "FileUpdateV2" :
                           (Notification.Type == EJUSYNCNotificationType::CommitComplete) ? "CommitComplete" : "FileUpdate";
    UE_LOG(LogTemp, Display, TEXT("[V2HANDLER] Received %s for '%s' | state=%d enabled=%d initialDone=%d"),
           ANSI_TO_TCHAR(TypeStr), *Notification.Filename, (int32)CurrentState, bEnableLiveUpdates ? 1 : 0, bInitialSpawnDone ? 1 : 0);

    if (!bInitialSpawnDone &&
        (CurrentState == EJUSYNCSpawnerState::Error ||
         CurrentState == EJUSYNCSpawnerState::FetchingList ||
         CurrentState == EJUSYNCSpawnerState::Connecting))
    {
        if (!bInitialKickInFlight)
        {
            bInitialKickInFlight = true;
            GetWorldTimerManager().ClearTimer(InitialFileListRetryTimerHandle);
            InitialFileListRetryCount = 0;
            UE_LOG(LogTemp, Display, TEXT("[V2HANDLER] Broker signaled scene ready (%s for '%s') â€” fetching file list now"),
                   ANSI_TO_TCHAR(TypeStr), *Notification.Filename);
            RequestFileList();
        }
        return;
    }

    if (!bEnableLiveUpdates)
    {
        UE_LOG(LogTemp, Warning, TEXT("[SIGNAL] DROPPED â€” 'Enable Live Updates' is FALSE in spawner Details panel"));
        return;
    }

    if (CurrentState != EJUSYNCSpawnerState::Complete)
    {
        UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] BLOCKED: CurrentState=%d (need Complete=%d)"), (int32)CurrentState, (int32)EJUSYNCSpawnerState::Complete);
        return;
    }

    if (Notification.Type == EJUSYNCNotificationType::FileUpdate)
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] File update notification: '%s' (rank %d, size=%lld)"),
               *Notification.Filename, Notification.SourceRank, (long long)Notification.FileSize);
        if (MinimumFileSizeBytes <= 0 || Notification.FileSize >= MinimumFileSizeBytes)
        {
            HandleFileUpdateNotification(Notification.Filename, Notification.SourceRank, Notification.FileSize, 0, 0,
                static_cast<uint64>(Notification.Timestamp));
        }
    }
    else if (Notification.Type == EJUSYNCNotificationType::FileUpdateV2)
    {
        const bool bSenderSaysChanged = (Notification.HashLo != Notification.HashPrevLo ||
                                         Notification.HashHi != Notification.HashPrevHi);
        UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] FileUpdateV2: '%s' (rank %d, sender_changed=%d)"),
               *Notification.Filename, Notification.SourceRank, bSenderSaysChanged ? 1 : 0);
        if (MinimumFileSizeBytes > 0 && Notification.FileSize < MinimumFileSizeBytes)
        {
            UE_LOG(LogTemp, Verbose, TEXT("[LiveUpdate] Skipping tiny file in V2 notification: '%s' (%lld bytes)"),
                   *Notification.Filename, (long long)Notification.FileSize);
        }
        else
        {
            HandleFileUpdateNotification(Notification.Filename, Notification.SourceRank, Notification.FileSize,
                static_cast<uint64>(Notification.HashLo), static_cast<uint64>(Notification.HashHi),
                static_cast<uint64>(Notification.Timestamp));
        }
    }
    else if (Notification.Type == EJUSYNCNotificationType::CommitComplete)
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Commit complete notification"));
        HandleCommitCompleteNotification();
    }
}

void AJUSYNCFileSpawnerActor::HandleFileUpdateNotification(const FString& Filename, int32_t SourceRank, int64 FileSize, uint64 HashLo, uint64 HashHi, uint64 SampleId)
{
    if (!bAutoRefreshMeshes || Filename.IsEmpty())
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping file update: auto-refresh disabled or empty filename"));
        return;
    }

    if (!bInitialSpawnDone)
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Blocking: initial spawn not done yet (file='%s')"), *Filename);
        return;
    }

    ConsiderSenderLoop(Filename, SampleId);

    const uint64 OldHashLo = FileHashLo.FindRef(Filename);
    const uint64 OldHashHi = FileHashHi.FindRef(Filename);
    UpdateTrackedFileMetadata(Filename, SourceRank, FileSize, HashLo, HashHi);

    if (!JUSYNCIsLiveUpdateGeometryFile(Filename))
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping non-clip USD file: '%s'"), *Filename);
        return;
    }

    if (IsSuppressedElementVariant(Filename))
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping variant duplicate of a live element: '%s'"), *Filename);
        return;
    }

    if ((HashLo != 0 || HashHi != 0) &&
        OldHashLo == HashLo &&
        OldHashHi == HashHi)
    {
        bool bAlreadyApplied = false;
#if WITH_ANARI_USD_MIDDLEWARE
        bool bAppliedChecked = false;
        if (UJUSYNCSubsystem* ApplyCheck = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
        {
            if (const FJUSYNCPointCloudSpawner* PCApply = ApplyCheck->GetPointCloudSpawner())
            {
                bAppliedChecked = true;
                bAlreadyApplied = PCApply->HasAppliedFileHash(Filename, HashLo, HashHi);
            }
        }
#endif
        if (!bAppliedChecked)
        {
            const int64* AppliedSize = AppliedFileSizes.Find(Filename);
            bAlreadyApplied = AppliedSize && *AppliedSize == FileSize;
        }
        if (bAlreadyApplied)
        {
            UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Dedup: hash already applied for '%s'"), *Filename);
            return;
        }
        UE_LOG(LogTemp, Display,
            TEXT("[LiveUpdate] Dedup bypassed for '%s': hash tracked but never applied"), *Filename);
    }

    const FString LogicalKey = JUSYNCBuildLogicalActorKey(Filename);
    if (!LogicalKey.IsEmpty() && ChangeTracker)
    {
        const FString PreviousTracked = LogicalTrackedFile.FindRef(LogicalKey);
        if (!PreviousTracked.IsEmpty() && PreviousTracked != Filename)
        {
            const FJUSYNCFileChangeRequest* PrevReq = ChangeTracker->Find(PreviousTracked);
            if (PrevReq &&
                PrevReq->State != EJUSYNCFileChangeState::Completed &&
                PrevReq->State != EJUSYNCFileChangeState::Failed)
            {
                UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Superseding variant '%s' with '%s' (element '%s')"),
                    *PreviousTracked, *Filename, *LogicalKey);
#if WITH_ANARI_USD_MIDDLEWARE
                if (const uint64* AbortHandle = ActiveDownloadAborts.Find(PreviousTracked))
                {
                    JusyncAbortTrigger_C(*AbortHandle);
                }
#endif
                ChangeTracker->Remove(PreviousTracked);
            }
        }
        LogicalTrackedFile.Add(LogicalKey, Filename);
    }

    const int32 TargetRank = ResolveFileRank(Filename, SourceRank);
    SeenV2Files.Add(Filename, TargetRank);
    SampleId = ResolveLiveSampleId(SampleId);

    JUSYNC_DIAG_MARK(Filename, JUSYNC_DIAG_QUEUE, static_cast<uint64>(FileSize));

    if (ChangeTracker && ChangeTracker->Contains(Filename))
    {
        const FJUSYNCFileChangeRequest* ReqBefore = ChangeTracker->Find(Filename);
        const bool bWasQueued = ReqBefore && ReqBefore->State == EJUSYNCFileChangeState::Queued;

        ChangeTracker->QueueChange(Filename, TargetRank, FileSize, HashLo, HashHi, -1, false, SampleId);

        if (bWasQueued)
        {
            UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Coalesced queued update for '%s'"), *Filename);
        }
        else
        {
            UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Superseded in-flight update for '%s'"), *Filename);
#if WITH_ANARI_USD_MIDDLEWARE
            if (const uint64* AbortHandle = ActiveDownloadAborts.Find(Filename))
            {
                if (JusyncAbortTrigger_C(*AbortHandle))
                {
                    UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Aborted in-flight download of '%s' (newer data requested)"), *Filename);
                }
            }
#endif
        }
        ChainRefreshNext();
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Refreshing file: '%s' from rank %d"), *Filename, TargetRank);

    if (QueueRefreshFile(Filename, TargetRank, FileSize, HashLo, HashHi, SampleId))
    {
        ChainRefreshNext();
    }
}

void AJUSYNCFileSpawnerActor::HandleCommitCompleteNotification(bool bIsTimer)
{
    if (!bAutoRefreshMeshes)
    {
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Skipping commit complete: auto-refresh disabled"));
        return;
    }

    if (!bIsTimer)
    {
        LastClosedLiveSampleId = LiveSampleId;
        bLiveSampleFrameClosed = true;
    }

    double Now = FPlatformTime::Seconds();
    if (bSceneDiffInFlight)
    {
        bCommitDiffPending = true;
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Deferring commit complete: diff already in progress"));
        return;
    }

    if (ChangeTracker && ChangeTracker->HasActiveWork())
    {
        bCommitDiffPending = true;
        UE_LOG(LogTemp, Verbose, TEXT("[LiveUpdate] Deferring commit complete: file changes active"));
        return;
    }

    if (!bIsTimer && Now - LastCommitCompleteTime < CommitCompleteCooldownSeconds)
    {
        double Remaining = CommitCompleteCooldownSeconds - (Now - LastCommitCompleteTime);
        bCommitDiffPending = true;
        UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Deferring commit complete: cooldown active (%.1fs remaining)"), Remaining);
        return;
    }

    bCommitDiffPending = false;
    LastCommitCompleteTime = Now;
    bSceneDiffInFlight = true;
    UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] Commit complete - re-fetching file list"));

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem || !Subsystem->IsBrokerConnected())
    {
        UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Cannot refresh: broker not connected"));
        bSceneDiffInFlight = false;
        return;
    }

    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis]()
    {
        if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

        TArray<FString> NewFiles;
        TArray<int64> NewSizes;
        TArray<int32> NewRanks;
        TArray<uint64> NewHashLo;
        TArray<uint64> NewHashHi;
        bool bSuccess = WeakSubsystem->RequestFileListWithSizesAndRanks(-1, 15000, NewFiles, NewSizes, NewRanks, &NewHashLo, &NewHashHi);

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakThis, NewFiles = MoveTemp(NewFiles), NewSizes = MoveTemp(NewSizes), NewRanks = MoveTemp(NewRanks), NewHashLo = MoveTemp(NewHashLo), NewHashHi = MoveTemp(NewHashHi), bSuccess]()
            {
                if (!WeakThis.IsValid())
                {
                    return;
                }

                if (!bSuccess || NewFiles.Num() == 0)
                {
                    WeakThis->bSceneDiffInFlight = false;
                    return;
                }

                WeakThis->RawHashLo = NewHashLo;
                WeakThis->RawHashHi = NewHashHi;
                WeakThis->DiffAndRefreshFileList(NewFiles, NewSizes, NewRanks, false);
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
}

void AJUSYNCFileSpawnerActor::DiffAndRefreshFileList(const TArray<FString>& NewFiles, const TArray<int64>& NewSizes, const TArray<int32>& NewRanks, bool bIsManual)
{
    if (ChangeTracker && ChangeTracker->HasActiveWork())
    {
        UE_LOG(LogTemp, Verbose, TEXT("[LiveUpdate] Skipping diff: file changes already active"));
        bSceneDiffInFlight = false;
        bCommitDiffPending = true;
        return;
    }

    if (UJUSYNCSubsystem* GrdSubsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem())
    {
        MaybeTriggerGradientLoad(NewFiles, NewSizes, NewRanks, GrdSubsystem);
    }

    TArray<FString> OldFileList = FilteredFiles;

    TMap<FString, int64> OldSizes;
    TMap<FString, uint64> OldHashLo;
    TMap<FString, uint64> OldHashHi;
    for (int32 i = 0; i < FilteredFiles.Num(); ++i)
    {
        OldSizes.Add(FilteredFiles[i], FilteredSizes.IsValidIndex(i) ? FilteredSizes[i] : 0);
        if (FilteredHashLo.IsValidIndex(i) && FilteredHashHi.IsValidIndex(i))
        {
            OldHashLo.Add(FilteredFiles[i], FilteredHashLo[i]);
            OldHashHi.Add(FilteredFiles[i], FilteredHashHi[i]);
        }
    }

    TArray<FString> NewFilteredFiles;
    TArray<int64> NewFilteredSizes;
    TArray<int32> NewFilteredRanks;
    TArray<uint64> NewFilteredHashLo;
    TArray<uint64> NewFilteredHashHi;
    TArray<FString> NewPngFiles;
    TArray<int64> NewPngSizes;
    TArray<int32> NewPngRanks;
    FilterFileList(NewFiles, NewSizes, NewRanks, RawHashLo, RawHashHi,
        NewFilteredFiles, NewFilteredSizes, NewFilteredRanks, NewFilteredHashLo, NewFilteredHashHi,
        NewPngFiles, NewPngSizes, NewPngRanks);

    CollapseElementVariants(NewFilteredFiles, NewFilteredSizes, NewFilteredRanks,
        NewFilteredHashLo, NewFilteredHashHi);

    int32 ChangedCount = 0;
    int32 NewCount = 0;
    const uint64 DiffSampleId = (!bIsManual && LastClosedLiveSampleId != 0)
        ? LastClosedLiveSampleId
        : 0;

    for (int32 i = 0; i < NewFilteredFiles.Num(); ++i)
    {
        const FString& Fname = NewFilteredFiles[i];
        const int64 NewSize = NewFilteredSizes[i];
        const int32 Rank = NewFilteredRanks[i];
        const uint64 NewHLo = NewFilteredHashLo[i];
        const uint64 NewHHi = NewFilteredHashHi[i];

        if (MinimumFileSizeBytes > 0 && NewSize < MinimumFileSizeBytes)
        {
            UE_LOG(LogTemp, Verbose, TEXT("[LiveUpdate] Skipping tiny file '%s' (%lld bytes)"), *Fname, (long long)NewSize);
            continue;
        }

        int64 OldSize = 0;
        int64* pOldSize = OldSizes.Find(Fname);
        bool bFound = (pOldSize != nullptr);
        if (bFound) OldSize = *pOldSize;

        uint64 OldHLo = 0, OldHHi = 0;
        uint64* pHLo = OldHashLo.Find(Fname);
        uint64* pHHi = OldHashHi.Find(Fname);
        bool bHasOldHash = (pHLo != nullptr && pHHi != nullptr);
        if (bHasOldHash) { OldHLo = *pHLo; OldHHi = *pHHi; }

        bool bHashChanged = (NewHLo != OldHLo || NewHHi != OldHHi);
        bool bSizeChanged = (NewSize != OldSize);

        bool bShouldRefresh = bIsManual || !bFound || bSizeChanged;
        if (!bShouldRefresh && NewHLo && bHashChanged)
            bShouldRefresh = true;
        if (!bShouldRefresh && !bHasOldHash && NewHLo)
            bShouldRefresh = true;

        if (bShouldRefresh)
        {
            if (QueueRefreshFile(Fname, Rank, NewSize, NewHLo, NewHHi, DiffSampleId))
            {
                const char* Reason = bIsManual ? "manual" : (!bFound ? "new" : (bSizeChanged ? "size" : "hash"));
                UE_LOG(LogTemp, Verbose, TEXT("[LiveUpdate] File changed '%s': %s"), *Fname, ANSI_TO_TCHAR(Reason));
                if (!bFound) NewCount++; else ChangedCount++;
            }
        }
    }

    TSet<FString> NewFileSet;
    NewFileSet.Reserve(NewFiles.Num());
    for (const FString& Nf : NewFiles)
    {
        NewFileSet.Add(Nf);
    }

    TSet<FString> LiveLogicalKeys;
    LiveLogicalKeys.Reserve(NewFiles.Num());
    for (const FString& LiveFile : NewFiles)
    {
        const FString Logical = JUSYNCBuildLogicalActorKey(LiveFile);
        if (!Logical.IsEmpty())
        {
            LiveLogicalKeys.Add(Logical);
        }
    }

    TMap<FString, FString> LiveFileByStream;
    LiveFileByStream.Reserve(NewFiles.Num());
    for (const FString& LiveFile : NewFiles)
    {
        const FString Stream = BuildClipStreamKey(LiveFile);
        if (!Stream.IsEmpty())
        {
            LiveFileByStream.Add(Stream, LiveFile);
        }
    }

    int32 DeletedCount = 0;
    const float GraceSeconds =
        FMath::Max(0.0f, CVarJusyncDeletedFileGrace.GetValueOnGameThread());
    for (const FString& OldName : OldFileList)
    {
        if (NewFileSet.Contains(OldName))
        {
            PendingDeletedFiles.Remove(OldName);
            continue;
        }

        const FString OldLogical = JUSYNCBuildLogicalActorKey(OldName);
        if (!OldLogical.IsEmpty() && LiveLogicalKeys.Contains(OldLogical))
        {
            UE_LOG(LogTemp, Verbose, TEXT("[LiveUpdate] Keeping actor(s) for variant file '%s' (logical '%s' still live)"),
                *OldName, *OldLogical);
            PendingDeletedFiles.Remove(OldName);
            FileHashLo.Remove(OldName);
            FileHashHi.Remove(OldName);
            FileLastSize.Remove(OldName);
            SeenV2Files.Remove(OldName);
            continue;
        }

        const FString OldStream = BuildClipStreamKey(OldName);
        if (!OldStream.IsEmpty())
        {
            if (const FString* LiveFile = LiveFileByStream.Find(OldStream))
            {
                TransferFileActorOwnership(OldName, *LiveFile);
                UE_LOG(LogTemp, Verbose,
                    TEXT("[LiveUpdate] Rolled clip '%s' onto live file '%s' (actors kept)"),
                    *OldName, **LiveFile);
            }
            else
            {
                PendingDeletedFiles.Add(OldName,
                    (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f) + GraceSeconds);
                UE_LOG(LogTemp, Verbose,
                    TEXT("[LiveUpdate] Deferred destroy of deleted file '%s' (%.1fs grace)"),
                    *OldName, GraceSeconds);
            }
        }
        else
        {
            const int32 Destroyed = DestroyFileActors(OldName);
            if (Destroyed > 0)
            {
                DeletedCount += Destroyed;
                UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] Destroyed %d orphan actor(s) for deleted file '%s'"), Destroyed, *OldName);
            }
        }

        FileHashLo.Remove(OldName);
        FileHashHi.Remove(OldName);
        FileLastSize.Remove(OldName);
        SeenV2Files.Remove(OldName);
    }
    if (DeletedCount > 0)
    {
        UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] Cleaned up %d orphan actors from deleted files"), DeletedCount);
    }

    const int32 SkippedCount = FMath::Max(0, NewFiles.Num() - NewFilteredFiles.Num());
    FString Tag = bIsManual ? TEXT("[ManualRefresh]") : TEXT("[LiveUpdate]");
    FString Summary = FString::Printf(TEXT("%s %d changed, %d new, %d deleted, %d skipped"), *Tag, ChangedCount, NewCount, DeletedCount, SkippedCount);
    UE_LOG(LogTemp, Display, TEXT("%s"), *Summary);

    RawFileList = NewFiles;
    RawFileSizes = NewSizes;
    RawFileRanks = NewRanks;
    FilteredFiles = MoveTemp(NewFilteredFiles);
    FilteredSizes = MoveTemp(NewFilteredSizes);
    FilteredRanks = MoveTemp(NewFilteredRanks);
    FilteredHashLo = MoveTemp(NewFilteredHashLo);
    FilteredHashHi = MoveTemp(NewFilteredHashHi);
    FilesTotal = FilteredFiles.Num();

    for (int32 i = 0; i < FilteredFiles.Num(); ++i)
    {
        FileLastSize.Add(FilteredFiles[i], FilteredSizes.IsValidIndex(i) ? FilteredSizes[i] : 0);
        FileHashLo.Add(FilteredFiles[i], FilteredHashLo.IsValidIndex(i) ? FilteredHashLo[i] : 0);
        FileHashHi.Add(FilteredFiles[i], FilteredHashHi.IsValidIndex(i) ? FilteredHashHi[i] : 0);
    }

    bSceneDiffInFlight = false;

    if (ChangeTracker && ChangeTracker->QueuedCount() > 0)
    {
        ChainRefreshNext();
    }
}

void AJUSYNCFileSpawnerActor::ManualRefresh()
{
    UE_LOG(LogTemp, Display, TEXT("[ManualRefresh] Triggered manual refresh"));

    if (CurrentState != EJUSYNCSpawnerState::Complete)
    {
        UE_LOG(LogTemp, Warning, TEXT("[ManualRefresh] Not in Complete state (%d), cannot refresh"), (int32)CurrentState);
        return;
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem || !Subsystem->IsBrokerConnected())
    {
        UE_LOG(LogTemp, Warning, TEXT("[ManualRefresh] Broker not connected"));
        return;
    }

    bSceneDiffInFlight = true;

    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis]()
    {
        if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

        TArray<FString> NewFiles;
        TArray<int64> NewSizes;
        TArray<int32> NewRanks;
        TArray<uint64> NewHashLo;
        TArray<uint64> NewHashHi;
        bool bSuccess = WeakSubsystem->RequestFileListWithSizesAndRanks(-1, 15000, NewFiles, NewSizes, NewRanks, &NewHashLo, &NewHashHi);

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakThis, NewFiles = MoveTemp(NewFiles), NewSizes = MoveTemp(NewSizes), NewRanks = MoveTemp(NewRanks), NewHashLo = MoveTemp(NewHashLo), NewHashHi = MoveTemp(NewHashHi), bSuccess]()
            {
                if (!WeakThis.IsValid())
                {
                    return;
                }

                if (!bSuccess || NewFiles.Num() == 0)
                {
                    WeakThis->bSceneDiffInFlight = false;
                    return;
                }

                WeakThis->RawHashLo = NewHashLo;
                WeakThis->RawHashHi = NewHashHi;
                WeakThis->DiffAndRefreshFileList(NewFiles, NewSizes, NewRanks, true);
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
}

void AJUSYNCFileSpawnerActor::StartLiveUpdatePolling()
{
    if (!bEnableLiveUpdates || LiveUpdatePollInterval <= 0.0f)
        return;

    if (GWorld)
    {
        GWorld->GetTimerManager().ClearTimer(LiveUpdateTimerHandle);
    }
    UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Tick-driven backstop polling active (interval: %.1fs)"), LiveUpdatePollInterval);
}

void AJUSYNCFileSpawnerActor::StopLiveUpdatePolling()
{
    if (GWorld)
    {
        GWorld->GetTimerManager().ClearTimer(LiveUpdateTimerHandle);
    }
}

void AJUSYNCFileSpawnerActor::OnLiveUpdateTimer()
{
    UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] TICK complete=%d autoRefresh=%d activeWork=%d diffBusy=%d initialDone=%d tracked=%d raw=%d"),
        CurrentState == EJUSYNCSpawnerState::Complete ? 1 : 0,
        bAutoRefreshMeshes ? 1 : 0,
        (ChangeTracker && ChangeTracker->HasActiveWork()) ? 1 : 0,
        bSceneDiffInFlight ? 1 : 0,
        bInitialSpawnDone ? 1 : 0,
        FilteredFiles.Num(),
        RawFileList.Num());

    if (CurrentState != EJUSYNCSpawnerState::Complete)
        return;

    if (bAutoRefreshMeshes)
    {
        HandleCommitCompleteNotification(true);
        return;
    }

    if (!bInitialSpawnDone) return;
    if (bSceneDiffInFlight) return;

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem || !Subsystem->IsBrokerConnected()) return;

    bSceneDiffInFlight = true;

    TWeakObjectPtr<AJUSYNCFileSpawnerActor> WeakThis = this;
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSubsystem, WeakThis]()
    {
        if (!WeakSubsystem.IsValid() || !WeakThis.IsValid()) return;

        TArray<FString> NewFiles;
        TArray<int64> NewSizes;
        TArray<int32> NewRanks;
        TArray<uint64> NewHashLo;
        TArray<uint64> NewHashHi;
        bool bSuccess = WeakSubsystem->RequestFileListWithSizesAndRanks(-1, 10000, NewFiles, NewSizes, NewRanks, &NewHashLo, &NewHashHi);

        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [WeakThis, NewFiles = MoveTemp(NewFiles), NewSizes = MoveTemp(NewSizes), NewRanks = MoveTemp(NewRanks), NewHashLo = MoveTemp(NewHashLo), NewHashHi = MoveTemp(NewHashHi), bSuccess]()
            {
                if (!WeakThis.IsValid() || !bSuccess || NewFiles.Num() == 0)
                {
                    if (WeakThis.IsValid()) WeakThis->bSceneDiffInFlight = false;
                    return;
                }

                WeakThis->RawHashLo = NewHashLo;
                WeakThis->RawHashHi = NewHashHi;
                WeakThis->DiffAndRefreshFileList(NewFiles, NewSizes, NewRanks, false);
            },
            TStatId(), nullptr, ENamedThreads::GameThread);
    });
}

bool AJUSYNCFileSpawnerActor::QueueRefreshFile(const FString& Filename, int32 Rank, int64 Size, uint64 HashLo, uint64 HashHi, uint64 SampleId)
{
    if (!bAutoRefreshMeshes || Filename.IsEmpty())
    {
        return false;
    }
    if (!ChangeTracker)
    {
        return false;
    }
    if (ChangeTracker->Contains(Filename))
    {
        return false;
    }

    const FString LogicalKey = JUSYNCBuildLogicalActorKey(Filename);
    if (!LogicalKey.IsEmpty())
    {
        LogicalTrackedFile.Add(LogicalKey, Filename);
    }

    ChangeTracker->QueueChange(Filename, Rank, Size, HashLo, HashHi, -1, false, SampleId);
    return true;
}

int32 AJUSYNCFileSpawnerActor::ResolveFileRank(const FString& Filename, int32 KnownRank) const
{
    if (KnownRank >= 0)
    {
        return KnownRank;
    }

    int32 ParsedRank = INDEX_NONE;
    if (JUSYNCElementNames::ParseFileRank(Filename, ParsedRank))
    {
        return ParsedRank;
    }

    for (int32 i = 0; i < FilteredFiles.Num(); ++i)
    {
        if (FilteredFiles[i] == Filename && FilteredRanks.IsValidIndex(i) && FilteredRanks[i] >= 0)
        {
            return FilteredRanks[i];
        }
    }
    for (int32 i = 0; i < RawFileList.Num(); ++i)
    {
        if (RawFileList[i] == Filename && RawFileRanks.IsValidIndex(i) && RawFileRanks[i] >= 0)
        {
            return RawFileRanks[i];
        }
    }
    if (const int32* pRank = SeenV2Files.Find(Filename))
    {
        if (*pRank >= 0)
        {
            return *pRank;
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] No owner rank known for '%s'; fetching from rank 0 (a multi-rank piece will fail over until the next file-list poll)"), *Filename);
    return 0;
}

void AJUSYNCFileSpawnerActor::UpdateTrackedFileMetadata(const FString& Filename, int32 Rank, int64 Size, uint64 HashLo, uint64 HashHi)
{
    if (Filename.IsEmpty())
    {
        return;
    }

    if (Rank >= 0)
    {
        SeenV2Files.Add(Filename, Rank);
    }
    if (Size > 0)
    {
        FileLastSize.Add(Filename, Size);
    }
    if (HashLo != 0 || HashHi != 0)
    {
        FileHashLo.Add(Filename, HashLo);
        FileHashHi.Add(Filename, HashHi);
    }

    for (int32 i = 0; i < FilteredFiles.Num(); ++i)
    {
        if (FilteredFiles[i] != Filename)
        {
            continue;
        }
        if (Rank >= 0 && FilteredRanks.IsValidIndex(i)) FilteredRanks[i] = Rank;
        if (Size > 0 && FilteredSizes.IsValidIndex(i)) FilteredSizes[i] = Size;
        if (HashLo != 0 || HashHi != 0)
        {
            if (HashLo != 0 && FilteredHashLo.IsValidIndex(i)) FilteredHashLo[i] = HashLo;
            if (HashHi != 0 && FilteredHashHi.IsValidIndex(i)) FilteredHashHi[i] = HashHi;
        }
        break;
    }

    for (int32 i = 0; i < RawFileList.Num(); ++i)
    {
        if (RawFileList[i] != Filename)
        {
            continue;
        }
        if (Rank >= 0 && RawFileRanks.IsValidIndex(i)) RawFileRanks[i] = Rank;
        if (Size > 0 && RawFileSizes.IsValidIndex(i)) RawFileSizes[i] = Size;
        if (HashLo != 0 || HashHi != 0)
        {
            if (HashLo != 0 && RawHashLo.IsValidIndex(i)) RawHashLo[i] = HashLo;
            if (HashHi != 0 && RawHashHi.IsValidIndex(i)) RawHashHi[i] = HashHi;
        }
        break;
    }
}

int32 AJUSYNCFileSpawnerActor::CollapseElementVariants(
    TArray<FString>& Files, TArray<int64>& Sizes, TArray<int32>& Ranks,
    TArray<uint64>& HashLo, TArray<uint64>& HashHi)
{
    if (CVarJusyncCollapseVariants.GetValueOnGameThread() == 0)
    {
        ElementVariantWinners.Reset();
        IgnoredVariantFiles.Reset();
        return 0;
    }

    struct FElection
    {
        int32 Uniq = -1;
        int32 Index = INDEX_NONE;
    };

    TMap<FString, FElection> Best;
    TMap<FString, TArray<int32>> Members;
    Best.Reserve(Files.Num());
    Members.Reserve(Files.Num());

    for (int32 i = 0; i < Files.Num(); ++i)
    {
        if (JUSYNCIsSubsetStreamFile(Files[i]))
        {
            continue;
        }

        const JUSYNCElementNames::FVariant Variant = JUSYNCElementNames::Parse(Files[i]);
        if (Variant.Element.IsEmpty())
        {
            continue;
        }
        const FString Group = Variant.GroupKey();
        Members.FindOrAdd(Group).Add(i);

        const FElection* Cur = Best.Find(Group);
        if (!Cur || Variant.Uniq > Cur->Uniq)
        {
            Best.Add(Group, { Variant.Uniq, i });
        }
    }

    TMap<FString, TPair<int32, FString>> NewWinners;
    TSet<FString> Losers;
    for (const TPair<FString, FElection>& Pair : Best)
    {
        if (Pair.Value.Uniq < 0)
        {
            continue;
        }

        const FString& WinnerFile = Files[Pair.Value.Index];
        NewWinners.Add(Pair.Key, TPair<int32, FString>(Pair.Value.Uniq, WinnerFile));

        for (const int32 Idx : Members[Pair.Key])
        {
            if (Idx != Pair.Value.Index)
            {
                Losers.Add(Files[Idx]);
            }
        }
    }

    for (const TPair<FString, TPair<int32, FString>>& Prev : ElementVariantWinners)
    {
        const TPair<int32, FString>* Now = NewWinners.Find(Prev.Key);
        if (!Now || Now->Value == Prev.Value.Value)
        {
            continue;
        }
        const int32 Destroyed = DestroyFileActors(Prev.Value.Value);
        if (Destroyed > 0)
        {
            UE_LOG(LogTemp, Display,
                TEXT("[LiveUpdate] Variant collapse: destroyed %d actor(s) of superseded file '%s'"),
                Destroyed, *Prev.Value.Value);
        }
    }

    ElementVariantWinners = MoveTemp(NewWinners);
    IgnoredVariantFiles = Losers;

    if (Losers.Num() == 0)
    {
        return 0;
    }

    UE_LOG(LogTemp, Log, TEXT("[LiveUpdate] Variant collapse: suppressing %d duplicate file(s): %s"),
        Losers.Num(), *FString::Join(Losers.Array(), TEXT(", ")));

    int32 Pruned = 0;
    for (int32 i = Files.Num() - 1; i >= 0; --i)
    {
        if (!Losers.Contains(Files[i]))
        {
            continue;
        }
        Files.RemoveAtSwap(i);
        Sizes.RemoveAtSwap(i);
        Ranks.RemoveAtSwap(i);
        HashLo.RemoveAtSwap(i);
        HashHi.RemoveAtSwap(i);
        Pruned++;
    }
    return Pruned;
}

bool AJUSYNCFileSpawnerActor::IsSuppressedElementVariant(const FString& Filename)
{
    if (CVarJusyncCollapseVariants.GetValueOnGameThread() == 0 || Filename.IsEmpty())
    {
        return false;
    }

    if (JUSYNCIsSubsetStreamFile(Filename))
    {
        return false;
    }

    const JUSYNCElementNames::FVariant Variant = JUSYNCElementNames::Parse(Filename);
    if (Variant.Element.IsEmpty())
    {
        return false;
    }

    if (IgnoredVariantFiles.Contains(Filename))
    {
        return true;
    }

    const FString Group = Variant.GroupKey();
    TPair<int32, FString>* Current = ElementVariantWinners.Find(Group);
    if (!Current)
    {
        ElementVariantWinners.Add(Group, TPair<int32, FString>(Variant.Uniq, Filename));
        return false;
    }
    if (Current->Value == Filename)
    {
        return false;
    }
    if (Variant.Uniq < 0 && Current->Key < 0)
    {
        return false;
    }

    if (Variant.Uniq > Current->Key)
    {
        const FString Evicted = Current->Value;
        Current->Key = Variant.Uniq;
        Current->Value = Filename;
        IgnoredVariantFiles.Add(Evicted);

        const int32 Destroyed = DestroyFileActors(Evicted);
        UE_LOG(LogTemp, Display, TEXT("[LiveUpdate] Variant collapse: '%s' supersedes '%s' (%d actor(s) removed)"),
            *Filename, *Evicted, Destroyed);
        return false;
    }

    if (IgnoredVariantFiles.Num() > 4096)
    {
        IgnoredVariantFiles.Reset();
    }
    IgnoredVariantFiles.Add(Filename);
    return true;
}

int32 AJUSYNCFileSpawnerActor::DestroyFileActors(const FString& Filename)
{
    if (Filename.IsEmpty())
    {
        return 0;
    }

    TArray<AActor*> MeshActors;
    if (TArray<AActor*>* pMeshActors = FilenameToActors.Find(Filename))
    {
        MeshActors.Append(*pMeshActors);
    }

    TArray<AActor*> PCActors;
    if (TArray<AActor*>* pPCActors = FilenameToPCActors.Find(Filename))
    {
        PCActors.Append(*pPCActors);
    }

    if (MeshActors.Num() == 0 && PCActors.Num() == 0)
    {
        return 0;
    }

    TSet<AActor*> PCSet;
    TSet<FString> FileElements;
    for (AActor* Actor : PCActors)
    {
        if (Actor && Actor->IsValidLowLevel())
        {
            PCSet.Add(Actor);
        }
    }

    TSet<AActor*> Processed;
    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    FJUSYNCPointCloudSpawner* PCSpawner = Subsystem ? Subsystem->GetPointCloudSpawner() : nullptr;
    if (PCSpawner)
    {
        for (AActor* Actor : PCActors)
        {
            if (!Actor || !Actor->IsValidLowLevel())
            {
                continue;
            }
            const FString Key = PCSpawner->GetElementKeyForActor(Actor);
            if (!Key.IsEmpty())
            {
                FileElements.Add(JUSYNCCanonicalFromRankedElementKey(Key));
            }
        }
    }
    int32 Destroyed = 0;

    auto DestroyOne = [&](AActor* Actor)
    {
        if (!Actor || !Actor->IsValidLowLevel() || Processed.Contains(Actor))
        {
            return;
        }
        Processed.Add(Actor);
        SpawnedActors.Remove(Actor);
        if (ActorsSpawned > 0)
        {
            ActorsSpawned--;
        }
        if (AnimationController) AnimationController->RemoveActor(Actor);
        if (PCSet.Contains(Actor) && PCSpawner)
        {
            PCSpawner->DestroyTrackedActor(Actor);
        }
        else
        {
            Actor->Destroy();
        }
        Destroyed++;
    };

    for (AActor* Actor : MeshActors)
    {
        DestroyOne(Actor);
    }
    for (AActor* Actor : PCActors)
    {
        DestroyOne(Actor);
    }

    if (TArray<AActor*>* pMeshActors = FilenameToActors.Find(Filename))
    {
        pMeshActors->Empty();
    }
    if (TArray<AActor*>* pPCActors = FilenameToPCActors.Find(Filename))
    {
        pPCActors->Empty();
    }

    for (auto It = FileToActorMap.CreateIterator(); It; ++It)
    {
        if (Processed.Contains(It.Value()))
        {
            It.RemoveCurrent();
        }
    }

    for (auto It = PCElementToFilename.CreateIterator(); It; ++It)
    {
        if (It.Value() == Filename)
        {
            It.RemoveCurrent();
        }
    }

    if (PathlineManager && FileElements.Num() > 0)
    {
        for (const FString& Element : FileElements)
        {
            bool bRankRemains = false;
            for (const TPair<FString, FString>& Pair : PCElementToFilename)
            {
                if (Pair.Key == Element ||
                    Pair.Key.StartsWith(Element + TEXT("_r"), ESearchCase::CaseSensitive))
                {
                    bRankRemains = true;
                    break;
                }
            }
            if (!bRankRemains)
            {
                PathlineManager->RemoveElement(Element);
            }
        }
    }

    return Destroyed;
}

void AJUSYNCFileSpawnerActor::TransferFileActorOwnership(const FString& FromFile,
    const FString& ToFile)
{
    if (FromFile.IsEmpty() || ToFile.IsEmpty() || FromFile == ToFile)
    {
        return;
    }

    auto MoveList = [&](TMap<FString, TArray<AActor*>>& Map)
    {
        TArray<AActor*>* From = Map.Find(FromFile);
        if (!From || From->Num() == 0)
        {
            return;
        }
        TArray<AActor*>& To = Map.FindOrAdd(ToFile);
        for (AActor* Actor : *From)
        {
            if (Actor && Actor->IsValidLowLevel() && !To.Contains(Actor))
            {
                To.Add(Actor);
            }
        }
        From->Reset();
    };
    MoveList(FilenameToActors);
    MoveList(FilenameToPCActors);

    for (TPair<FString, FString>& Pair : PCElementToFilename)
    {
        if (Pair.Value == FromFile)
        {
            Pair.Value = ToFile;
        }
    }

    auto RemapKey = [&](const FString& FromKey, const FString& ToKey)
    {
        if (FromKey.IsEmpty() || FromKey == ToKey)
        {
            return;
        }
        if (AActor** pActor = FileToActorMap.Find(FromKey))
        {
            AActor* Actor = *pActor;
            FileToActorMap.Remove(FromKey);
            if (Actor && Actor->IsValidLowLevel() && !FileToActorMap.Contains(ToKey))
            {
                FileToActorMap.Add(ToKey, Actor);
            }
        }
    };
    RemapKey(FromFile, ToFile);
    RemapKey(JUSYNCBuildLogicalActorKey(FromFile), JUSYNCBuildLogicalActorKey(ToFile));
}

void AJUSYNCFileSpawnerActor::SweepPendingDeletedFiles()
{
    if (PendingDeletedFiles.Num() == 0)
    {
        return;
    }
    const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

    TMap<FString, FString> LiveFileByStream;
    LiveFileByStream.Reserve(RawFileList.Num());
    for (const FString& LiveFile : RawFileList)
    {
        const FString Stream = BuildClipStreamKey(LiveFile);
        if (!Stream.IsEmpty())
        {
            LiveFileByStream.Add(Stream, LiveFile);
        }
    }

    for (auto It = PendingDeletedFiles.CreateIterator(); It; ++It)
    {
        const FString& Name = It.Key();
        if (RawFileList.Contains(Name))
        {
            It.RemoveCurrent();
            continue;
        }

        const FString Stream = BuildClipStreamKey(Name);
        const FString* LiveFile =
            Stream.IsEmpty() ? nullptr : LiveFileByStream.Find(Stream);
        if (LiveFile)
        {
            TransferFileActorOwnership(Name, *LiveFile);
            UE_LOG(LogTemp, Verbose,
                TEXT("[LiveUpdate] Rolled retired clip '%s' onto late file '%s' (actors kept)"),
                *Name, **LiveFile);
            It.RemoveCurrent();
            continue;
        }

        if (Now < It.Value())
        {
            continue;
        }

        if (!Stream.IsEmpty())
        {
            UE_LOG(LogTemp, Display, TEXT(
                "[LiveUpdate] Clip file '%s' left the store without a successor - "
                "holding last frame (stream paused/ended or sender store cleanup)"), *Name);
            It.RemoveCurrent();
            continue;
        }

        const int32 Destroyed = DestroyFileActors(Name);
        if (Destroyed > 0)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[LiveUpdate] Destroyed %d orphan actor(s) for deleted file '%s' (grace expired)"),
                Destroyed, *Name);
        }
        It.RemoveCurrent();
    }
}

void AJUSYNCFileSpawnerActor::AbortAllInFlightDownloads()
{
    for (const TPair<FString, uint64>& Pair : ActiveDownloadAborts)
    {
#if WITH_ANARI_USD_MIDDLEWARE
        JusyncAbortTrigger_C(Pair.Value);
        JusyncAbortRelease_C(Pair.Value);
#endif
    }
    ActiveDownloadAborts.Reset();
}

void AJUSYNCFileSpawnerActor::ChainRefreshNext()
{
    if (bIsCancelled) return;
    if (!ChangeTracker) return;
    if (ChangeTracker->QueuedCount() == 0) return;

    if (PipelineActive > 0 && ChangeTracker->ActiveCount() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] PipelineActive=%d stale (no active change) - resetting"), PipelineActive);
        PipelineActive = 0;
    }

    UJUSYNCSubsystem* Subsystem = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Warning, TEXT("[LiveUpdate] ChainRefreshNext: subsystem unavailable, leaving %d files queued"), ChangeTracker->QueuedCount());
        return;
    }

    int32 AvailableSpots = FMath::Max(0, PipelineDepth - PipelineActive);
    int32 Spawned = 0;

    while (AvailableSpots > 0 && ChangeTracker->QueuedCount() > 0)
    {
        TPair<FString, int32> Popped = ChangeTracker->PopNextQueued();
        if (Popped.Key.IsEmpty())
        {
            break;
        }

        const FJUSYNCFileChangeRequest* Request = ChangeTracker->Find(Popped.Key);
        PipelineActive++;
        AvailableSpots--;

        LoadFileThroughPipeline(
            Popped.Key,
            Popped.Value,
            Request ? Request->Size : 0,
            Request ? Request->HashLo : 0,
            Request ? Request->HashHi : 0,
            Request ? Request->FileIndex : -1,
            false,
            Request ? Request->SampleId : 0);

        Spawned++;
    }

    if (Spawned == 0 && ChangeTracker->QueuedCount() > 0)
    {
        FTimerHandle RetryHandle;
        GetWorldTimerManager().SetTimer(RetryHandle, FTimerDelegate::CreateUObject(this, &AJUSYNCFileSpawnerActor::ChainRefreshNext), 0.02f, false);
    }
}

void AJUSYNCFileSpawnerActor::RetryRemainingFiles()
{
    if (FailedFileIndices.Num() == 0) return;
    if (CurrentRetryCount >= MaxRetries) return;

    RetryFailedDownloads();
}

void AJUSYNCFileSpawnerActor::PlayTimeStepAnimation()
{
    if (AnimationController)
    {
        AnimationController->Play();
    }
}

void AJUSYNCFileSpawnerActor::StopTimeStepAnimation()
{
    if (AnimationController)
    {
        AnimationController->Stop();
    }
}

void AJUSYNCFileSpawnerActor::SetTimeStepIndex(int32 NewIndex)
{
    if (AnimationController)
    {
        AnimationController->SetCurrentTimeStepIndex(NewIndex);
    }
}

#if WITH_EDITOR

#include "NiagaraDataInterfaceArrayFloat.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	struct FJUSYNCStreamlineParamDef
	{
		const TCHAR* Name;
		UClass* Class;
	};

	void JUSYNCAddStreamlineNiagaraParameters()
	{
		const TCHAR* AssetPath = TEXT("/Game/FX/NS_JusyncStreamlines.NS_JusyncStreamlines");
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, AssetPath);
		if (!System)
		{
			UE_LOG(LogTemp, Error, TEXT("[JUSYNC] Could not load %s"), AssetPath);
			return;
		}

		const FJUSYNCStreamlineParamDef Defs[] =
		{
			{ TEXT("User.Positions"),  UNiagaraDataInterfaceArrayPosition::StaticClass() },
			{ TEXT("User.LinkOrders"), UNiagaraDataInterfaceArrayFloat::StaticClass() },
			{ TEXT("User.Widths"),     UNiagaraDataInterfaceArrayFloat::StaticClass() },
			{ TEXT("User.Colors"),     UNiagaraDataInterfaceArrayColor::StaticClass() },
			{ TEXT("User.Scalars"),    UNiagaraDataInterfaceArrayFloat::StaticClass() },
		};

		FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

		bool bChanged = false;
		for (const FJUSYNCStreamlineParamDef& Def : Defs)
		{
			const FName ParamName(Def.Name);
			FNiagaraVariable Var(FNiagaraTypeDefinition(Def.Class), ParamName);
			if (Store.GetDataInterface(Var) != nullptr)
			{
				UE_LOG(LogTemp, Log, TEXT("[JUSYNC] %s already present"), Def.Name);
				continue;
			}

			int32 Offset = INDEX_NONE;
			Store.AddParameter(Var,   false,   true, &Offset);  // bInitializebTriggerRebind
			UNiagaraDataInterface* DataInterface = NewObject<UNiagaraDataInterface>(
				System, Def.Class, FName(ParamName.ToString().RightChop(5)), RF_Public | RF_Transactional);
			Store.SetDataInterface(DataInterface, Var);
			bChanged = true;
			UE_LOG(LogTemp, Log, TEXT("[JUSYNC] Added data interface parameter %s"), Def.Name);
		}

		if (!bChanged)
		{
			return;
		}

		System->RequestCompile(false);
		System->MarkPackageDirty();

		UPackage* Package = System->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		const FString Filename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		const bool bSaved = UPackage::SavePackage(Package, System, *Filename, SaveArgs);
		UE_LOG(LogTemp, Log, TEXT("[JUSYNC] Saved %s: %s"), AssetPath, bSaved ? TEXT("true") : TEXT("false"));
	}
}

static FAutoConsoleCommand GJUSYNCAddStreamlineParams(
	TEXT("JUSYNC.AddStreamlineNiagaraParams"),
	TEXT("Adds the streamline renderer's data interface user parameters to NS_JusyncStreamlines and saves it."),
	FConsoleCommandDelegate::CreateStatic(&JUSYNCAddStreamlineNiagaraParameters));
#endif
