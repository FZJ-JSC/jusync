#include "JUSYNCUSDLoader.h"
#include "JUSYNCSubsystem.h"
#include "Async/Async.h"
#include "Tasks/Task.h"

void FJUSYNCUSDLoader::BakeLUTIntoMesh(FJUSYNCMeshData& Mesh, const TArray<FColor>& LUT)
{
    if (LUT.Num() <= 1)
    {
        return;
    }
    if (Mesh.Vertices.Num() == 0 || Mesh.UVs.Num() != Mesh.Vertices.Num())
    {
        return;
    }

    Mesh.VertexColors.SetNum(Mesh.Vertices.Num());
    for (int32 v = 0; v < Mesh.Vertices.Num(); ++v)
    {
        const float Scalar = Mesh.UVs.IsValidIndex(v) ? Mesh.UVs[v].X : 0.f;
        const int32 Idx = FMath::Clamp(FMath::RoundToInt(Scalar * (LUT.Num() - 1)), 0, LUT.Num() - 1);
        Mesh.VertexColors[v] = LUT[Idx];
    }
}

void FJUSYNCUSDLoader::BakeLUTIntoMeshes(TArray<FJUSYNCMeshData>& Meshes, const TArray<FColor>& LUT)
{
    if (LUT.Num() <= 1)
    {
        return;
    }
    for (auto& Mesh : Meshes)
    {
        BakeLUTIntoMesh(Mesh, LUT);
    }
}

bool FJUSYNCUSDLoader::ParseBuffer(UJUSYNCSubsystem* Subsystem, const TArray<uint8>& Buffer, const FString& Filename,
    TArray<FJUSYNCMeshData>& OutMeshes, TArray<FJUSYNCPointCloudData>& OutPointClouds)
{
    if (!Subsystem)
    {
        return false;
    }
    return Subsystem->LoadUSDFullFromBufferNoCopy(Buffer, Filename, OutMeshes, OutPointClouds);
}

bool FJUSYNCUSDLoader::ParseBufferCompact(UJUSYNCSubsystem* Subsystem, const TArray<uint8>& Buffer, const FString& Filename,
    TArray<FJUSYNCCompactMeshRef>& OutMeshes, TArray<FJUSYNCPointCloudRef>& OutPointClouds)
{
    if (!Subsystem)
    {
        return false;
    }
    return Subsystem->LoadUSDFullCompactFromBuffer(Buffer, Filename, OutMeshes, OutPointClouds);
}

void FJUSYNCUSDLoader::BakeLUTIntoCompactMesh(FJUSYNCCompactMeshData& Mesh, const TArray<FColor>& LUT)
{
    if (LUT.Num() <= 1)
    {
        return;
    }
    if (Mesh.Points.Num() == 0 || Mesh.UVs.Num() != Mesh.Points.Num())
    {
        return;
    }

    Mesh.VertexColors.SetNum(Mesh.Points.Num());
    for (int32 v = 0; v < Mesh.Points.Num(); ++v)
    {
        const float Scalar = Mesh.UVs.IsValidIndex(v) ? Mesh.UVs[v].X : 0.f;
        const int32 Idx = FMath::Clamp(FMath::RoundToInt(Scalar * (LUT.Num() - 1)), 0, LUT.Num() - 1);
        Mesh.VertexColors[v] = LUT[Idx];
    }
    Mesh.bHasBakedColors = true;
}

void FJUSYNCUSDLoader::BakeLUTIntoCompactMeshes(TArray<FJUSYNCCompactMeshRef>& Meshes, const TArray<FColor>& LUT)
{
    if (LUT.Num() <= 1)
    {
        return;
    }
    for (FJUSYNCCompactMeshRef& Mesh : Meshes)
    {
        if (Mesh)
        {
            FJUSYNCUSDLoader::BakeLUTIntoCompactMesh(*Mesh, LUT);
        }
    }
}

void FJUSYNCUSDLoader::EnsureCompactMeshesBaked(TArray<FJUSYNCCompactMeshRef>& Meshes, const TArray<FColor>& LUT, uint64 LUTVersion)
{
    if (LUT.Num() <= 1)
    {
        return;
    }

    for (FJUSYNCCompactMeshRef& Mesh : Meshes)
    {
        if (!Mesh)
        {
            continue;
        }

        if (Mesh->bHasBakedColors && Mesh->LUTVersion == LUTVersion)
        {
            continue;
        }

        FJUSYNCCompactMeshRef MutableMesh = MakeShared<FJUSYNCCompactMeshData, ESPMode::ThreadSafe>(*Mesh);
        FJUSYNCUSDLoader::BakeLUTIntoCompactMesh(*MutableMesh, LUT);
        MutableMesh->LUTVersion = LUTVersion;
        Mesh = MoveTemp(MutableMesh);
    }
}

