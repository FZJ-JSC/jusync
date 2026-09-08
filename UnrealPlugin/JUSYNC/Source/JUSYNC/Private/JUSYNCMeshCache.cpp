#include "JUSYNCMeshCache.h"

FJUSYNCMeshCache::FJUSYNCMeshCache()
{
}

FJUSYNCMeshCache::~FJUSYNCMeshCache()
{
}

FString FJUSYNCMeshCache::MakeKey(const FString& Filename, uint64 HashLo, uint64 HashHi, int64 Size)
{
    if (HashLo != 0 || HashHi != 0)
    {
        return FString::Printf(TEXT("%s|H:%llx-%llx|S:%lld"), *Filename,
            static_cast<unsigned long long>(HashLo), static_cast<unsigned long long>(HashHi), static_cast<long long>(Size));
    }
    return FString::Printf(TEXT("%s|S:%lld"), *Filename, static_cast<long long>(Size));
}

int64 FJUSYNCMeshCache::EstimatePayloadBytes(const TArray<FJUSYNCCompactMeshRef>& Meshes, const TArray<FJUSYNCPointCloudRef>& PointClouds)
{
    int64 Bytes = 0;
    for (const FJUSYNCCompactMeshRef& M : Meshes)
    {
        if (M)
        {
            Bytes += M->EstimateBytes();
        }
    }
    for (const FJUSYNCPointCloudRef& P : PointClouds)
    {
        if (!P)
        {
            continue;
        }
        Bytes += 128;
        Bytes += static_cast<int64>(P->Positions.Num()) * sizeof(FVector);
        Bytes += static_cast<int64>(P->Colors.Num()) * sizeof(FColor);
        Bytes += static_cast<int64>(P->Widths.Num()) * sizeof(float);
        Bytes += static_cast<int64>(P->LidarPoints.Num()) * sizeof(FLidarPointCloudPoint);
    }
    return Bytes;
}

bool FJUSYNCMeshCache::HasBakedVertexColors(const TArray<FJUSYNCCompactMeshRef>& Meshes)
{
    for (const FJUSYNCCompactMeshRef& M : Meshes)
    {
        if (M && M->bHasBakedColors)
        {
            return true;
        }
    }
    return false;
}

bool FJUSYNCMeshCache::Contains(const FString& Filename, uint64 HashLo, uint64 HashHi, int64 Size, uint64 LUTVersion) const
{
    const FString Key = MakeKey(Filename, HashLo, HashHi, Size);
    FScopeLock Lock(&Mutex);
    const TUniquePtr<FEntry>* Found = Entries.Find(Key);
    if (!Found)
    {
        return false;
    }
    const FEntry* Entry = Found->Get();
    return Entry->LUTVersion == LUTVersion ||
           (Entry->LUTVersion == 0 && !HasBakedVertexColors(Entry->Meshes));
}

bool FJUSYNCMeshCache::Get(const FString& Filename, uint64 HashLo, uint64 HashHi, int64 Size, uint64 LUTVersion,
    TArray<FJUSYNCCompactMeshRef>& OutMeshes,
    TArray<FJUSYNCPointCloudRef>& OutPointClouds,
    TArray<TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet>>* OutPrebuiltStreams) const
{
    const FString Key = MakeKey(Filename, HashLo, HashHi, Size);
    FScopeLock Lock(&Mutex);
    const TUniquePtr<FEntry>* Found = Entries.Find(Key);
    const FEntry* Entry = Found ? Found->Get() : nullptr;
    if (!Entry)
    {
        return false;
    }

    if (Entry->LUTVersion != LUTVersion &&
        !(Entry->LUTVersion == 0 && !HasBakedVertexColors(Entry->Meshes)))
    {
        return false;
    }

    OutMeshes = Entry->Meshes;
    OutPointClouds = Entry->PointClouds;

    if (OutPrebuiltStreams)
    {
        OutPrebuiltStreams->Reset();
        OutPrebuiltStreams->Reserve(Entry->PrebuiltStreams.Num());
        for (const TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet>& Template : Entry->PrebuiltStreams)
        {
            if (Template)
            {
                OutPrebuiltStreams->Add(MakeUnique<RealtimeMesh::FRealtimeMeshStreamSet>(*Template, true));
            }
            else
            {
                OutPrebuiltStreams->Add(nullptr);
            }
        }
    }

    // const Get still updates LRU by mutating the non-logical-const ordering arrays.
    const_cast<FJUSYNCMeshCache*>(this)->TouchLocked(Key);
    return true;
}

