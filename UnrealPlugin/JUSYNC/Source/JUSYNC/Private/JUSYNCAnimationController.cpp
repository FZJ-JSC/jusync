#include "JUSYNCAnimationController.h"
#include "GameFramework/Actor.h"

void FJUSYNCAnimationController::SetPlaybackSettings(float InFPS, bool bInLoop)
{
    FPS = FMath::Max(1.0f, InFPS);
    bLoop = bInLoop;
    Accumulator = 0.0f;
}

void FJUSYNCAnimationController::Play()
{
    if (SortedOrders.Num() <= 1)
    {
        return;
    }
    bPlaying = true;
    Accumulator = 0.0f;
    ApplyVisibility();
}

void FJUSYNCAnimationController::Stop()
{
    bPlaying = false;
    Accumulator = 0.0f;
    CurrentIndex = 0;
    ApplyVisibility();
}

void FJUSYNCAnimationController::Pause()
{
    bPlaying = false;
}

void FJUSYNCAnimationController::ShowAll()
{
    bPlaying = false;
    for (const TPair<AActor*, int64>& Pair : ActorToStep)
    {
        if (Pair.Key && Pair.Key->IsValidLowLevel())
        {
            Pair.Key->SetActorHiddenInGame(false);
        }
    }
}

void FJUSYNCAnimationController::AddActor(const FString& Filename, AActor* Actor)
{
    if (!Actor)
    {
        return;
    }

    FString Label;
    int64 Order = DetectTimeStepOrder(Filename, Label);
    if (Order < 0)
    {
        Order = 0;
        Label = FPaths::GetBaseFilename(Filename);
    }

    FTimeStep* Step = FindOrCreateStep(Filename, Order, Label);
    if (!Step)
    {
        return;
    }

    Step->Actors.Add(Actor);
    ActorToStep.Add(Actor, Order);
    ApplyVisibility();
}

void FJUSYNCAnimationController::RemoveActor(AActor* Actor)
{
    if (!Actor)
    {
        return;
    }

    int64* Order = ActorToStep.Find(Actor);
    if (Order)
    {
        FTimeStep* Step = TimeSteps.Find(*Order);
        if (Step)
        {
            Step->Actors.Remove(Actor);
            if (Step->Actors.Num() == 0 && Step->Files.Num() == 0)
            {
                TimeSteps.Remove(*Order);
                SortedOrders.Remove(*Order);
            }
        }
        ActorToStep.Remove(Actor);
    }
}

void FJUSYNCAnimationController::RemoveFile(const FString& Filename)
{
    for (auto It = TimeSteps.CreateIterator(); It; ++It)
    {
        It.Value().Files.Remove(Filename);
        if (It.Value().Files.Num() == 0 && It.Value().Actors.Num() == 0)
        {
            SortedOrders.Remove(It.Key());
            It.RemoveCurrent();
        }
    }
}

void FJUSYNCAnimationController::Clear()
{
    TimeSteps.Empty();
    SortedOrders.Empty();
    ActorToStep.Empty();
    CurrentIndex = 0;
    Accumulator = 0.0f;
    bPlaying = false;
}

void FJUSYNCAnimationController::Tick(float DeltaSeconds)
{
    if (!bPlaying || SortedOrders.Num() <= 1)
    {
        return;
    }

    Accumulator += DeltaSeconds;
    const float FrameTime = 1.0f / FPS;
    while (Accumulator >= FrameTime)
    {
        Accumulator -= FrameTime;
        CurrentIndex++;
        if (CurrentIndex >= SortedOrders.Num())
        {
            if (bLoop)
            {
                CurrentIndex = 0;
            }
            else
            {
                CurrentIndex = SortedOrders.Num() - 1;
                bPlaying = false;
                break;
            }
        }
    }
    ApplyVisibility();
}

void FJUSYNCAnimationController::SetCurrentTimeStepIndex(int32 NewIndex)
{
    if (SortedOrders.Num() == 0)
    {
        CurrentIndex = 0;
        return;
    }
    CurrentIndex = FMath::Clamp(NewIndex, 0, SortedOrders.Num() - 1);
    ApplyVisibility();
}

int64 FJUSYNCAnimationController::DetectTimeStepOrder(const FString& Filename, FString& OutLabel)
{
    OutLabel = FPaths::GetBaseFilename(Filename);
    const FString Base = FPaths::GetBaseFilename(Filename);

    int32 Start = INDEX_NONE;
    int32 End = INDEX_NONE;
    for (int32 i = Base.Len() - 1; i >= 0; --i)
    {
        if (FChar::IsDigit(Base[i]))
        {
            End = i + 1;
        }
        else if (End != INDEX_NONE)
        {
            Start = i + 1;
            break;
        }
    }

    if (End == INDEX_NONE)
    {
        return -1;
    }
    if (Start == INDEX_NONE)
    {
        Start = 0;
    }

    int64 Order = 0;
    for (int32 i = Start; i < End; ++i)
    {
        Order = Order * 10 + static_cast<int64>(Base[i] - TEXT('0'));
    }
    return Order;
}

FJUSYNCAnimationController::FTimeStep* FJUSYNCAnimationController::FindOrCreateStep(const FString& Filename, int64 Order, const FString& Label)
{
    FTimeStep* Step = TimeSteps.Find(Order);
    const bool bNewStep = (Step == nullptr);
    if (!Step)
    {
        Step = &TimeSteps.Add(Order);
        Step->Label = Label;
        Step->Order = Order;
    }

    if (!Step->Files.Contains(Filename))
    {
        Step->Files.Add(Filename);
    }

    if (bNewStep && !SortedOrders.Contains(Order))
    {
        SortedOrders.Add(Order);
        SortedOrders.Sort();
    }

    return Step;
}

void FJUSYNCAnimationController::ApplyVisibility()
{
    if (SortedOrders.Num() <= 1)
    {
        for (const TPair<AActor*, int64>& Pair : ActorToStep)
        {
            if (Pair.Key && Pair.Key->IsValidLowLevel())
            {
                Pair.Key->SetActorHiddenInGame(false);
            }
        }
        return;
    }

    const int64 CurrentOrder = SortedOrders.IsValidIndex(CurrentIndex) ? SortedOrders[CurrentIndex] : INDEX_NONE;
    for (const TPair<AActor*, int64>& Pair : ActorToStep)
    {
        if (Pair.Key && Pair.Key->IsValidLowLevel())
        {
            Pair.Key->SetActorHiddenInGame(Pair.Value != CurrentOrder);
        }
    }
}
