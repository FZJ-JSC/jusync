#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "JUSYNCTypes.h"
#include "RealtimeMeshSimple.h"

/**
 * Parsed-file cache keyed by filename + broker hash (size fallback).
 *
 * The cache stores raw parsed USD payload. LUT vertex-color baking is NOT baked
 * into the cached copy; callers receive a working copy and bake on demand so a
 * later gradient/LUT change does not serve stale vertex colors.
 */
class JUSYNC_API FJUSYNCMeshCache
{
public:
    FJUSYNCMeshCache();
    ~FJUSYNCMeshCache();

    static FString MakeKey(const FString& Filename, uint64 HashLo, uint64 HashHi, int64 Size);
    static int64 EstimatePayloadBytes(const TArray<FJUSYNCCompactMeshRef>& Meshes, const TArray<FJUSYNCPointCloudRef>& PointClouds);
    static bool HasBakedVertexColors(const TArray<FJUSYNCCompactMeshRef>& Meshes);

    bool Contains(const FString& Filename, uint64 HashLo, uint64 HashHi, int64 Size, uint64 LUTVersion) const;
    bool Get(const FString& Filename, uint64 HashLo, uint64 HashHi, int64 Size, uint64 LUTVersion,
        TArray<FJUSYNCCompactMeshRef>& OutMeshes,
        TArray<FJUSYNCPointCloudRef>& OutPointClouds,
        TArray<TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet>>* OutPrebuiltStreams = nullptr) const;

    void Store(const FString& Filename, uint64 HashLo, uint64 HashHi, int64 Size, uint64 LUTVersion,
        const TArray<FJUSYNCCompactMeshRef>& Meshes,
        const TArray<FJUSYNCPointCloudRef>& PointClouds,
        const TArray<TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet>>* PrebuiltStreams = nullptr);
    void RemoveFile(const FString& Filename);
    void Clear();

    void SetLimits(int32 InMaxEntries, int64 InMaxBytes);
    int32 GetEntryCount() const;
    int64 GetEstimatedBytes() const;

private:
    struct FEntry
    {
        TArray<FJUSYNCCompactMeshRef> Meshes;
        TArray<FJUSYNCPointCloudRef> PointClouds;
        TArray<TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet>> PrebuiltStreams;
        int64 EstimatedBytes = 0;
        uint64 LUTVersion = 0;
    };

    void EvictLocked();
    void TouchLocked(const FString& Key);
    void RemoveLocked(const FString& Key);

    mutable FCriticalSection Mutex;
    TMap<FString, TUniquePtr<FEntry>> Entries;
    TArray<FString> LRUOrder;
    int64 TotalBytes = 0;
    int32 MaxEntries = 64;
    int64 MaxBytes = 2LL * 1024 * 1024 * 1024;
};
