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
    for (const TPair<AActor*, FActorSlot>& Pair : ActorSlots)
    {
        if (Pair.Key && Pair.Key->IsValidLowLevel())
        {
            Pair.Key->SetActorHiddenInGame(false);
        }
    }
}

void FJUSYNCAnimationController::AddActor(const FString& Filename, AActor* Actor, const FString& InElementName)
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

    const JUSYNCElementNames::FVariant Variant = InElementName.IsEmpty()
        ? JUSYNCElementNames::Parse(Filename)
        : JUSYNCElementNames::ParseElement(InElementName);

    FActorSlot& Slot = ActorSlots.FindOrAdd(Actor);
    if (Slot.Seq == 0)
    {
        Slot.Seq = ++NextSeq;
    }
    Slot.Order = Order;
    Slot.Element = Variant.Element;
    Slot.Uniq = Variant.Uniq;

    ApplyVisibility();
}

void FJUSYNCAnimationController::RemoveActor(AActor* Actor)
{
    if (!Actor)
    {
        return;
    }

    if (FActorSlot* Slot = ActorSlots.Find(Actor))
    {
        FTimeStep* Step = TimeSteps.Find(Slot->Order);
        if (Step)
        {
            Step->Actors.Remove(Actor);
            if (Step->Actors.Num() == 0 && Step->Files.Num() == 0)
            {
                TimeSteps.Remove(Slot->Order);
                SortedOrders.Remove(Slot->Order);
            }
        }
        ActorSlots.Remove(Actor);
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
    ActorSlots.Empty();
    NextSeq = 0;
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
    FString Base = FPaths::GetBaseFilename(Filename);

    // Progressive-refinement subsets ("<base>_s<k>.geo", RawGeo v2) belong to
    // their base file's timestep; the "_s<k>" tail is neither a time nor a
    // rank mark. Without this strip, "...__7_s2" would bucket as step 2.
    const int32 RefineUnderscore = Base.Find(TEXT("_s"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (RefineUnderscore != INDEX_NONE)
    {
        const FString RefineTail = Base.Mid(RefineUnderscore + 2);
        if (!RefineTail.IsEmpty() && RefineTail.IsNumeric())
        {
            Base = Base.Left(RefineUnderscore);
            OutLabel = Base;
        }
    }

    const int32 TimeSep = Base.Find(TEXT("__"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
    if (TimeSep != INDEX_NONE)
    {
        FString TimeTok = Base.Mid(TimeSep + 2);
        const int32 UniqueSuffix = TimeTok.Find(TEXT("_u"), ESearchCase::IgnoreCase);
        if (UniqueSuffix != INDEX_NONE)
        {
            TimeTok = TimeTok.Left(UniqueSuffix);
        }

        // MPI clip keys carry the owning rank between the separator and the time
        // ("__r1_0.500000"), so a bare IsNumeric() test fails and the fallback
        // below then reads the trailing decimals as the order - collapsing every
        // timestep of every rank onto one step. Drop the rank token first.
        if (TimeTok.StartsWith(TEXT("r"), ESearchCase::IgnoreCase))
        {
            const int32 RankSep = TimeTok.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
            if (RankSep > 1)
            {
                const FString RankDigits = TimeTok.Mid(1, RankSep - 1);
                if (RankDigits.IsNumeric())
                {
                    TimeTok = TimeTok.Mid(RankSep + 1);
                }
            }
        }

        if (!TimeTok.IsEmpty() && TimeTok.IsNumeric())
        {
            return static_cast<int64>(FCString::Atod(*TimeTok) * 1000000.0 + 0.5);
        }
    }

    // A trailing rank mark is not a time step. Without this, "cloud_r1" and
    // "cloud_r10" are bucketed as steps 1 and 10 and one instant of a partitioned
    // element is spread over several animation steps; only MPI keys carry "_rN".
    FString Stripped = Base;
    const int32 RankUnderscore = Stripped.Find(TEXT("_r"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (RankUnderscore != INDEX_NONE)
    {
        const FString RankTail = Stripped.Mid(RankUnderscore + 2);
        if (RankTail.IsNumeric() && !RankTail.Contains(TEXT(".")))
        {
            Stripped = Stripped.Left(RankUnderscore);
        }
    }

    int32 Start = INDEX_NONE;
    int32 End = INDEX_NONE;
    for (int32 i = Stripped.Len() - 1; i >= 0; --i)
    {
        if (FChar::IsDigit(Stripped[i]))
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
        Order = Order * 10 + static_cast<int64>(Stripped[i] - TEXT('0'));
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
        TSet<AActor*> Winners;
        ComputeElementWinners(INDEX_NONE, Winners);
        for (const TPair<AActor*, FActorSlot>& Pair : ActorSlots)
        {
            if (Pair.Key && Pair.Key->IsValidLowLevel())
            {
                Pair.Key->SetActorHiddenInGame(!Winners.Contains(Pair.Key));
            }
        }
        return;
    }

    const int64 CurrentOrder = SortedOrders.IsValidIndex(CurrentIndex) ? SortedOrders[CurrentIndex] : INDEX_NONE;
    TSet<AActor*> Winners;
    ComputeElementWinners(CurrentOrder, Winners);

    for (const TPair<AActor*, FActorSlot>& Pair : ActorSlots)
    {
        if (Pair.Key && Pair.Key->IsValidLowLevel())
        {
            const bool bVisible = (Pair.Value.Order == CurrentOrder) && Winners.Contains(Pair.Key);
            Pair.Key->SetActorHiddenInGame(!bVisible);
        }
    }
}

void FJUSYNCAnimationController::ComputeElementWinners(int64 OrderFilter, TSet<AActor*>& OutWinners) const
{
    OutWinners.Empty(ActorSlots.Num());

    TMap<FString, FActorSlot> Best;
    Best.Reserve(ActorSlots.Num());

    for (const TPair<AActor*, FActorSlot>& Pair : ActorSlots)
    {
        if (!Pair.Key || !Pair.Key->IsValidLowLevel())
        {
            continue;
        }
        if (OrderFilter != INDEX_NONE && Pair.Value.Order != OrderFilter)
        {
            continue;
        }

        const FString Key = Pair.Value.Element.IsEmpty()
            ? FString::Printf(TEXT("%p"), Pair.Key)
            : Pair.Value.Element;

        FActorSlot* Cur = Best.Find(Key);
        const bool bNewer = !Cur
            || Pair.Value.Uniq > Cur->Uniq
            || (Pair.Value.Uniq == Cur->Uniq && Pair.Value.Seq > Cur->Seq);
        if (bNewer)
        {
            Best.Add(Key, Pair.Value);
        }
    }

    for (const TPair<AActor*, FActorSlot>& Pair : ActorSlots)
    {
        if (!Pair.Key || !Pair.Key->IsValidLowLevel())
        {
            continue;
        }
        if (OrderFilter != INDEX_NONE && Pair.Value.Order != OrderFilter)
        {
            continue;
        }
        const FString Key = Pair.Value.Element.IsEmpty()
            ? FString::Printf(TEXT("%p"), Pair.Key)
            : Pair.Value.Element;
        if (const FActorSlot* Cur = Best.Find(Key))
        {
            if (Cur->Seq == Pair.Value.Seq)
            {
                OutWinners.Add(Pair.Key);
            }
        }
    }
}