void FJUSYNCUSDLoader::EnsureCompactMeshBaked(FJUSYNCCompactMeshRef& Mesh, const TArray<FColor>& LUT, uint64 LUTVersion)
{
    if (!Mesh || LUT.Num() <= 1)
    {
        return;
    }

    if (Mesh->bHasBakedColors && Mesh->LUTVersion == LUTVersion)
    {
        return;
    }

    FJUSYNCCompactMeshRef MutableMesh = MakeShared<FJUSYNCCompactMeshData, ESPMode::ThreadSafe>(*Mesh);
    FJUSYNCUSDLoader::BakeLUTIntoCompactMesh(*MutableMesh, LUT);
    MutableMesh->LUTVersion = LUTVersion;
    Mesh = MoveTemp(MutableMesh);
}

static void JUSYNCBuildPrebuiltStreams(FJUSYNCParsedFileResult& Result)
{
    if (Result.bHasCompactMeshes)
    {
        Result.PrebuiltStreams.SetNum(Result.CompactMeshes.Num());
        for (int32 i = 0; i < Result.CompactMeshes.Num(); ++i)
        {
            if (Result.PrebuiltStreams.IsValidIndex(i) && Result.PrebuiltStreams[i])
            {
                continue;
            }
            if (!Result.CompactMeshes[i] || !Result.CompactMeshes[i]->IsValid())
            {
                continue;
            }

            TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet> Streams = MakeUnique<RealtimeMesh::FRealtimeMeshStreamSet>();
            if (JUSYNCBuildRealtimeMeshStreams(*Result.CompactMeshes[i], *Streams))
            {
                Result.PrebuiltStreams[i] = MoveTemp(Streams);
            }
        }
        return;
    }

    Result.PrebuiltStreams.SetNum(Result.Meshes.Num());
    for (int32 i = 0; i < Result.Meshes.Num(); ++i)
    {
        if (Result.PrebuiltStreams.IsValidIndex(i) && Result.PrebuiltStreams[i])
        {
            continue;
        }
        if (!Result.Meshes[i].IsValid())
        {
            continue;
        }

        TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet> Streams = MakeUnique<RealtimeMesh::FRealtimeMeshStreamSet>();
        if (JUSYNCBuildRealtimeMeshStreams(Result.Meshes[i], *Streams))
        {
            Result.PrebuiltStreams[i] = MoveTemp(Streams);
        }
    }
}

void FJUSYNCUSDLoader::ParseAsync(
    UJUSYNCSubsystem* Subsystem,
    TArray<uint8>&& Buffer,
    FString Filename,
    TArray<FColor> LUT,
    uint64 LUTVersion,
    bool bPrebuildStreams,
    int32 FileIndex,
    int32 Rank,
    uint64 Generation,
    uint64 HashLo,
    uint64 HashHi,
    int64 Size,
    TFunction<void(FJUSYNCParsedFileResult&&)>&& OnGameThread)
{
    TWeakObjectPtr<UJUSYNCSubsystem> WeakSubsystem = Subsystem;

    FFunctionGraphTask::CreateAndDispatchWhenReady(
        [WeakSubsystem, Buffer = MoveTemp(Buffer), Filename = MoveTemp(Filename), LUT = MoveTemp(LUT), LUTVersion, bPrebuildStreams, FileIndex, Rank, Generation, HashLo, HashHi, Size, OnGameThread = MoveTemp(OnGameThread)]() mutable
        {
            FJUSYNCParsedFileResult Result;
            Result.Filename = MoveTemp(Filename);
            Result.FileIndex = FileIndex;
            Result.Rank = Rank;
            Result.Generation = Generation;
            Result.HashLo = HashLo;
            Result.HashHi = HashHi;
            Result.Size = Size;
            Result.LUTVersion = LUTVersion;

            if (UJUSYNCSubsystem* S = WeakSubsystem.Get())
            {
                Result.bParsed = FJUSYNCUSDLoader::ParseBufferCompact(S, Buffer, Result.Filename, Result.CompactMeshes, Result.CompactPointClouds);
                if (Result.bParsed)
                {
                    Result.bHasCompactMeshes = true;
                    Result.bHasCompactPointClouds = Result.CompactPointClouds.Num() > 0;
                }
                if (Result.bParsed && LUT.Num() > 1)
                {
                    FJUSYNCUSDLoader::EnsureCompactMeshesBaked(Result.CompactMeshes, LUT, LUTVersion);
                }
                if (Result.bParsed && bPrebuildStreams)
                {
                    JUSYNCBuildPrebuiltStreams(Result);
                }
            }

            FFunctionGraphTask::CreateAndDispatchWhenReady(
                [OnGameThread = MoveTemp(OnGameThread), Result = MoveTemp(Result)]() mutable
                {
                    if (OnGameThread)
                    {
                        OnGameThread(MoveTemp(Result));
                    }
                },
                TStatId(), nullptr, ENamedThreads::GameThread);
        },
        TStatId(), nullptr, ENamedThreads::AnyBackgroundThreadNormalTask);
}
