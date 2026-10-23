#include "JUSYNCMeshMorpher.h"

#include "Async/ParallelFor.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Hash/CityHash.h"
#include "JUSYNCModule.h"
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

// Auto mode follows the measured sample gap so a slow stream still gets a
// full-length blend. At interactive rates that gap is the *update* interval
// (often 0.5 s because the scene diff polls), which leaves the mesh one whole
// update behind on screen. Keep auto snappy by default; raise this if you want
// slow samples to blend for their whole gap.
static TAutoConsoleVariable<int32> CVarJUSyncMorphAutoMaxMs(
    TEXT("jusync.Morph.MeshAutoMaxMs"), 200,
    TEXT("Cap in ms for the auto mesh blend when MeshBlendMs is 0 (default 200)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJUSyncMorphTopologyMatch(
    TEXT("jusync.Morph.MeshTopologyMatch"), 1,
    TEXT("1 = morph across samples whose triangle topology changed (sender re-meshes every timestep)")
    TEXT(" by matching vertices to the nearest previous-sample vertex; 0 = identical-topology only."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJUSyncMorphMatchMaxVerts(
    TEXT("jusync.Morph.MeshMatchMaxVerts"), 3000000,
    TEXT("Vertex budget per side for cross-topology matching (game thread); bigger meshes instant-apply."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarJUSyncMorphMatchMinPct(
    TEXT("jusync.Morph.MeshMatchMinPct"), 40,
    TEXT("Minimum percentage of new vertices that must find a previous-sample match for a")
    TEXT(" cross-topology morph; below this the update instant-applies."),
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

    // Cross-topology correspondence: matches every vertex of the new sample to the nearest
    // vertex of the previous sample. Uniform grid stored as a sorted (cellKey, vertexIndex)
    // array; key computation and queries are parallel, queries binary-search the
    // 27-cell neighborhood.
    class FVertMatcher
    {
    public:
        FVertMatcher(const TArray<FVector3f>& Old, float MinAcceptRadius)
        {
            Points = &Old;
            FVector3f Min = Old[0], Max = Old[0];
            for (const FVector3f& P : Old)
            {
                Min = FVector3f::Min(Min, P);
                Max = FVector3f::Max(Max, P);
            }
            const FVector3f Extent = Max - Min;
            // Cell ~= local point spacing so a 3x3x3 probe reaches the nearest old vertex
            // for mesh drift well under one spacing.
            const float Volume = FMath::Max(Extent.X, 1e-3f)
                               * FMath::Max(Extent.Y, 1e-3f)
                               * FMath::Max(Extent.Z, 1e-3f);
            CellSize = FMath::Max(FMath::Pow(Volume / FMath::Max(Old.Num(), 1), 1.f / 3.f) * 1.25f,
                                  FMath::Max(MinAcceptRadius, 1e-3f));
            AcceptRadiusSq = FMath::Square(CellSize * 2.0f);

            Keyed.SetNumUninitialized(Old.Num());
            ParallelFor(Old.Num(), [&](int32 Idx)
            {
                Keyed[Idx] = MakeTuple(CellKey(Old[Idx]), Idx);
            });
            Keyed.Sort([](const TPair<uint64, int32>& A, const TPair<uint64, int32>& B)
            {
                return A.Key != B.Key ? A.Key < B.Key : A.Value < B.Value;
            });
        }

        // Returns old-vertex index or INDEX_NONE.
        int32 FindNearest(const FVector3f& P) const
        {
            const FIntVector C(
                FMath::FloorToInt(P.X / CellSize),
                FMath::FloorToInt(P.Y / CellSize),
                FMath::FloorToInt(P.Z / CellSize));

            int32 Best = INDEX_NONE;
            float BestDistSq = AcceptRadiusSq;
            for (int32 dz = -1; dz <= 1; ++dz)
            {
                for (int32 dy = -1; dy <= 1; ++dy)
                {
                    for (int32 dx = -1; dx <= 1; ++dx)
                    {
                        const uint64 Key = Mix(C.X + dx, C.Y + dy, C.Z + dz);

                        int32 Lo = 0;
                        int32 Hi = Keyed.Num(); // lower bound: first entry with Key >= Key
                        while (Lo < Hi)
                        {
                            const int32 Mid = (Lo + Hi) / 2;
                            if (Keyed[Mid].Key < Key)
                            {
                                Lo = Mid + 1;
                            }
                            else
                            {
                                Hi = Mid;
                            }
                        }
                        const int32 Begin = Lo;

                        Hi = Keyed.Num(); // upper bound: first entry with Key > Key
                        while (Lo < Hi)
                        {
                            const int32 Mid = (Lo + Hi) / 2;
                            if (Keyed[Mid].Key <= Key)
                            {
                                Lo = Mid + 1;
                            }
                            else
                            {
                                Hi = Mid;
                            }
                        }
                        // Cap the scan at MaxBucket entries per cell.
                        const int32 End = FMath::Min(Lo, Begin + MaxBucket);

                        for (int32 k = Begin; k < End; ++k)
                        {
                            const int32 Idx = Keyed[k].Value;
                            const float DistSq = FVector3f::DistSquared((*Points)[Idx], P);
                            if (DistSq <= BestDistSq)
                            {
                                BestDistSq = DistSq;
                                Best = Idx;
                            }
                        }
                    }
                }
            }
            return Best;
        }

    private:
        static uint64 Mix(int32 ix, int32 iy, int32 iz)
        {
            uint64 h = static_cast<uint32>(ix);
            h = h * 0x9E3779B97F4A7C15ull + static_cast<uint32>(iy);
            h = h * 0xC2B2AE3D27D4EB4Full + static_cast<uint32>(iz);
            return h;
        }

        uint64 CellKey(const FVector3f& P) const
        {
            return Mix(
                FMath::FloorToInt(P.X / CellSize),
                FMath::FloorToInt(P.Y / CellSize),
                FMath::FloorToInt(P.Z / CellSize));
        }

        static constexpr int32 MaxBucket = 32;
        const TArray<FVector3f>* Points = nullptr;
        float CellSize = 1.f;
        float AcceptRadiusSq = 1.f;
        TArray<TPair<uint64, int32>> Keyed;
    };

    // Out[i] = matched previous position for new vertex i (own position when unmatched,
    // so the vertex simply does not travel). Returns matched fraction of new vertices.
    static float BuildMatchedPrev(const TArray<FVector3f>& Old, const TArray<FVector3f>& New,
                                  TArray<FVector3f>& Out, int32& OutMatched)
    {
        FVertMatcher Matcher(Old, 0.f);

        Out.SetNumUninitialized(New.Num());
        FThreadSafeCounter Matched(0);
        ParallelFor(New.Num(), [&](int32 i)
        {
            const int32 Src = Matcher.FindNearest(New[i]);
            if (Src != INDEX_NONE)
            {
                Out[i] = Old[Src];
                Matched.Increment();
            }
            else
            {
                Out[i] = New[i];
            }
        });
        OutMatched = Matched.GetValue();
        return New.Num() > 0 ? static_cast<float>(OutMatched) / New.Num() : 0.f;
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

bool FJUSYNCMeshMorpher::IsMaterialMorphCapable(const UMaterialInterface* Material)
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
        if (IsMaterialMorphCapable(CurrentMID))
        {
            Entry.MorphMID = CurrentMID;
            return CurrentMID;
        }
        Entry.MorphMID.Reset();
        return nullptr;
    }

    // Static material that has the parameters -> create our own MID.

    if (IsMaterialMorphCapable(Current))
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

    const double Gap = NowSeconds - Entry->LastArrival;
    if (Gap < JUSyncMorph::MinBlendGapSeconds)
    {
        return EApplyMode::Normal;
    }

    if (!IsMaterialMorphCapable(IntentMaterial))
    {
        return EApplyMode::Normal;
    }

    const uint64 NewHash = ComputeTriHash(MeshData.Triangles);
    const bool bIndexAligned = (NewHash == Entry->TriHash);

    if (!bIndexAligned)
    {
        // The ANARI-USD sender writes points AND face indices as time-varying attributes,
        // so a re-meshed timestep changes topology. Fall back to nearest-vertex matching.
        if (!CVarJUSyncMorphTopologyMatch.GetValueOnGameThread())
        {
            return EApplyMode::Normal;
        }
        const int32 Budget = FMath::Max(CVarJUSyncMorphMatchMaxVerts.GetValueOnGameThread(), 1024);
        if (Entry->CurrPoints.Num() > Budget || MeshData.Points.Num() > Budget)
        {
            UE_LOG(LogJUSYNC, Verbose, TEXT("MeshMorph '%s': %d/%d verts over match budget %d, instant apply"),
                   *MeshData.ElementName, MeshData.Points.Num(), Entry->CurrPoints.Num(), Budget);
            return EApplyMode::Normal;
        }

        int32 Matched = 0;
        const float MatchedFrac = JUSyncMorph::BuildMatchedPrev(
            Entry->CurrPoints, MeshData.Points, OutPrevPoints, Matched);
        const int32 MinPct = CVarJUSyncMorphMatchMinPct.GetValueOnGameThread();
        if (MatchedFrac * 100.f < static_cast<float>(MinPct))
        {
            OutPrevPoints.Reset();
            UE_LOG(LogJUSYNC, Verbose, TEXT("MeshMorph '%s': topology changed, match %.1f%% < %d%%, instant apply"),
                   *MeshData.ElementName, MatchedFrac * 100.f, MinPct);
            return EApplyMode::Normal;
        }

        // NOTE: a mid-blend arrival restarts the morph from the previous SAMPLE's matched
        // positions instead of the in-flight displayed ones; at streaming rates the visible
        // error is sub-frame.
        Entry->bBlendByMatch = true;

        static double LastMatchLogTime = 0.0;
        if (NowSeconds - LastMatchLogTime > 2.0)
        {
            LastMatchLogTime = NowSeconds;
            UE_LOG(LogJUSYNC, Log, TEXT("MeshMorph '%s': cross-topology morph, matched %d/%d verts (%.1f%%)"),
                   *MeshData.ElementName, Matched, MeshData.Points.Num(), MatchedFrac * 100.f);
        }
        return EApplyMode::Blend;
    }

    Entry->bBlendByMatch = false;
    if (Entry->bBlending && Entry->Duration > SMALL_NUMBER)
    {
        const float S = JUSyncMorph::EaseSaturated((NowSeconds - Entry->BlendStart) / Entry->Duration);
        JUSyncMorph::WriteDisplayedPrev(Entry->PrevPoints, Entry->CurrPoints, S,
                                        MeshData.Points.Num(), MeshData.Points, OutPrevPoints);
    }
    else
    {
        // No blend in flight: the displayed pose is exactly the previous sample. Start the
        // new blend from Entry->CurrPoints. (Passing an empty FromA here made the overlap
        // zero and filled Out with the NEW points instead, producing a zero delta and an
        // instant snap for every sample that arrived after the previous ramp had finished.)
        JUSyncMorph::WriteDisplayedPrev(Entry->CurrPoints, Entry->CurrPoints, 0.f,
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
        const double AutoMax = FMath::Clamp(
            CVarJUSyncMorphAutoMaxMs.GetValueOnGameThread(), 8,
            FMath::Max(8, CVarJUSyncMorphMaxBlendMs.GetValueOnGameThread())) / 1000.0;
        Entry.Duration = (ForcedMs > 0)
            ? FMath::Max(ForcedMs, 8) / 1000.0
            : FMath::Clamp(Gap, JUSyncMorph::MinDurationSeconds, FMath::Max(JUSyncMorph::MinDurationSeconds, FMath::Min(MaxBlend, AutoMax)));
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
