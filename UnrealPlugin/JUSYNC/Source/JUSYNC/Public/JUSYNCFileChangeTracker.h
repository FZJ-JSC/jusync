#pragma once

#include "CoreMinimal.h"

enum class EJUSYNCFileChangeState : uint8
{
    Queued,
    Downloading,
    Parsing,
    Spawning,
    Completed,
    Failed
};

struct FJUSYNCFileChangeRequest
{
    FString Filename;
    int32 Rank = -1;
    int64 Size = 0;
    uint64 HashLo = 0;
    uint64 HashHi = 0;
    uint64 Generation = 0;
    int32 FileIndex = -1;
    bool bManual = false;
    bool bSuperseded = false;
    EJUSYNCFileChangeState State = EJUSYNCFileChangeState::Queued;
};

/**
 * Single game-thread tracker for pending live-update file changes.
 *
 * Replaces the old patchwork of RefreshingFiles / RefreshedFiles /
 * bCommitDiffInProgress / V2ActiveDownloads. The owner must only call this on
 * the game thread; background work captures values and returns via game-thread
 * tasks.
 */
class FJUSYNCFileChangeTracker
{
public:
    uint64 QueueChange(const FString& Filename, int32 Rank, int64 Size, uint64 HashLo, uint64 HashHi, int32 FileIndex = -1, bool bManual = false);
    bool Contains(const FString& Filename) const;
    const FJUSYNCFileChangeRequest* Find(const FString& Filename) const;
    FJUSYNCFileChangeRequest* FindMutable(const FString& Filename);

    void SetState(const FString& Filename, EJUSYNCFileChangeState NewState);
    void MarkCompleted(const FString& Filename);
    void MarkFailed(const FString& Filename);
    void Remove(const FString& Filename);
    void Clear();

    int32 QueuedCount() const;
    int32 ActiveCount() const;
    bool HasActiveWork() const;
    TArray<TPair<FString, int32>> GetQueuedFiles() const;
    TPair<FString, int32> PopNextQueued();

    uint64 GetGeneration(const FString& Filename) const;
    TMap<FString, uint64> GetGenerations() const;

private:
    TMap<FString, FJUSYNCFileChangeRequest> Changes;
    TArray<FString> QueueOrder;
    uint64 NextGeneration = 1;
};
