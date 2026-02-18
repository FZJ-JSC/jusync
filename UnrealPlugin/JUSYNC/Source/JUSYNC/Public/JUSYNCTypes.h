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
        switch(Status)
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

	FJUSYNCBenchmarkResult()
	{
		TestName = TEXT("");
		Timestamp = FDateTime::Now();
		TotalTimeMs = 0.0f;
		TriangleCount = 0;
		VertexCount = 0;
		RAMBeforeBytes = 0;
		RAMAfterBytes = 0;
		FPS = 0.0f;
		FrameTimeMs = 0.0f;
		ActorCount = 0;
		ErrorCount = 0;
		SuccessRate = 0.0f;
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

	// Output directory for CSV files
	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Benchmark")
	FString OutputDirectory;

	// Append timestamp to filename
	UPROPERTY(BlueprintReadWrite, Category = "JUSYNC|Benchmark")
	bool bAppendTimestamp = true;

	FJUSYNCBenchmarkConfig()
	{
		bEnableBenchmarking = false;
		OutputDirectory = TEXT("");
		bAppendTimestamp = true;
	}
};
