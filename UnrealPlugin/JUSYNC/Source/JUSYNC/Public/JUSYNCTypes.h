#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "JUSYNCTypes.generated.h"

// Forward declarations
class UProceduralMeshComponent;
class URealtimeMeshComponent;

USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCFileData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FString Filename;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	TArray<uint8> Data;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FString Hash;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FString FileType;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	int32 SourceRank;

	FJUSYNCFileData()
	{
		Filename = TEXT("");
		Hash = TEXT("");
		FileType = TEXT("");
		SourceRank = -1;
	}

	bool IsValid() const
	{
		return !Filename.IsEmpty() &&
			Data.Num() > 0 &&
			!Hash.IsEmpty() &&
			!FileType.IsEmpty();
	}
};

USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCMeshData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FString ElementName;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FString TypeName;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	TArray<FVector> Vertices;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	TArray<int32> Triangles;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	TArray<FVector> Normals;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	TArray<FVector2D> UVs;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	TArray<FColor> VertexColors;  // ADD THIS LINE

	// Update validation and helper functions
	bool HasVertexColors() const {
		return VertexColors.Num() > 0;
	}

	FJUSYNCMeshData()
	{
		ElementName = TEXT("");
		TypeName = TEXT("");
	}

	bool IsValid() const
	{
		return !ElementName.IsEmpty() &&
			Vertices.Num() > 0 &&
			Triangles.Num() > 0 &&
			(Triangles.Num() % 3 == 0);
	}

	int32 GetVertexCount() const { return Vertices.Num(); }
	int32 GetTriangleCount() const { return Triangles.Num() / 3; }

	bool HasNormals() const { return Normals.Num() > 0; }
	bool HasUVs() const { return UVs.Num() > 0; }
};

// RealtimeMesh-specific vertex structure
USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCRealtimeMeshVertex
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FVector Position;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FVector Normal;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FVector2D UV;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FColor Color;

	FJUSYNCRealtimeMeshVertex()
	{
		Position = FVector::ZeroVector;
		Normal = FVector::UpVector;
		UV = FVector2D::ZeroVector;
		Color = FColor::White;
	}
};

// RealtimeMesh-optimized data structure
USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCRealtimeMeshData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FString ElementName;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	TArray<FJUSYNCRealtimeMeshVertex> Vertices;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	TArray<int32> Triangles;

	FJUSYNCRealtimeMeshData()
	{
		ElementName = TEXT("");
	}

	bool IsValid() const
	{
		return !ElementName.IsEmpty() &&
			Vertices.Num() > 0 &&
			Triangles.Num() > 0 &&
			(Triangles.Num() % 3 == 0);
	}

	// Conversion methods
	static FJUSYNCRealtimeMeshData FromStandardMesh(const FJUSYNCMeshData& StandardMesh);
	FJUSYNCMeshData ToStandardMesh() const;
};

// Point cloud data for USD GeomPoints primitives
USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCPointCloudData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FString ElementName;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FString TypeName;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	TArray<FVector> Positions;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	TArray<FColor> Colors;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	TArray<float> Widths;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FVector BoundingBoxMin;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	FVector BoundingBoxMax;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	int32 PointCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	bool bHasColors = false;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	bool bHasNormals = false;

	FJUSYNCPointCloudData()
	{
		ElementName = TEXT("");
		TypeName = TEXT("GeomPoints");
		BoundingBoxMin = FVector::ZeroVector;
		BoundingBoxMax = FVector::ZeroVector;
	}

	bool IsValid() const
	{
		return !ElementName.IsEmpty() && PointCount > 0 && Positions.Num() > 0;
	}

	bool HasColors() const { return bHasColors && Colors.Num() > 0; }
	bool HasNormals() const { return bHasNormals; }
	bool HasWidths() const { return Widths.Num() > 0; }

	int32 GetPointCount() const { return PointCount; }
};

USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCTextureData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	int32 Width;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	int32 Height;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	int32 Channels;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
	TArray<uint8> Data;

	FJUSYNCTextureData()
	{
		Width = 0;
		Height = 0;
		Channels = 0;
	}

	bool IsValid() const
	{
		return Width > 0 && Height > 0 && Channels > 0 &&
			Data.Num() == (Width * Height * Channels);
	}

	int32 GetExpectedDataSize() const
	{
		return Width * Height * Channels;
	}
};

USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCWorkerStatus
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Worker")
	int32 Rank;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Worker")
	int32 Status;  // 0=offline, 1=idle, 2=busy, 3=error

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Worker")
	FString Hostname;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Worker")
	FString GpuInfo;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Worker")
	int64 LastHeartbeat;  // Unix timestamp

	FJUSYNCWorkerStatus()
	{
		Rank = -1;
		Status = 0;
		Hostname = TEXT("");
		GpuInfo = TEXT("");
		LastHeartbeat = 0;
	}

	bool IsOnline() const { return Status > 0; }
	bool IsIdle() const { return Status == 1; }
	bool IsBusy() const { return Status == 2; }
	bool HasError() const { return Status == 3; }

	FString GetStatusString() const
	{
		switch (Status)
		{
		case 0: return TEXT("Offline");
		case 1: return TEXT("Idle");
		case 2: return TEXT("Busy");
		case 3: return TEXT("Error");
		default: return TEXT("Unknown");
		}
	}
};

UENUM(BlueprintType)
enum class EJUSYNCExtension : uint8
{
	USD      UMETA(DisplayName = ".usda / .usd"),
	PNG      UMETA(DisplayName = ".png"),
	JSON     UMETA(DisplayName = ".json"),
	TXT      UMETA(DisplayName = ".txt"),
	BIN      UMETA(DisplayName = ".bin"),
	ALL      UMETA(DisplayName = "All Files")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJUSYNCFileReceived, const FJUSYNCFileData&, FileData);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJUSYNCMessageReceived, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJUSYNCProcessingProgress, float, Progress, const FString&, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJUSYNCError, const FString&, ErrorType, const FString&, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJUSYNCWorkerStatusReceived, const TArray<FJUSYNCWorkerStatus>&, WorkerStatus);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJUSYNCWorkerCountReceived, int32, WorkerCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJUSYNCPointCloudReceived, const TArray<FJUSYNCPointCloudData>&, PointCloudData);

// ========== BENCHMARKING STRUCTURES ==========

/**
 * Benchmark result for a single operation
 */
USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCBenchmarkResult
{
	GENERATED_BODY()

	// Test identification
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	FString TestName;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	FDateTime Timestamp;

	// Timing metrics (ms)
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	float TotalTimeMs;

	// Complexity metrics
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int32 TriangleCount;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int32 VertexCount;

	// Memory metrics (bytes)
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int64 RAMBeforeBytes;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int64 RAMAfterBytes;

	// Performance metrics
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	float FPS;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	float FrameTimeMs;

	// Additional metrics
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int32 ActorCount;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int32 ErrorCount;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	float SuccessRate;

	// Splitting metrics (for large meshes that exceed RMC limits)
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int32 SplitMeshCount;

	// CPU Usage metrics (percentage)
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	float CPUUsagePercent;

	// GPU/VRAM metrics (bytes)
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int64 VRAMBeforeBytes;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int64 VRAMAfterBytes;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int64 VRAMPeakBytes;

	// Detailed RAM metrics
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int64 RAMPeakBytes;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int64 RAMDuringBytes;

	// Thread metrics
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int32 ActiveThreadCount;

	// GPU utilization (percentage)
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	float GPUUsagePercent;

	// Hitch detection metrics (frame time spikes)
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int32 HitchCount;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	float AvgHitchDurationMs;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	float MaxHitchDurationMs;

	FJUSYNCBenchmarkResult()
	{
		TestName = TEXT("");
		Timestamp = FDateTime::Now();
		TotalTimeMs = 0.0f;
		TriangleCount = 0;
		VertexCount = 0;
		RAMBeforeBytes = 0;
		RAMAfterBytes = 0;
		RAMPeakBytes = 0;
		RAMDuringBytes = 0;
		FPS = 0.0f;
		FrameTimeMs = 0.0f;
		ActorCount = 0;
		ErrorCount = 0;
		SuccessRate = 0.0f;
		SplitMeshCount = 0;
		CPUUsagePercent = 0.0f;
		VRAMBeforeBytes = 0;
		VRAMAfterBytes = 0;
		VRAMPeakBytes = 0;
		ActiveThreadCount = 0;
		GPUUsagePercent = 0.0f;
		HitchCount = 0;
		AvgHitchDurationMs = 0.0f;
		MaxHitchDurationMs = 0.0f;
	}
};

/**
 * Benchmark configuration
 */
USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCBenchmarkConfig
{
	GENERATED_BODY()

	// Enable/disable benchmarking
	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Benchmark")
	bool bEnableBenchmarking = false;

	// Output directory for benchmark files
	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Benchmark")
	FString OutputDirectory;

	// Append timestamp to filename
	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Benchmark")
	bool bAppendTimestamp = true;

	// Output format (0 = CSV, 1 = JSON, 2 = Both)
	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Benchmark")
	int32 OutputFormat = 0;

	FJUSYNCBenchmarkConfig()
	{
		bEnableBenchmarking = false;
		OutputDirectory = TEXT("");
		bAppendTimestamp = true;
		OutputFormat = 0; // Default to CSV for backward compatibility
	}
};

// ========== PERFORMANCE METRICS STRUCTURES ==========

/**
 * Comprehensive performance metrics for real-time monitoring
 */
USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCMetricsData
{
	GENERATED_BODY()

	// Timestamp
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics")
	FDateTime Timestamp;

	// ===== MEMORY USAGE METRICS =====
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Memory")
	float SystemRAM_Used_GB;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Memory")
	float SystemRAM_Total_GB;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Memory")
	float VRAM_Used_GB;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Memory")
	float VRAM_Total_GB;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Memory")
	float MeshData_GB;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Memory")
	float TextureData_GB;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Memory")
	float Cache_GB;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Memory")
	float AvailableRAM_GB;

	// ===== MESH SPLITTING METRICS =====
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Splitting")
	int32 TotalMeshesProcessed;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Splitting")
	int32 SplitMeshes;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Splitting")
	float AverageSplitTime_ms;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Splitting")
	int32 LargestMesh_Vertices;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Splitting")
	int32 SmallestMesh_Vertices;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Splitting")
	int32 TotalChunksCreated;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Splitting")
	float AverageChunksPerSplit;

	// ===== PERFORMANCE TIMING METRICS =====
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Performance")
	float FrameTime_ms;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Performance")
	float FPS;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Performance")
	float MeshProcessingTime_ms;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Performance")
	float SplittingAlgorithmTime_ms;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Performance")
	float MemoryAllocationTime_ms;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Performance")
	float GPUUploadTime_ms;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Performance")
	float AsyncTasksTime_ms;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Performance")
	float IdleTime_ms;

	// ===== COMPONENT METRICS =====
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Components")
	int32 TotalRMCComponents;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Components")
	int32 ActiveComponents;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Components")
	int32 CulledComponents;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Components")
	int32 InstancedComponents;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Components")
	int32 DrawCalls;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Components")
	int32 TrianglesRendered_Millions;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Components")
	int32 VerticesRendered_Millions;

	// ===== STREAMING METRICS =====
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Streaming")
	float StreamingBandwidth_MBps;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Streaming")
	int32 QueueLength;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Streaming")
	float AverageLoadTime_ms;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Streaming")
	float CacheHitRate_Percent;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Streaming")
	int32 PrefetchedMeshes;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Streaming")
	int32 EvictedMeshes;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Streaming")
	FString MemoryPressure;

	// ===== QUALITY METRICS =====
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Quality")
	int32 CurrentLODLevel;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Quality")
	int32 MaxLODLevel;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Quality")
	int32 TargetFPS;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Quality")
	float ActualFPS;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Quality")
	float QualityReduction_Percent;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Quality")
	FString TextureResolution;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Quality")
	float MeshDecimation_Percent;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Quality")
	float VisualFidelity_Percent;

	// ===== ERROR METRICS =====
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Errors")
	int32 TotalErrors;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Errors")
	int32 MemoryWarnings;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Errors")
	int32 SplitFailures;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Errors")
	int32 RecoveryAttempts;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Errors")
	float SuccessRate_Percent;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Errors")
	FString LastErrorTime;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Errors")
	FString WarningLevel;

	// ===== HARDWARE METRICS =====
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Hardware")
	float CPUUsage_Percent;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Hardware")
	float GPUUsage_Percent;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Hardware")
	int32 ActiveCPUThreads;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Hardware")
	int32 TotalCPUThreads;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Hardware")
	float GPUMemoryBandwidth_GBps;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Hardware")
	float PCIeBandwidth_GBps;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Hardware")
	float CPUTemperature_C;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Metrics|Hardware")
	float GPUTemperature_C;

	// Constructor
	FJUSYNCMetricsData()
	{
		Timestamp = FDateTime::Now();

		// Initialize all metrics to zero/default values
		SystemRAM_Used_GB = 0.0f;
		SystemRAM_Total_GB = 0.0f;
		VRAM_Used_GB = 0.0f;
		VRAM_Total_GB = 0.0f;
		MeshData_GB = 0.0f;
		TextureData_GB = 0.0f;
		Cache_GB = 0.0f;
		AvailableRAM_GB = 0.0f;

		TotalMeshesProcessed = 0;
		SplitMeshes = 0;
		AverageSplitTime_ms = 0.0f;
		LargestMesh_Vertices = 0;
		SmallestMesh_Vertices = 0;
		TotalChunksCreated = 0;
		AverageChunksPerSplit = 0.0f;

		FrameTime_ms = 0.0f;
		FPS = 0.0f;
		MeshProcessingTime_ms = 0.0f;
		SplittingAlgorithmTime_ms = 0.0f;
		MemoryAllocationTime_ms = 0.0f;
		GPUUploadTime_ms = 0.0f;
		AsyncTasksTime_ms = 0.0f;
		IdleTime_ms = 0.0f;

		TotalRMCComponents = 0;
		ActiveComponents = 0;
		CulledComponents = 0;
		InstancedComponents = 0;
		DrawCalls = 0;
		TrianglesRendered_Millions = 0;
		VerticesRendered_Millions = 0;

		StreamingBandwidth_MBps = 0.0f;
		QueueLength = 0;
		AverageLoadTime_ms = 0.0f;
		CacheHitRate_Percent = 0.0f;
		PrefetchedMeshes = 0;
		EvictedMeshes = 0;
		MemoryPressure = TEXT("Low");

		CurrentLODLevel = 0;
		MaxLODLevel = 0;
		TargetFPS = 60;
		ActualFPS = 0.0f;
		QualityReduction_Percent = 0.0f;
		TextureResolution = TEXT("N/A");
		MeshDecimation_Percent = 0.0f;
		VisualFidelity_Percent = 100.0f;

		TotalErrors = 0;
		MemoryWarnings = 0;
		SplitFailures = 0;
		RecoveryAttempts = 0;
		SuccessRate_Percent = 100.0f;
		LastErrorTime = TEXT("Never");
		WarningLevel = TEXT("Low");

		CPUUsage_Percent = 0.0f;
		GPUUsage_Percent = 0.0f;
		ActiveCPUThreads = 0;
		TotalCPUThreads = 0;
		GPUMemoryBandwidth_GBps = 0.0f;
		PCIeBandwidth_GBps = 0.0f;
		CPUTemperature_C = 0.0f;
		GPUTemperature_C = 0.0f;
	}

	// Helper methods
	FString ToFormattedString() const;
	FString ToCSV() const;
	FString ToJSON() const;
};

