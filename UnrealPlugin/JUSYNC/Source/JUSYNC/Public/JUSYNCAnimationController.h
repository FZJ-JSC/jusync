#pragma once

#include "CoreMinimal.h"
#include "JUSYNCElementNames.h"

class AActor;

class FJUSYNCAnimationController
{
public:
    struct FTimeStep
    {
        FString Label;
        int64 Order = 0;
        TArray<FString> Files;
        TSet<AActor*> Actors;
    };

    struct FActorSlot
    {
        int64 Order = 0;
        FString Element;
        int32 Uniq = -1;
        int32 Seq = 0;
    };

    void SetPlaybackSettings(float InFPS, bool bInLoop);
    void Play();
    void Stop();
    void Pause();
    void ShowAll();
    bool IsPlaying() const { return bPlaying; }

    void AddActor(const FString& Filename, AActor* Actor, const FString& InElementName = FString());
    void RemoveActor(AActor* Actor);
    void RemoveFile(const FString& Filename);
    void Clear();

    void MarkActorLive(AActor* Actor);
    void UnmarkActorLive(AActor* Actor);
    bool IsActorLive(const AActor* Actor) const;

    void Tick(float DeltaSeconds);

    int32 GetTimeStepCount() const { return TimeSteps.Num(); }
    int32 GetCurrentTimeStepIndex() const { return CurrentIndex; }
    void SetCurrentTimeStepIndex(int32 NewIndex);

    static int64 DetectTimeStepOrder(const FString& Filename, FString& OutLabel);

private:
    FTimeStep* FindOrCreateStep(const FString& Filename, int64 Order, const FString& Label);
    void ApplyVisibility();

    void ComputeElementWinners(int64 OrderFilter, TSet<AActor*>& OutWinners) const;

    TMap<int64, FTimeStep> TimeSteps;
    TArray<int64> SortedOrders;
    TMap<AActor*, FActorSlot> ActorSlots;
    TSet<AActor*> LiveActors;
    int32 NextSeq = 0;
    int32 CurrentIndex = 0;
    float FPS = 30.0f;
    float Accumulator = 0.0f;
    bool bLoop = true;
    bool bPlaying = false;
};
