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

    FJUSYNCFileData()
    {
        Filename = TEXT("");
        Hash = TEXT("");
        FileType = TEXT("");
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

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJUSYNCFileReceived, const FJUSYNCFileData&, FileData);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJUSYNCMessageReceived, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJUSYNCProcessingProgress, float, Progress, const FString&, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJUSYNCError, const FString&, ErrorType, const FString&, ErrorMessage);
