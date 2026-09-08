#pragma once

#include "CoreMinimal.h"

class AActor;

/**
 * Minimal time-step playback controller.
 *
 * Time steps are detected from numeric filename suffixes (e.g. clip_00012.usda)
 * or supplied explicitly. Playback shows the actors belonging to the current
 * time step and hides the others. This is intentionally a scene-level timeline,
 * not a mesh deformation timeline.
 */
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

    void SetPlaybackSettings(float InFPS, bool bInLoop);
    void Play();
    void Stop();
    void Pause();
    void ShowAll();
    bool IsPlaying() const { return bPlaying; }

    void AddActor(const FString& Filename, AActor* Actor);
    void RemoveActor(AActor* Actor);
    void RemoveFile(const FString& Filename);
    void Clear();

    void Tick(float DeltaSeconds);

    int32 GetTimeStepCount() const { return TimeSteps.Num(); }
    int32 GetCurrentTimeStepIndex() const { return CurrentIndex; }
    void SetCurrentTimeStepIndex(int32 NewIndex);

    static int64 DetectTimeStepOrder(const FString& Filename, FString& OutLabel);

private:
    FTimeStep* FindOrCreateStep(const FString& Filename, int64 Order, const FString& Label);
    void ApplyVisibility();

    TMap<int64, FTimeStep> TimeSteps;
    TArray<int64> SortedOrders;
    TMap<AActor*, int64> ActorToStep;
    int32 CurrentIndex = 0;
    float FPS = 30.0f;
    float Accumulator = 0.0f;
    bool bLoop = true;
    bool bPlaying = false;
};
