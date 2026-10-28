#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "JUSYNCTypes.h"
#include "JUSYNCBlueprintLibrary.h"
#include "JUSYNCPointCloudSpawner.h"
#include "JUSYNCMeshCache.h"
#include "JUSYNCFileChangeTracker.h"
#include "JUSYNCAnimationController.h"
#include "JUSYNCUSDLoader.h"
#include "JUSYNCPathlineManager.h"
#include "JUSYNCFileSpawnerActor.generated.h"

class UTexture2D;
class URealtimeMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UBoxComponent;
class UNiagaraComponent;
class UNiagaraSystem;
struct FJUSYNCParsedFileResult;

UENUM(BlueprintType)
enum class EJUSYNCSpawnerState : uint8
{
    Idle          UMETA(DisplayName = "Idle"),
    Connecting    UMETA(DisplayName = "Connecting"),
    FetchingList  UMETA(DisplayName = "Fetching File List"),
    Downloading   UMETA(DisplayName = "Downloading Files"),
    Spawning      UMETA(DisplayName = "Spawning Meshes"),
    Complete      UMETA(DisplayName = "Complete"),
    Error         UMETA(DisplayName = "Error")
};

UENUM(BlueprintType)
enum class EJUSYNCPointShape : uint8
{
    Square UMETA(DisplayName = "Square"),
    Circle UMETA(DisplayName = "Circle")
};

UENUM(BlueprintType)
enum class EJUSYNCPointOrientation : uint8
{
    FacingCamera UMETA(DisplayName = "Facing Camera"),
    FacingNormal UMETA(DisplayName = "Facing Normal")
};

UENUM(BlueprintType)
enum class EJUSYNCPointScaling : uint8
{
    PerNode UMETA(DisplayName = "Per Node"),
    PerNodeAdaptive UMETA(DisplayName = "Per Node Adaptive"),
    PerPoint UMETA(DisplayName = "Per Point"),
    FixedScreenSize UMETA(DisplayName = "Fixed Screen Size")
};

UENUM(BlueprintType)
enum class EJUSYNCFitMode : uint8
{
    Uniform UMETA(DisplayName = "Uniform (keep proportions)"),
    Stretch UMETA(DisplayName = "Stretch (fill each axis)")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSpawnerFileProgress, int32, CurrentFile, int32, TotalFiles);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSpawnerFileComplete, const FString&, Filename, AActor*, SpawnedActor);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSpawnerAllComplete, int32, TotalSpawned, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSpawnerError, const FString&, ErrorMessage);

