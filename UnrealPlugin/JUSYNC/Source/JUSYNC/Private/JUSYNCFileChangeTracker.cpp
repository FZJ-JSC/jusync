#include "JUSYNCFileChangeTracker.h"

uint64 FJUSYNCFileChangeTracker::QueueChange(const FString& Filename, int32 Rank, int64 Size, uint64 HashLo, uint64 HashHi, int32 FileIndex, bool bManual)
{
    if (Filename.IsEmpty())
    {
        return 0;
    }

    FJUSYNCFileChangeRequest& Request = Changes.FindOrAdd(Filename);
    const bool bMetadataChanged =
        Request.Rank != Rank ||
        Request.Size != Size ||
        Request.HashLo != HashLo ||
        Request.HashHi != HashHi;

    Request.Rank = Rank;
    Request.Size = Size;
    Request.HashLo = HashLo;
    Request.HashHi = HashHi;
    Request.FileIndex = FileIndex;
    Request.bManual = bManual || Request.bManual;

    if (Request.State == EJUSYNCFileChangeState::Completed ||
        Request.State == EJUSYNCFileChangeState::Failed ||
        Request.State == EJUSYNCFileChangeState::Queued)
    {
        Request.Generation = NextGeneration++;
        Request.State = EJUSYNCFileChangeState::Queued;
        Request.bSuperseded = false;
        if (!QueueOrder.Contains(Filename))
        {
            QueueOrder.Add(Filename);
        }
    }
    else if (bMetadataChanged)
    {
        Request.bSuperseded = true;
    }

    return Request.Generation;
}

bool FJUSYNCFileChangeTracker::Contains(const FString& Filename) const
{
    const FJUSYNCFileChangeRequest* Request = Find(Filename);
    return Request && Request->State != EJUSYNCFileChangeState::Completed && Request->State != EJUSYNCFileChangeState::Failed;
}

const FJUSYNCFileChangeRequest* FJUSYNCFileChangeTracker::Find(const FString& Filename) const
{
    return Changes.Find(Filename);
}

FJUSYNCFileChangeRequest* FJUSYNCFileChangeTracker::FindMutable(const FString& Filename)
{
    return Changes.Find(Filename);
}

void FJUSYNCFileChangeTracker::SetState(const FString& Filename, EJUSYNCFileChangeState NewState)
{
    if (FJUSYNCFileChangeRequest* Request = FindMutable(Filename))
    {
        Request->State = NewState;
        if (NewState == EJUSYNCFileChangeState::Queued)
        {
            if (!QueueOrder.Contains(Filename))
            {
                QueueOrder.Add(Filename);
            }
        }
    }
}

void FJUSYNCFileChangeTracker::MarkCompleted(const FString& Filename)
{
    FJUSYNCFileChangeRequest* Request = FindMutable(Filename);
    if (Request)
    {
        Request->State = EJUSYNCFileChangeState::Completed;
    }
    QueueOrder.Remove(Filename);

    if (Request && Request->bSuperseded)
    {
        Request->bSuperseded = false;
        Request->Generation = NextGeneration++;
        Request->State = EJUSYNCFileChangeState::Queued;
        QueueOrder.Add(Filename);
    }
}

void FJUSYNCFileChangeTracker::MarkFailed(const FString& Filename)
{
    FJUSYNCFileChangeRequest* Request = FindMutable(Filename);
    if (Request)
    {
        Request->State = EJUSYNCFileChangeState::Failed;
    }
    QueueOrder.Remove(Filename);

    if (Request && Request->bSuperseded)
    {
        Request->bSuperseded = false;
        Request->Generation = NextGeneration++;
        Request->State = EJUSYNCFileChangeState::Queued;
        QueueOrder.Add(Filename);
    }
}

void FJUSYNCFileChangeTracker::Remove(const FString& Filename)
{
    Changes.Remove(Filename);
    QueueOrder.Remove(Filename);
}

void FJUSYNCFileChangeTracker::Clear()
{
    Changes.Empty();
    QueueOrder.Empty();
    NextGeneration = 1;
}

int32 FJUSYNCFileChangeTracker::QueuedCount() const
{
    int32 Count = 0;
    for (const TPair<FString, FJUSYNCFileChangeRequest>& Pair : Changes)
    {
        if (Pair.Value.State == EJUSYNCFileChangeState::Queued)
        {
            Count++;
        }
    }
    return Count;
}

int32 FJUSYNCFileChangeTracker::ActiveCount() const
{
    int32 Count = 0;
    for (const TPair<FString, FJUSYNCFileChangeRequest>& Pair : Changes)
    {
        const EJUSYNCFileChangeState State = Pair.Value.State;
        if (State == EJUSYNCFileChangeState::Downloading ||
            State == EJUSYNCFileChangeState::Parsing ||
            State == EJUSYNCFileChangeState::Spawning)
        {
            Count++;
        }
    }
    return Count;
}

bool FJUSYNCFileChangeTracker::HasActiveWork() const
{
    return QueuedCount() > 0 || ActiveCount() > 0;
}

TArray<TPair<FString, int32>> FJUSYNCFileChangeTracker::GetQueuedFiles() const
{
    TArray<TPair<FString, int32>> Result;
    Result.Reserve(QueueOrder.Num());
    for (const FString& Filename : QueueOrder)
    {
        const FJUSYNCFileChangeRequest* Request = Find(Filename);
        if (Request && Request->State == EJUSYNCFileChangeState::Queued)
        {
            Result.Emplace(Filename, Request->Rank);
        }
    }
    return Result;
}

TPair<FString, int32> FJUSYNCFileChangeTracker::PopNextQueued()
{
    while (QueueOrder.Num() > 0)
    {
        FString Filename = QueueOrder[0];
        QueueOrder.RemoveAt(0);

        FJUSYNCFileChangeRequest* Request = FindMutable(Filename);
        if (Request && Request->State == EJUSYNCFileChangeState::Queued)
        {
            Request->State = EJUSYNCFileChangeState::Downloading;
            return TPair<FString, int32>(Filename, Request->Rank);
        }
    }
    return TPair<FString, int32>();
}

uint64 FJUSYNCFileChangeTracker::GetGeneration(const FString& Filename) const
{
    const FJUSYNCFileChangeRequest* Request = Find(Filename);
    return Request ? Request->Generation : 0;
}

TMap<FString, uint64> FJUSYNCFileChangeTracker::GetGenerations() const
{
    TMap<FString, uint64> Result;
    for (const TPair<FString, FJUSYNCFileChangeRequest>& Pair : Changes)
    {
        Result.Add(Pair.Key, Pair.Value.Generation);
    }
    return Result;
}