/**
 * Metrics configuration
 */
USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCMetricsConfig
{
	GENERATED_BODY()

	// Enable/disable metrics collection
	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	bool bEnableMetrics = true;

	// Collection frequency (seconds)
	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	float CollectionInterval_Seconds = 1.0f;

	// Maximum history size
	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	int32 MaxHistorySize = 3600; // 1 hour at 1-second intervals

	// Enable real-time display
	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	bool bEnableDisplay = true;

	// Export settings
	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	bool bAutoExport = false;

	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	FString ExportDirectory;

	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	float ExportInterval_Seconds = 60.0f;

	// Which metrics to collect
	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	bool bCollectMemoryMetrics = true;

	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	bool bCollectSplittingMetrics = true;

	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	bool bCollectPerformanceMetrics = true;

	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	bool bCollectComponentMetrics = true;

	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	bool bCollectStreamingMetrics = true;

	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	bool bCollectQualityMetrics = true;

	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	bool bCollectErrorMetrics = true;

	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Metrics")
	bool bCollectHardwareMetrics = true;

	FJUSYNCMetricsConfig()
	{
		bEnableMetrics = true;
		CollectionInterval_Seconds = 1.0f;
		MaxHistorySize = 3600;
		bEnableDisplay = true;
		bAutoExport = false;
		ExportDirectory = TEXT("");
		ExportInterval_Seconds = 60.0f;

		bCollectMemoryMetrics = true;
		bCollectSplittingMetrics = true;
		bCollectPerformanceMetrics = true;
		bCollectComponentMetrics = true;
		bCollectStreamingMetrics = true;
		bCollectQualityMetrics = true;
		bCollectErrorMetrics = true;
		bCollectHardwareMetrics = true;
	}
};

// Metrics collection delegate
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJUSYNCMetricsUpdated, const FJUSYNCMetricsData&, MetricsData);