void FJUSYNCMeshCache::Store(const FString& Filename, uint64 HashLo, uint64 HashHi, int64 Size, uint64 LUTVersion,
    const TArray<FJUSYNCCompactMeshRef>& Meshes,
    const TArray<FJUSYNCPointCloudRef>& PointClouds,
    const TArray<TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet>>* PrebuiltStreams)
{
    if (Filename.IsEmpty())
    {
        return;
    }

    const FString Key = MakeKey(Filename, HashLo, HashHi, Size);
    FScopeLock Lock(&Mutex);

    if (Entries.Contains(Key))
    {
        RemoveLocked(Key);
    }

    TUniquePtr<FEntry> Entry = MakeUnique<FEntry>();
    Entry->Meshes = Meshes;
    Entry->PointClouds = PointClouds;
    if (PrebuiltStreams)
    {
        Entry->PrebuiltStreams.Reserve(PrebuiltStreams->Num());
        for (const TUniquePtr<RealtimeMesh::FRealtimeMeshStreamSet>& Template : *PrebuiltStreams)
        {
            if (Template)
            {
                Entry->PrebuiltStreams.Add(MakeUnique<RealtimeMesh::FRealtimeMeshStreamSet>(*Template, true));
            }
            else
            {
                Entry->PrebuiltStreams.Add(nullptr);
            }
        }
    }
    Entry->EstimatedBytes = EstimatePayloadBytes(Entry->Meshes, Entry->PointClouds);
    Entry->LUTVersion = LUTVersion;

    TotalBytes += Entry->EstimatedBytes;
    Entries.Add(Key, MoveTemp(Entry));
    TouchLocked(Key);
    EvictLocked();
}

void FJUSYNCMeshCache::RemoveFile(const FString& Filename)
{
    FScopeLock Lock(&Mutex);
    TArray<FString> Keys;
    const FString Prefix = Filename + TEXT("|");
    for (const TPair<FString, TUniquePtr<FEntry>>& Pair : Entries)
    {
        if (Pair.Key.StartsWith(Prefix))
        {
            Keys.Add(Pair.Key);
        }
    }
    for (const FString& Key : Keys)
    {
        RemoveLocked(Key);
    }
}

void FJUSYNCMeshCache::Clear()
{
    FScopeLock Lock(&Mutex);
    Entries.Empty();
    LRUOrder.Empty();
    TotalBytes = 0;
}

void FJUSYNCMeshCache::SetLimits(int32 InMaxEntries, int64 InMaxBytes)
{
    FScopeLock Lock(&Mutex);
    MaxEntries = FMath::Max(1, InMaxEntries);
    MaxBytes = FMath::Max<int64>(1024 * 1024, InMaxBytes);
    EvictLocked();
}

int32 FJUSYNCMeshCache::GetEntryCount() const
{
    FScopeLock Lock(&Mutex);
    return Entries.Num();
}

int64 FJUSYNCMeshCache::GetEstimatedBytes() const
{
    FScopeLock Lock(&Mutex);
    return TotalBytes;
}

void FJUSYNCMeshCache::EvictLocked()
{
    while (Entries.Num() > MaxEntries || TotalBytes > MaxBytes)
    {
        FString VictimKey;
        for (const FString& Key : LRUOrder)
        {
            if (Entries.Contains(Key))
            {
                VictimKey = Key;
                break;
            }
        }
        if (VictimKey.IsEmpty())
        {
            break;
        }
        RemoveLocked(VictimKey);
    }
}

void FJUSYNCMeshCache::TouchLocked(const FString& Key)
{
    LRUOrder.Remove(Key);
    LRUOrder.Add(Key);
}

void FJUSYNCMeshCache::RemoveLocked(const FString& Key)
{
    if (TUniquePtr<FEntry>* Entry = Entries.Find(Key))
    {
        TotalBytes -= Entry->Get()->EstimatedBytes;
        Entries.Remove(Key);
    }
    LRUOrder.Remove(Key);
}
