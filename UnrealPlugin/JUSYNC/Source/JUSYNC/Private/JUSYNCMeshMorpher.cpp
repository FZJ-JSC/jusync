#include "JUSYNCMeshMorpher.h"

#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Hash/CityHash.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "RealtimeMeshComponent.h"

const FName FJUSYNCMeshMorpher::MorphTParameter(TEXT("JusyncMorphT"));
const FName FJUSYNCMeshMorpher::MorphActiveParameter(TEXT("JusyncMorphActive"));

static TAutoConsoleVariable<int32> CVarJUSyncMorphEnabled(
    TEXT("jusync.Morph.Enabled"), 1,
    TEXT("GPU-morph streamed USD mesh updates between consecutive time samples.\n")
    TEXT("Requires the JusyncMorphT/JusyncMorphActive WPO branch in the applied material;")
    TEXT(" meshes without it keep the instant-apply path."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJUSyncMorphBlendMs(
    TEXT("jusync.Morph.MeshBlendMs"), 0,
    TEXT("Fixed mesh blend window in ms; 0 = auto (measured sample gap clamped by MeshMaxBlendMs)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJUSyncMorphMaxBlendMs(
    TEXT("jusync.Morph.MeshMaxBlendMs"), 2000,
    TEXT("Cap in ms for the auto mesh blend window."),
    ECVF_Default);

namespace JUSyncMorph
{
    static constexpr double MinBlendGapSeconds = 0.004;
    static constexpr double MinDurationSeconds = 0.008;

    static float EaseSaturated(double u)
    {
        const float t = FMath::Clamp(static_cast<float>(u), 0.f, 1.f);
        return t * t * (3.f - 2.f * t); // smoothstep: zero velocity at both keys
    }

    static void WriteDisplayedPrev(
        const TArray<FVector3f>& FromA, const TArray<FVector3f>& FromB, float S,
        int32 NewCount, const TArray<FVector3f>& OwnPositions, TArray<FVector3f>& Out)
    {
        Out.SetNumUninitialized(NewCount);
        const int32 Overlap = FMath::Min3(NewCount, FromA.Num(), FromB.Num());
        for (int32 i = 0; i < Overlap; ++i)
        {
            Out[i] = FMath::Lerp(FromA[i], FromB[i], S);
        }
        for (int32 i = Overlap; i < NewCount; ++i)
        {
            Out[i] = OwnPositions[i];
        }
    }
}

FJUSYNCMeshMorpher::FJUSYNCMeshMorpher(TWeakObjectPtr<UObject> InOwner)
    : Owner(InOwner)
{
}

bool FJUSYNCMeshMorpher::IsEnabled() const
{
    return CVarJUSyncMorphEnabled.GetValueOnGameThread() != 0;
}

bool FJUSYNCMeshMorpher::IsTickable() const
{
    return Owner.IsValid() && Entries.Num() > 0;
}

uint64 FJUSYNCMeshMorpher::ComputeTriHash(const TArray<int32>& Triangles)
{
    if (Triangles.Num() == 0)
    {
        return 0;
    }
    return CityHash64(reinterpret_cast<const char*>(Triangles.GetData()),
                      static_cast<size_t>(Triangles.Num()) * sizeof(int32));
}

bool FJUSYNCMeshMorpher::IsMorphCapable(const UMaterialInterface* Material)
{
    if (!Material)
    {
        return false;
    }
    float Value = 0.f;
    return Material->GetScalarParameterValue(MorphTParameter, Value);
}

UMaterialInstanceDynamic* FJUSYNCMeshMorpher::ResolveMorphMID(URealtimeMeshComponent* Comp, FEntry& Entry)
{
    UMaterialInterface* Current = Comp ? Comp->GetMaterial(0) : nullptr;
    if (!Current)
    {
        Entry.MorphMID.Reset();
        return nullptr;
    }

    if (UMaterialInstanceDynamic* CurrentMID = Cast<UMaterialInstanceDynamic>(Current))
    {
        if (IsMorphCapable(CurrentMID))
        {
            Entry.MorphMID = CurrentMID;
            return CurrentMID;
        }
        Entry.MorphMID.Reset();
        return nullptr;
    }

    // Static material that has the parameters -> create our own MID.

    if (IsMorphCapable(Current))
    {
        UMaterialInstanceDynamic* NewMID = UMaterialInstanceDynamic::Create(Current, Comp);
        if (NewMID)
        {
            NewMID->SetScalarParameterValue(MorphActiveParameter, 0.f);
            NewMID->SetScalarParameterValue(MorphTParameter, 1.f);
            Comp->SetMaterial(0, NewMID);
            Entry.MorphMID = NewMID;
            return NewMID;
        }
    }

    Entry.MorphMID.Reset();
    return nullptr;
}

FJUSYNCMeshMorpher::EApplyMode FJUSYNCMeshMorpher::BeginUpdate(
    URealtimeMeshComponent* Comp, UMaterialInterface* IntentMaterial,
    const FJUSYNCCompactMeshData& MeshData,
    double NowSeconds, TArray<FVector3f>& OutPrevPoints)
{
    OutPrevPoints.Reset();

    if (!IsEnabled() || !Comp || MeshData.Points.Num() == 0)
    {
        return EApplyMode::Normal;
    }

    FEntry* Entry = Entries.Find(Comp);
    if (!Entry || !Entry->bHasTriHash || Entry->CurrPoints.Num() == 0)
    {
        Entries.FindOrAdd(Comp); // first sight of this component: Commit records baseline
        return EApplyMode::Normal;
    }

    const uint64 NewHash = ComputeTriHash(MeshData.Triangles);
    if (NewHash != Entry->TriHash)
    {
        return EApplyMode::Normal;
    }

    const double Gap = NowSeconds - Entry->LastArrival;
    if (Gap < JUSyncMorph::MinBlendGapSeconds)
    {
        return EApplyMode::Normal;
    }

    if (!IsMorphCapable(IntentMaterial))
    {
        return EApplyMode::Normal;
    }

    if (Entry->bBlending && Entry->Duration > SMALL_NUMBER)
    {
        const float S = JUSyncMorph::EaseSaturated((NowSeconds - Entry->BlendStart) / Entry->Duration);
        JUSyncMorph::WriteDisplayedPrev(Entry->PrevPoints, Entry->CurrPoints, S,
                                        MeshData.Points.Num(), MeshData.Points, OutPrevPoints);
    }
    else
    {
        JUSyncMorph::WriteDisplayedPrev(TArray<FVector3f>{}, Entry->CurrPoints, 0.f,
                                        MeshData.Points.Num(), MeshData.Points, OutPrevPoints);
    }

    return EApplyMode::Blend;
}

void FJUSYNCMeshMorpher::Commit(
    URealtimeMeshComponent* Comp, const FJUSYNCCompactMeshData& MeshData,
    double NowSeconds, bool bBlended, TArray<FVector3f>&& UploadedPrev)
{
    if (!Comp)
    {
        return;
    }

    FEntry& Entry = Entries.FindOrAdd(Comp);

    const double Gap = NowSeconds - Entry.LastArrival;

    Entry.TriHash = ComputeTriHash(MeshData.Triangles);
    Entry.bHasTriHash = true;
    Entry.CurrPoints = MeshData.Points; // one copy per sample; source of next blend's prev
    Entry.LastArrival = NowSeconds;

    UMaterialInstanceDynamic* MID = ResolveMorphMID(Comp, Entry);

    if (bBlended && MID && Gap > 0.0)
    {
        Entry.PrevPoints = MoveTemp(UploadedPrev);
        const int32 ForcedMs = CVarJUSyncMorphBlendMs.GetValueOnGameThread();
        const double MaxBlend = FMath::Max(CVarJUSyncMorphMaxBlendMs.GetValueOnGameThread(), 8) / 1000.0;
        Entry.Duration = (ForcedMs > 0)
            ? ForcedMs / 1000.0
            : FMath::Clamp(Gap, JUSyncMorph::MinDurationSeconds, FMath::Max(JUSyncMorph::MinDurationSeconds, MaxBlend));
        Entry.BlendStart = NowSeconds;
        Entry.bBlending = true;

        MID->SetScalarParameterValue(MorphActiveParameter, 1.f);
        MID->SetScalarParameterValue(MorphTParameter, 0.f); // hold at prev while the ramp starts
    }
    else
    {
        Entry.bBlending = false;
        Entry.PrevPoints.Reset();
        if (MID)
        {
            MID->SetScalarParameterValue(MorphActiveParameter, 0.f);
            MID->SetScalarParameterValue(MorphTParameter, 1.f);
        }
    }
}

void FJUSYNCMeshMorpher::Seed(URealtimeMeshComponent* Comp, const FJUSYNCCompactMeshData& MeshData, double NowSeconds)
{
    Commit(Comp, MeshData, NowSeconds, /*bBlended*/false, TArray<FVector3f>{});
}

void FJUSYNCMeshMorpher::Forget(URealtimeMeshComponent* Comp)
{
    Entries.Remove(Comp);
}

void FJUSYNCMeshMorpher::Tick(float DeltaTime)
{
    const double Now = FPlatformTime::Seconds();

    for (auto It = Entries.CreateIterator(); It; ++It)
    {
        if (!It.Key().IsValid() || !It.Key().Get())
        {
            It.RemoveCurrent();
            continue;
        }

        FEntry& Entry = It.Value();
        if (!Entry.bBlending)
        {
            continue;
        }

        UMaterialInstanceDynamic* MID = Entry.MorphMID.Get();
        if (!MID)
        {
            Entry.bBlending = false; // material was replaced with a non-MID; stop driving
            continue;
        }

        const double u = (Now - Entry.BlendStart) / Entry.Duration;
        if (u >= 1.0)
        {
            MID->SetScalarParameterValue(MorphTParameter, 1.f); // exact on the new key
            Entry.bBlending = false;
        }
        else
        {
            MID->SetScalarParameterValue(MorphTParameter, JUSyncMorph::EaseSaturated(u));
        }
    }
}