UCLASS(Blueprintable, BlueprintType, Category = "JUSYNC")
class JUSYNC_API AJUSYNCFileSpawnerActor : public AActor
{
    GENERATED_BODY()

public:
    AJUSYNCFileSpawnerActor();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Connection")
    FString BrokerEndpoint;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Connection")
    int32 RequestTimeoutMs;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Connection")
    float BandwidthBytesPerSecond;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Filters")
    int32 MinimumFileSizeBytes;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Filters")
    bool bFilterUSDOnly;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Filters")
    bool bClipsOnly;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    AActor* SpawnTargetActor;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    FVector BaseSpawnLocation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    float SpawnSpacing;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement", meta = (ClampMin = "1", ClampMax = "100"))
    int32 SpawnGridColumns;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Material")
    UMaterialInterface* SpawnMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Material")
    FString TextureSampleParameterName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    FVector SpawnScale;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    bool bUseUniformScaling;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "JUSYNC|Spawner|Placement")
    TObjectPtr<UBoxComponent> FitVolume;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement")
    bool bFitToVolume;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement", meta = (EditCondition = "bFitToVolume", EditConditionHides))
    EJUSYNCFitMode FitMode;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement", meta = (EditCondition = "bFitToVolume", EditConditionHides, ClampMin = "0.0", ClampMax = "0.9"))
    float FitPaddingFraction;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Placement", meta = (EditCondition = "bFitToVolume", EditConditionHides))
    bool bCenterInVolume;

    UFUNCTION(BlueprintCallable, CallInEditor, Category = "JUSYNC|Spawner|Placement")
    void RefitSpawnedActorsToVolume();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Behavior")
    bool bAutoStart;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|LiveUpdate", meta = (DisplayName = "Enable Live Updates"))
    bool bEnableLiveUpdates;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|LiveUpdate", meta = (EditCondition = "bEnableLiveUpdates", EditConditionHides))
    float LiveUpdatePollInterval;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|LiveUpdate", meta = (EditCondition = "bEnableLiveUpdates", EditConditionHides))
    bool bAutoRefreshMeshes;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pipeline", meta = (ClampMin = "1", ClampMax = "16"))
    int32 PipelineDepth;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pipeline", meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxSpawnsPerFrame;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pipeline")
    TArray<FString> IgnoredElementPatterns;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pipeline")
    bool bCollapseElementVariants;

    UPROPERTY()
    bool bCollapseVariantsMigrated = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|LiveUpdate", meta = (ClampMin = "0.0", ClampMax = "30.0"))
    float CommitCompleteCooldownSeconds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Morph")
    bool bMorphEnabled;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Morph", meta = (ClampMin = "0", ClampMax = "30000"))
    int32 MeshBlendMs;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Morph", meta = (ClampMin = "8", ClampMax = "30000"))
    int32 MeshMaxBlendMs;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Morph", meta = (ClampMin = "0", ClampMax = "6"))
    int32 CloudSteps;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Morph", meta = (ClampMin = "0", ClampMax = "30000"))
    int32 CloudBlendMs;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Morph", meta = (ClampMin = "8", ClampMax = "30000"))
    int32 CloudMaxBlendMs;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Morph")
    bool bCloudFixedSize;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Morph", meta = (ClampMin = "0.0", ClampMax = "2.0", EditCondition = "bCloudFixedSize"))
    float CloudScreenSizePct;

    UFUNCTION(BlueprintCallable, CallInEditor, Category = "JUSYNC|Spawner|Morph")
    void ApplyMorphSettings();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Diagnostics")
    bool bEnablePerfLogging;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "0.0"))
    float PointCloudSize;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    EJUSYNCPointShape PointShape;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    EJUSYNCPointOrientation PointOrientation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    EJUSYNCPointScaling PointScaling;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "0.0", ClampMax = "0.15"))
    float PointSizeBias;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "0.0"))
    float GapFillingStrength;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "1", ClampMax = "256"))
    int32 PointCloudPoolSize;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    bool bCalculatePointCloudNormals;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "0", ClampMax = "10000000"))
    int32 PointCloudNormalsMaxPoints;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "1", ClampMax = "100"))
    int32 PointCloudNormalsQuality;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "0.0"))
    float PointCloudNormalsNoiseTolerance;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (ClampMin = "0.0", ClampMax = "30.0"))
    float PointCloudNormalsCooldownSeconds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    bool bSpawnPointClouds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud")
    FString GradientPngFilename;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|PointCloud", meta = (InlineEditConditionToggle))
    bool bUseGradientColors;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines")
    bool bEnablePathlines;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides, ClampMin = "1", ClampMax = "1048576"))
    int32 PathlineMaxTracks;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides, ClampMin = "2", ClampMax = "4096"))
    int32 PathlineHistorySamples;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides, ClampMin = "16"))
    int32 PathlineMaxNodes;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides, ClampMin = "1", ClampMax = "512"))
    int32 PathlineTrackExpirySamples;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides))
    TSoftObjectPtr<UNiagaraSystem> PathlineNiagaraSystem;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides, ClampMin = "0.001"))
    float PathlineRadius;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides, ClampMin = "0.001"))
    float PathlineRadiusScale;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides))
    bool bPathlineUsePointWidth;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides))
    FString PathlineIdAttribute;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides))
    FString PathlineScalarAttribute;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides, ClampMin = "0", ClampMax = "3"))
    int32 PathlineScalarComponent;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides))
    bool bPathlineFilterEnabled;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines && bPathlineFilterEnabled", EditConditionHides))
    float PathlineFilterMin;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines && bPathlineFilterEnabled", EditConditionHides))
    float PathlineFilterMax;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides))
    bool bPathlineHueFilterEnabled;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines && bPathlineHueFilterEnabled", EditConditionHides, ClampMin = "0", ClampMax = "360"))
    float PathlineHue;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines && bPathlineHueFilterEnabled", EditConditionHides, ClampMin = "0", ClampMax = "180"))
    float PathlineHueTolerance;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides))
    bool bPathlineColorByScalar;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines && bPathlineColorByScalar", EditConditionHides))
    float PathlineColorMin;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines && bPathlineColorByScalar", EditConditionHides))
    float PathlineColorMax;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines && !bPathlineColorByScalar", EditConditionHides))
    FLinearColor PathlineFlatColor;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (ClampMin = "0", ClampMax = "4", EditCondition = "bEnablePathlines", EditConditionHides))
    float PathlineEmissiveStrength;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Points")
    bool bPointCloudsVisible;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Points")
    bool bPointCloudHueFilterEnabled;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Points",
        meta = (EditCondition = "bPointCloudHueFilterEnabled", EditConditionHides, ClampMin = "0", ClampMax = "360"))
    float PointCloudHue;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Points",
        meta = (EditCondition = "bPointCloudHueFilterEnabled", EditConditionHides, ClampMin = "0", ClampMax = "180"))
    float PointCloudHueTolerance;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides, ClampMin = "0"))
    int32 PathlineSelectionSeed;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides, ClampMin = "0.0"))
    float PathlinePendingTimeoutSeconds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Pathlines",
        meta = (EditCondition = "bEnablePathlines", EditConditionHides))
    UMaterialInterface* PathlineMaterial;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "JUSYNC|Spawner|Pathlines")
    int32 PathlineActiveTracks;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "JUSYNC|Spawner|Pathlines")
    int64 PathlineNodeCount;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "JUSYNC|Spawner|Pathlines")
    int32 PathlineVisibleTracks;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "JUSYNC|Spawner|Pathlines")
    int32 PathlineDroppedSamples;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "JUSYNC|Spawner|Pathlines")
    int64 PathlineIgnoredSamples;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Cache")
    bool bEnableMeshCache;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Cache", meta = (EditCondition = "bEnableMeshCache", EditConditionHides, ClampMin = "1", ClampMax = "4096"))
    int32 MeshCacheMaxEntries;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Cache", meta = (EditCondition = "bEnableMeshCache", EditConditionHides, ClampMin = "16"))
    int32 MeshCacheMaxMB;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Cache", meta = (EditCondition = "bEnableMeshCache", EditConditionHides))
    bool bCachePointClouds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Cache")
    bool bDynamicCacheBudget = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Cache", meta = (EditCondition = "bDynamicCacheBudget", EditConditionHides, ClampMin = "10", ClampMax = "90"))
    int32 CacheRamPercent = 70;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Cache", meta = (EditCondition = "bDynamicCacheBudget", EditConditionHides, ClampMin = "64"))
    int32 CacheBudgetMaxMB = 8192;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Cache", meta = (EditCondition = "bDynamicCacheBudget", EditConditionHides, ClampMin = "10", ClampMax = "90"))
    int32 CacheVRamPercent = 70;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Playback")
    bool bEnableSampleCache;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Playback", meta = (EditCondition = "bEnableSampleCache", EditConditionHides, ClampMin = "1", ClampMax = "64"))
    int32 SampleCacheSize;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Playback", meta = (EditCondition = "bEnableSampleCache", EditConditionHides, ClampMin = "16"))
    int32 SampleCacheMaxMB;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Playback", meta = (EditCondition = "bEnableSampleCache", EditConditionHides))
    bool bPauseLiveApplyDuringPlayback;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Playback", meta = (EditCondition = "bEnableSampleCache", EditConditionHides, UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
    float PlaybackPosition;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "JUSYNC|Spawner|Playback")
    int32 CachedSampleCount;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "JUSYNC|Spawner|Playback")
    int32 PlaybackSampleIndex;

    UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "JUSYNC|Spawner|Playback")
    bool bSamplePlaybackActive;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Animation")
    bool bEnableTimeStepAnimation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Animation", meta = (EditCondition = "bEnableTimeStepAnimation", EditConditionHides, ClampMin = "1.0", ClampMax = "240.0"))
    float TimeStepPlaybackFPS;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Animation", meta = (EditCondition = "bEnableTimeStepAnimation", EditConditionHides))
    bool bLoopTimeStepAnimation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Benchmark")
    bool bEnableBenchmarking;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Benchmark", meta = (EditCondition = "bEnableBenchmarking", EditConditionHides))
    FString BenchmarkTestName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Benchmark", meta = (EditCondition = "bEnableBenchmarking", EditConditionHides))
    FString BenchmarkOutputDirectory;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Benchmark", meta = (EditCondition = "bEnableBenchmarking", EditConditionHides))
    int32 BenchmarkOutputFormat;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Benchmark", meta = (EditCondition = "bEnableBenchmarking", EditConditionHides))
    bool bBenchmarkAppendTimestamp;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Benchmark", meta = (EditCondition = "bEnableBenchmarking", EditConditionHides, ClampMin = "1.0", ClampMax = "300.0"))
    float BenchmarkInteractiveWindowSeconds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Benchmark", meta = (EditCondition = "bEnableBenchmarking", EditConditionHides))
    bool bBenchmarkColdStart;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Benchmark", meta = (EditCondition = "bEnableBenchmarking", EditConditionHides))
    bool bExportClientBenchmarkReport;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JUSYNC|Spawner|Benchmark", meta = (EditCondition = "bEnableBenchmarking", EditConditionHides))
    bool bIncludeClusterBenchmark;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Spawner|State")
    EJUSYNCSpawnerState CurrentState;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Spawner|State")
    int32 FilesDownloaded;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Spawner|State")
    int32 FilesTotal;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Spawner|State")
    int32 ActorsSpawned;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Spawner|State")
    TArray<AActor*> SpawnedActors;

    UPROPERTY(BlueprintAssignable, BlueprintCallable, Category = "JUSYNC|Spawner|Events")
    FOnSpawnerFileProgress OnFileProgress;

    UPROPERTY(BlueprintAssignable, BlueprintCallable, Category = "JUSYNC|Spawner|Events")
    FOnSpawnerFileComplete OnFileComplete;

    UPROPERTY(BlueprintAssignable, BlueprintCallable, Category = "JUSYNC|Spawner|Events")
    FOnSpawnerAllComplete OnAllComplete;

    UPROPERTY(BlueprintAssignable, BlueprintCallable, Category = "JUSYNC|Spawner|Events")
    FOnSpawnerError OnError;

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner")
    void StartSpawning();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner")
    void CancelSpawning();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner")
    void ClearSpawnedActors();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|LiveUpdate", meta = (DisplayName = "Manual Refresh"))
    void ManualRefresh();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Animation")
    void PlayTimeStepAnimation();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Animation")
    void StopTimeStepAnimation();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Animation")
    void SetTimeStepIndex(int32 NewIndex);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Playback")
    void SetPlaybackPosition(float NewPosition);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Playback")
    void PlaybackPreviousSample();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Playback")
    void PlaybackNextSample();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Playback")
    void FollowLatestSample();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Playback")
    void ClearCachedSamples();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Points")
    void SetPointCloudsVisible(bool bVisible);

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Points")
    void UpdatePointCloudHueFilter();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Points")
    void ApplyPointCloudDisplaySettings(bool bRecalculateNormals = false);

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Spawner|Playback")
    float GetPlaybackPosition() const { return PlaybackPosition; }

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Spawner|Playback")
    int32 GetCachedSampleCount() const { return CachedSampleCount; }

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Spawner|Playback")
    int32 GetPlaybackSampleIndex() const { return PlaybackSampleIndex; }

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Spawner|Playback")
    double GetCachedSampleTime(int32 Index) const;

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Spawner|Playback")
    bool IsPlayingCachedSample() const { return bSamplePlaybackActive; }

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Playback")
    bool ConsumeSenderLoopDetected();

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Playback")
    bool ConsumeSenderLoopDetectedIfRecent(float MaxAgeSeconds);

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Spawner|Playback")
    bool HasSenderLoopDetected() const { return bSenderLoopDetected; }

    UFUNCTION(BlueprintCallable, Category = "JUSYNC|Spawner|Playback")
    FString GetSampleCacheDebugString() const;

    UFUNCTION(BlueprintPure, Category = "JUSYNC|Spawner")
    FVector GetNextSpawnLocation() const;

    static FString BuildClipStreamKey(const FString& Filename);

    static int64 ComputeCacheBudgetBytes(int64 FreeBytes, int32 Percent, int64 FloorBytes, int64 CapBytes);

    static bool PassesBudgetHysteresis(int64 LastAccepted, int64 Value, int32 PercentToAct = 10);

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaSeconds) override;
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    void ConnectToBroker();
    void RequestFileList();
    void ProcessAndDownloadFiles();

    void OnFileListReceived_Internal(const TArray<FString>& FileList, const TArray<int64>& FileSizes, const TArray<int32>& FileRanks, bool bSuccess, const TArray<uint64>& HashLo, const TArray<uint64>& HashHi);
    void OnFileListError(const FString& ErrorMessage);

    void DownloadGradientPng(UJUSYNCSubsystem* Subsystem);
    void LoadMeshTextureAsync(UJUSYNCSubsystem* Subsystem);
    void MaybeTriggerGradientLoad(const TArray<FString>& Files, const TArray<int64>& Sizes, const TArray<int32>& Ranks, UJUSYNCSubsystem* Subsystem);
    void ApplySenderTextureToActor(AActor* Actor);
    void ApplySenderTextureToComponent(URealtimeMeshComponent* Comp, UTexture2D* SenderTex);
    UMaterialInstanceDynamic* GetOrCreateSenderMID(URealtimeMeshComponent* Comp, UTexture2D* SenderTex);
    void PipelineDownloadNext(UJUSYNCSubsystem* Subsystem);
    void ApplyPointCloudSettingsToSpawner(FJUSYNCPointCloudSpawner* Spawner);

    void MaybeStartBenchmarkSession();
    void ScheduleBenchmarkInteractiveWindow();
    void FinalizeBenchmarkSession();
    void ApplyClientReportCounters(const FString& OutDir, FJUSYNCBenchmarkResult& Result);
    void FetchClusterBenchmarkReport(FJUSYNCBenchmarkResult& Result);

    int32 CalculateDynamicTimeout(int64 FileSizeBytes) const;
    void LoadFileThroughPipeline(const FString& Filename, int32 Rank, int64 Size, uint64 HashLo, uint64 HashHi, int32 FileIndex, bool bIsInitial, uint64 SampleId = 0);
    void ApplyParsedFileData(FJUSYNCParsedFileResult&& Result);
    AActor* SpawnOrUpdateMesh(FJUSYNCCompactMeshRef& Mesh, const FString& Filename, bool bIsRefresh, TArray<AActor*>& ReplacementActors, RealtimeMesh::FRealtimeMeshStreamSet* PrebuiltStreams = nullptr, const TArray<AActor*>* OldMeshActors = nullptr, bool bForceGradientSpawn = false, int32 Rank = 0, bool bForceApplyHidden = false);
    void DestroyUnreplacedMeshActors(const FString& Filename, const TArray<AActor*>& OldMeshActors, const TArray<AActor*>& ReplacementActors);
    void DestroyUnreplacedPCActors(const FString& Filename, const TArray<AActor*>& OldPCActors, const TSet<FString>& NewPCKeys);

    FVector ComputeFitScale(const FBox& MeshBox) const;
    FVector ComputeFitLocation(const FBox& MeshBox, const FVector& Scale) const;

    FBox FitUnionBounds;
    bool bHasFitUnionBounds = false;
    bool bFitLocked = false;
    bool bFitUnionGrew = false;
    void AccumulateFitBounds(const FBox& B);

    TMap<TWeakObjectPtr<AActor>, FBox> LastMeshLocalBounds;

    bool BakeLUTVertexColorCompact(FJUSYNCCompactMeshRef& Mesh, UMaterialInterface*& OutVertexMaterial);
    void RegisterMeshForLUTRecolor(AActor* Spawned, FJUSYNCCompactMeshRef& Mesh, const FString& Filename, const FVector& Loc);
    void RecolorGradientPendingMeshes();
    void ProcessGradientDeferredMeshes(bool bForce = false);
    void ProcessGradientDeferredMeshesTimer();
    void RebuildGradientMaterial();
    void FlushHiddenMeshUpdates();

    void CheckAllDownloadsComplete();
    void RetryFailedDownloads();
    void OnPointCloudSpawnedHandler(const FString& EleName, AActor* Spawned);

    UFUNCTION()
    void OnBrokerNotification(const FJUSYNCNotification& Notification);
    void HandleFileUpdateNotification(const FString& Filename, int32_t SourceRank, int64 FileSize, uint64 HashLo, uint64 HashHi, uint64 SampleId = 0);
    void HandleCommitCompleteNotification(bool bIsTimer = false);
    void StartLiveUpdatePolling();
    void StopLiveUpdatePolling();
    void OnLiveUpdateTimer();
    void DiffAndRefreshFileList(const TArray<FString>& NewFiles, const TArray<int64>& NewSizes, const TArray<int32>& NewRanks, bool bIsManual);

    void FilterFileList(const TArray<FString>& InFiles, const TArray<int64>& InSizes, const TArray<int32>& InRanks,
        const TArray<uint64>& InHashLo, const TArray<uint64>& InHashHi,
        TArray<FString>& OutFiles, TArray<int64>& OutSizes, TArray<int32>& OutRanks,
        TArray<uint64>& OutHashLo, TArray<uint64>& OutHashHi,
        TArray<FString>& OutPngFiles, TArray<int64>& OutPngSizes, TArray<int32>& OutPngRanks) const;
    bool QueueRefreshFile(const FString& Filename, int32 Rank, int64 Size, uint64 HashLo, uint64 HashHi, uint64 SampleId = 0);
    void UpdateTrackedFileMetadata(const FString& Filename, int32 Rank, int64 Size, uint64 HashLo, uint64 HashHi);

    int32 ResolveFileRank(const FString& Filename, int32 KnownRank) const;
    int32 DestroyFileActors(const FString& Filename);

    void TransferFileActorOwnership(const FString& FromFile, const FString& ToFile);

    void SweepPendingDeletedFiles();

    void DeduplicateLogicalActors();

    TMap<FString, TPair<int32, FString>> ElementVariantWinners;
    TSet<FString> IgnoredVariantFiles;

    int32 CollapseElementVariants(TArray<FString>& Files, TArray<int64>& Sizes,
        TArray<int32>& Ranks, TArray<uint64>& HashLo, TArray<uint64>& HashHi);

    bool IsSuppressedElementVariant(const FString& Filename);

    void ChainRefreshNext();
    void RetryRemainingFiles();
    void ProcessDeferredSpawns();

    TArray<FString> RawFileList;
    TArray<int64> RawFileSizes;
    TArray<int32> RawFileRanks;
    TArray<uint64> RawHashLo;
    TArray<uint64> RawHashHi;

    TArray<FString> FilteredFiles;
    TArray<int64> FilteredSizes;
    TArray<int32> FilteredRanks;
    TArray<uint64> FilteredHashLo;
    TArray<uint64> FilteredHashHi;
    struct FJUSYNCFileCandidateMeta
    {
        int32 Rank = 0;
        int64 Size = 0;
    };
    TMap<FString, FJUSYNCFileCandidateMeta> GradientPngRankMap;
    TMap<int32, TArray<FColor>> RankGradients;
    std::atomic<bool> bGradientReady;
    std::atomic<bool> bGradientDownloadStarted;
    bool bGradientAttempted;
    TWeakObjectPtr<UTexture2D> GradientLUTTexture;
    TWeakObjectPtr<UMaterialInterface> GradientMaterial;
    FName GradientTextureParameterName;

    struct FRecolorMeshEntry
    {
        FJUSYNCCompactMeshRef Mesh;
        FString Filename;
        int32 Rank = 0;
        FVector Loc = FVector::ZeroVector;
        FRotator Rot = FRotator::ZeroRotator;
        FVector Scale = FVector::OneVector;
    };
    TMap<AActor*, FRecolorMeshEntry> GradientPendingMeshes;
    TMap<AActor*, FRecolorMeshEntry> HiddenPendingMeshUpdates;

    TMap<FString, TWeakObjectPtr<UTexture2D>> MeshTextureCache;
    TWeakObjectPtr<UTexture2D> ActiveMeshTexture;
    std::atomic<bool> bMeshTextureLoading;
    bool bMeshTextureReady;
    TMap<URealtimeMeshComponent*, TWeakObjectPtr<UMaterialInstanceDynamic>> SenderMIDCache;

    int32 NextSpawnIndex;
    int32 PendingDownloads;
    int32 PendingAsyncSpawns;
    int32 PendingAsyncPCS;
    int32 PendingParseTasks;
    int32 PipelineNextIndex;
    int32 PipelineActive;
    double DownloadStallSince;
    float DownloadWatchdogAcc;
    bool bIsCancelled;
    int32 MaxRetries;
    int32 CurrentRetryCount;
    TArray<int32> FailedFileIndices;
    TArray<int32> ParseFailedIndices;

    FTimerHandle LiveUpdateTimerHandle;
    float LiveUpdatePollAccumulator = 0.0f;

    FTimerHandle InitialFileListRetryTimerHandle;
    int32 InitialFileListRetryCount = 0;
    bool bInitialKickInFlight = false;
    TMap<FString, AActor*> FileToActorMap;
    TMap<FString, TArray<AActor*>> FilenameToActors;
    TMap<FString, TArray<AActor*>> FilenameToPCActors;
    bool bPathlineMaterialFallbackTried = false;
    UPROPERTY(Transient)
    TObjectPtr<class UMaterialExpressionMultiply> TrailEmissiveExpression;
    UPROPERTY(Transient)
    TObjectPtr<class UMaterialParameterCollection> PathlineGlowCollection;
    float LastPathlineGlow = -1.0f;
    bool bPathlineGlowMissingWarned = false;
    TMap<FString, float> PendingDeletedFiles;
    TMap<FString, FString> PCElementToFilename;

    TMap<FString, uint64> FileHashLo;
    TMap<FString, uint64> FileHashHi;
    TMap<FString, int64> FileLastSize;
    TMap<FString, int64> AppliedFileSizes;
    double LastCommitCompleteTime;
    bool bSceneDiffInFlight;
    bool bCommitDiffPending = false;
    bool bInitialSpawnDone;
    double InitialSpawnPendingSince = 0.0;
    bool bInCheckAllDownloads = false;

    bool bBenchmarkSessionActive = false;
    bool bBenchmarkWindowScheduled = false;
    bool bBenchmarkFinalized = false;
    double BenchmarkSessionWallStart = 0.0;
    int64 BenchmarkRAMStartBytes = 0;
    int64 BenchmarkVRAMStartBytes = 0;
    int64 BenchmarkSceneVertices = 0;
    int64 BenchmarkSceneTriangles = 0;
    FTimerHandle BenchmarkFinalizeTimerHandle;

    TMap<FString, int32> SeenV2Files;
    TMap<FString, FString> LogicalTrackedFile;
    FTimerHandle SpawnThrottleTimer;

    TUniquePtr<FJUSYNCMeshCache> MeshCache;
    TUniquePtr<FJUSYNCFileChangeTracker> ChangeTracker;
    TUniquePtr<FJUSYNCAnimationController> AnimationController;

    TMap<FString, uint64> ActiveDownloadAborts;

    struct FJUSYNCSampleCacheEntry
    {
        FJUSYNCCompactMeshRef Mesh;
        FJUSYNCPointCloudRef PointCloud;
        FString Filename;
        int32 Rank = 0;
        int64 Bytes = 0;
        uint64 Serial = 0;
        uint64 SampleId = 0;
        double SampleTime = -1.0;
        uint64 HashLo = 0;
        uint64 HashHi = 0;
    };
    TMap<FString, TArray<FJUSYNCSampleCacheEntry>> SampleCache;
    TMap<FString, FJUSYNCSampleCacheEntry> PendingLiveSamples;
    TMap<FString, FString> SampleCacheLiveFilename;
    int64 SampleCacheBytes = 0;
    int64 SampleCacheEntryCount = 0;
    uint64 NextSampleCacheSerial = 0;
    uint64 NextPlaybackSampleId = 0;
    TMap<uint64, double> SampleTimeById;

    double NewestCachedSampleTime = -1.0;
    bool bSenderLoopDetected = false;
    double SenderLoopDetectedSeconds = 0.0;

    void ConsiderSenderLoop(const FString& Filename, uint64 SenderTimestamp);

    TSharedPtr<FJUSYNCPathlineManager> PathlineManager;

    UPROPERTY(Transient)
    TMap<FString, TObjectPtr<UNiagaraComponent>> PathlineComponents;

    bool bPathlineSystemWarned = false;

    uint64 LiveSampleId = 0;
    uint64 LastClosedLiveSampleId = 0;
    bool bLiveSampleFrameClosed = true;
    uint64 PlaybackAnchorSampleId = 0;
    bool bApplyingCachedSample = false;

    uint64 ResolveLiveSampleId(uint64 SenderTimestamp);
    uint64 CacheParsedSample(const FJUSYNCParsedFileResult& Result, bool bIsInitial);
    void IngestPathlineSample(const FJUSYNCParsedFileResult& Result, uint64 EffectiveSampleId);
    void RebuildPathlinesFromSampleCache();
    FJUSYNCPathlineSettings BuildPathlineSettings() const;
    void SyncPathlineManager();

    void SyncPathlineComponents();

    void EnsurePathlineGlow();

    void DestroyPathlineComponent(const FString& ElementName);

    void DestroyAllPathlineComponents();

    UMaterialInterface* CreateEmissiveVertexColorMaterial();
    void AddSampleCacheEntry(const FString& Key, FJUSYNCSampleCacheEntry&& Entry);
    void TrimSampleCache();

    void PollDynamicCacheBudgets(float DeltaSeconds);

    int64 GetSampleCacheBudgetBytes() const;

    int32 GetSampleCacheCountCap() const;

    float CacheBudgetAccum = 0.0f;
    int64 DynamicSampleCacheBytes = 0;  // 0 = not sampled yet
    int64 DynamicCloudBudgetBytes = 0;  // 0 = not sampled yet
    void ClearSampleCache();
    void FlushPendingLiveSamples();
    void ApplyCachedSample(float NormalizedPosition);
    int32 GetMaxSampleCacheCount() const;
    void GetSampleTimeline(TArray<uint64>& OutSampleIds) const;
    double LookupSampleTime(uint64 InSampleId) const;
    void RefreshPlaybackWindow();
    const FJUSYNCSampleCacheEntry* FindSampleForPlayback(
        const TArray<FJUSYNCSampleCacheEntry>& List, uint64 SelectedSampleId) const;
    void HideUnselectedPlaybackActors(const TSet<AActor*>& KeptActors);

    void AbortAllInFlightDownloads();

    struct FDeferredSpawnEntry
    {
        TArray<FJUSYNCCompactMeshRef> Meshes;
        FString Filename;
        int32 Rank = 0;
        TArray<AActor*> OldActors;
        TArray<AActor*> ReplacementActors;
        int32 ExpectedNewMeshes = 0;
        int32 SpawnedNewMeshes = 0;
        uint64 Generation = 0;
        TArray<TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet>> PrebuiltStreams;
    };
    TArray<FDeferredSpawnEntry> DeferredSpawns;

    struct FJUSYNCGradientDeferredMeshEntry
    {
        FJUSYNCCompactMeshRef Mesh;
        FString Filename;
        int32 Rank = 0;
        bool bIsRefresh = false;
        TArray<AActor*> OldActors;
        int32 ExpectedNewMeshes = 1;
        int32 SpawnedNewMeshes = 0;
        uint64 Generation = 0;
        double DeferredTime = 0.0;
        TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet> PrebuiltStream;
    };
    TArray<FJUSYNCGradientDeferredMeshEntry> GradientDeferredMeshes;
    TMap<FString, TArray<AActor*>> GradientDeferredReplacementActors;
    FTimerHandle GradientDeferredTimerHandle;
    float GradientDeferredTimeoutSeconds = 3.0f;
};
