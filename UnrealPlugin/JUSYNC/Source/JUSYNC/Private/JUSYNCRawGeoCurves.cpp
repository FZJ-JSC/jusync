#include "JUSYNCRawGeoCurves.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "JUSYNCModule.h"

namespace
{
    constexpr int32 GCurveSides = 4;
    constexpr float GCurveDefaultRadius = 0.5f;

    FColor CurveColor(const float* Components, uint32 NumComponents)
    {
        auto Quantize = [](float Value)
        {
            return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value * 255.0f), 0, 255));
        };
        return FColor(
            Quantize(Components[0]),
            Quantize(Components[1]),
            Quantize(Components[2]),
            NumComponents == 4 ? Quantize(Components[3]) : 255);
    }

    // Expand one payload's polylines into side-facing tube triangles. Positions
    // and normals are already in UE space; winding follows the (x,z,-y) rotation
    // so the outward normal matches the ring direction.
    void BuildTubes(const FJUSYNCRawGeoCurvePayload& Payload, FJUSYNCCompactMeshData& Out)
    {
        Out.ElementName = Payload.ElementName;
        Out.TypeName = TEXT("BasisCurves");
        Out.bHasBakedColors = Payload.bHasColors;

        const int32 TotalNodes = Payload.Points.Num();
        if (TotalNodes < 2 || Payload.CurveCounts.Num() == 0)
        {
            return;
        }

        const int32 Sides = GCurveSides;
        const int32 TotalVerts = TotalNodes * Sides;
        Out.Points.SetNumUninitialized(TotalVerts);
        Out.Normals.SetNumUninitialized(TotalVerts);
        if (Payload.bHasColors)
        {
            Out.VertexColors.SetNumUninitialized(TotalVerts);
        }

        TArray<int32> Triangles;
        Triangles.Reserve((TotalNodes - Payload.CurveCounts.Num()) * Sides * 6);

        int32 PointCursor = 0;
        int32 VertCursor = 0;
        for (const int32 CurveNodes : Payload.CurveCounts)
        {
            const int32 CurvePointBase = PointCursor;
            const int32 CurveVertBase = VertCursor;

            for (int32 n = 0; n < CurveNodes; ++n)
            {
                const int32 PointIndex = CurvePointBase + n;
                const FVector3f Position = Payload.Points[PointIndex];

                FVector3f Tangent;
                if (CurveNodes == 1)
                {
                    Tangent = FVector3f(1.0f, 0.0f, 0.0f);
                }
                else if (n == 0)
                {
                    Tangent = Payload.Points[PointIndex + 1] - Position;
                }
                else if (n == CurveNodes - 1)
                {
                    Tangent = Position - Payload.Points[PointIndex - 1];
                }
                else
                {
                    Tangent = Payload.Points[PointIndex + 1] - Payload.Points[PointIndex - 1];
                }
                if (Tangent.SizeSquared() < 1.0e-12f)
                {
                    Tangent = FVector3f(1.0f, 0.0f, 0.0f);
                }
                Tangent = Tangent.GetSafeNormal();

                const FVector3f Reference =
                    FMath::Abs(Tangent.Z) < 0.9f ? FVector3f(0.0f, 0.0f, 1.0f) : FVector3f(0.0f, 1.0f, 0.0f);
                FVector3f Side = FVector3f::CrossProduct(Tangent, Reference).GetSafeNormal();
                if (Side.IsNearlyZero())
                {
                    Side = FVector3f(0.0f, 1.0f, 0.0f);
                }
                FVector3f Up = FVector3f::CrossProduct(Side, Tangent).GetSafeNormal();
                if (Up.IsNearlyZero())
                {
                    Up = FVector3f(0.0f, 0.0f, 1.0f);
                }

                const float Radius = Payload.Widths.Num() == Payload.Points.Num()
                    ? FMath::Max(0.0f, Payload.Widths[PointIndex])
                    : GCurveDefaultRadius;
                const FColor Color = Payload.Colors.Num() == Payload.Points.Num()
                    ? Payload.Colors[PointIndex]
                    : FColor::White;

                for (int32 s = 0; s < Sides; ++s)
                {
                    const float Angle = (2.0f * PI * static_cast<float>(s)) / static_cast<float>(Sides);
                    const FVector3f Direction = Side * FMath::Cos(Angle) + Up * FMath::Sin(Angle);
                    const int32 VertexIndex = CurveVertBase + n * Sides + s;
                    Out.Points[VertexIndex] = Position + Direction * Radius;
                    Out.Normals[VertexIndex] = Direction;
                    if (Out.VertexColors.Num() > 0)
                    {
                        Out.VertexColors[VertexIndex] = Color;
                    }
                }
            }

            for (int32 n = 0; n + 1 < CurveNodes; ++n)
            {
                if ((Payload.Points[CurvePointBase + n + 1] - Payload.Points[CurvePointBase + n]).SizeSquared()
                    < 1.0e-12f)
                {
                    continue;
                }
                const int32 Ring0 = CurveVertBase + n * Sides;
                const int32 Ring1 = Ring0 + Sides;
                for (int32 s = 0; s < Sides; ++s)
                {
                    const int32 s1 = (s + 1) % Sides;
                    Triangles.Add(Ring0 + s);
                    Triangles.Add(Ring1 + s);
                    Triangles.Add(Ring0 + s1);

                    Triangles.Add(Ring0 + s1);
                    Triangles.Add(Ring1 + s);
                    Triangles.Add(Ring1 + s1);
                }
            }

            PointCursor += CurveNodes;
            VertCursor += CurveNodes * Sides;
        }

        Out.Triangles = MoveTemp(Triangles);

        for (FVector3f& Normal : Out.Normals)
        {
            Normal = Normal.GetSafeNormal();
            if (Normal.IsNearlyZero())
            {
                Normal = FVector3f(0.0f, 0.0f, 1.0f);
            }
        }
    }

    void EmitCurveTubes(const FJUSYNCRawGeoCurvePayload& Payload, FJUSYNCCompactMeshRef& OutMesh)
    {
        FJUSYNCCompactMeshRef Mesh = MakeShared<FJUSYNCCompactMeshData, ESPMode::ThreadSafe>();
        BuildTubes(Payload, *Mesh);
        OutMesh = MoveTemp(Mesh);
    }

    constexpr double GCurveMergeMaxAgeSeconds = 30.0;    constexpr int64 GCurveMergeDefaultBudgetKPoints = 16384;
    constexpr int32 GCurveMaxSubsets = 8;

    TAutoConsoleVariable<int32> CVarJusyncCurveProgressive(
        TEXT("jusync.RawGeo.CurveProgressive"), 2,
        TEXT("RawGeo v3 curve refinement policy. 2 (default) = coarse subset 0 now, full mesh on completion; 1 = also emit after every subset; 0 = coarse only, drop refinements."),
        ECVF_Default);

    // One frame's curve lattice. Curves are stored per ordinal (not per subset
    // point), so concatenating in ordinal order is the same geometry a single
    // S=1 payload carries.
    struct FJUSYNCRawGeoCurveMerge
    {
        int64 SubsetCount = 0;
        int64 FullCurveCount = 0;
        int64 FullNumPoints = 0;
        int64 Filled = 0;
        double Time = 0.0;
        double LastUseSeconds = 0.0;
        FString PrimName;
        bool bHasColors = false;
        bool bHasWidths = false;
        TArray<uint8> FilledSlots;
        TArray<TArray<FVector3f>> Points;
        TArray<TArray<FColor>> Colors;
        TArray<TArray<float>> Widths;
    };

    FCriticalSection GRawGeoCurveMergeLock;
    TMap<FString, FJUSYNCRawGeoCurveMerge> GRawGeoCurveMerges;
    int64 GRawGeoCurveMergeTotalPoints = 0;
    TSet<FString> GRawGeoCurveWarnedKeys;

    int64 CurveMergeBudgetPoints()
    {
        if (const IConsoleVariable* Var =
            IConsoleManager::Get().FindConsoleVariable(TEXT("jusync.RawGeo.RefineBudgetKPoints")))
        {
            return FMath::Max<int64>(1024, static_cast<int64>(Var->GetInt()) * 1024);
        }
        return GCurveMergeDefaultBudgetKPoints * 1024;
    }

    // Caller holds the merge lock.
    void PruneCurveMergesLocked(double Now)
    {
        for (auto It = GRawGeoCurveMerges.CreateIterator(); It; ++It)
        {
            if (Now - It.Value().LastUseSeconds > GCurveMergeMaxAgeSeconds)
            {
                GRawGeoCurveMergeTotalPoints -= It.Value().FullNumPoints;
                It.RemoveCurrent();
            }
        }
    }

    // Caller holds the merge lock. Drops least-recently-used frames until the
    // new one fits; false when the stream alone is over the shared budget.
    bool CurveMergeKeyFitsBudgetLocked(int64 FullNumPoints)
    {
        const int64 Budget = CurveMergeBudgetPoints();
        if (FullNumPoints > Budget)
        {
            return false;
        }
        while (GRawGeoCurveMergeTotalPoints + FullNumPoints > Budget && GRawGeoCurveMerges.Num() > 0)
        {
            FString OldestKey;
            double Oldest = TNumericLimits<double>::Max();
            for (const TPair<FString, FJUSYNCRawGeoCurveMerge>& Pair : GRawGeoCurveMerges)
            {
                if (Pair.Value.LastUseSeconds < Oldest)
                {
                    Oldest = Pair.Value.LastUseSeconds;
                    OldestKey = Pair.Key;
                }
            }
            if (OldestKey.IsEmpty())
            {
                break;
            }
            if (const FJUSYNCRawGeoCurveMerge* Victim = GRawGeoCurveMerges.Find(OldestKey))
            {
                GRawGeoCurveMergeTotalPoints -= Victim->FullNumPoints;
            }
            GRawGeoCurveMerges.Remove(OldestKey);
        }
        return GRawGeoCurveMergeTotalPoints + FullNumPoints <= Budget;
    }

    bool ScatterCurveSubset(FJUSYNCRawGeoCurveMerge& Merge, const FJUSYNCRawGeoCurvePayload& Payload)
    {
        const int32 Subset = static_cast<int32>(Payload.SubsetIndex);
        const int32 Count = static_cast<int32>(Payload.SubsetCount);
        if (!Merge.FilledSlots.IsValidIndex(Subset))
        {
            return false;
        }

        int32 Start = 0;
        for (int32 i = 0; i < Payload.CurveCounts.Num(); ++i)
        {
            const int64 Ordinal = static_cast<int64>(Subset) + static_cast<int64>(i) * Count;
            if (Ordinal < 0 || Ordinal >= Merge.FullCurveCount)
            {
                return false;
            }
            const int32 Length = Payload.CurveCounts[i];
            if (Length < 0 || Start + Length > Payload.Points.Num())
            {
                return false;
            }

            TArray<FVector3f>& DstPoints = Merge.Points[static_cast<int32>(Ordinal)];
            DstPoints.Reset(Length);
            DstPoints.Append(Payload.Points.GetData() + Start, Length);

            if (Merge.bHasColors)
            {
                TArray<FColor>& DstColors = Merge.Colors[static_cast<int32>(Ordinal)];
                DstColors.Reset(Length);
                if (Payload.bHasColors && Payload.Colors.Num() >= Start + Length)
                {
                    DstColors.Append(Payload.Colors.GetData() + Start, Length);
                }
                else
                {
                    DstColors.Init(FColor::White, Length);
                }
            }
            if (Merge.bHasWidths)
            {
                TArray<float>& DstWidths = Merge.Widths[static_cast<int32>(Ordinal)];
                DstWidths.Reset(Length);
                if (Payload.bHasWidths && Payload.Widths.Num() >= Start + Length)
                {
                    DstWidths.Append(Payload.Widths.GetData() + Start, Length);
                }
                else
                {
                    DstWidths.Init(GCurveDefaultRadius, Length);
                }
            }
            Start += Length;
        }

        if (Merge.FilledSlots[Subset] == 0)
        {
            Merge.FilledSlots[Subset] = 1;
            ++Merge.Filled;
        }
        return true;
    }

    // Concatenates every filled ordinal in order, so an incomplete merge (policy
    // 1) shows the curves that have arrived without holes in the middle.
    void BuildMergedCurvePayload(const FJUSYNCRawGeoCurveMerge& Merge, FJUSYNCRawGeoCurvePayload& Out)
    {
        Out.ElementName = Merge.PrimName;
        Out.Time = Merge.Time;
        Out.SubsetIndex = 0;
        Out.SubsetCount = Merge.SubsetCount;
        Out.FullCurveCount = Merge.FullCurveCount;
        Out.FullNumPoints = Merge.FullNumPoints;
        Out.bHasColors = Merge.bHasColors;
        Out.bHasWidths = Merge.bHasWidths;

        for (int64 Curve = 0; Curve < Merge.FullCurveCount; ++Curve)
        {
            const int64 Slot = Curve % Merge.SubsetCount;
            if (!Merge.FilledSlots.IsValidIndex(static_cast<int32>(Slot)) ||
                Merge.FilledSlots[static_cast<int32>(Slot)] == 0)
            {
                continue;
            }
            const TArray<FVector3f>& CurvePoints = Merge.Points[static_cast<int32>(Curve)];
            if (CurvePoints.Num() < 2)
            {
                continue;
            }
            Out.CurveCounts.Add(CurvePoints.Num());
            Out.Points.Append(CurvePoints);
            if (Out.bHasColors && Merge.Colors.IsValidIndex(static_cast<int32>(Curve)))
            {
                Out.Colors.Append(Merge.Colors[static_cast<int32>(Curve)]);
            }
            if (Out.bHasWidths && Merge.Widths.IsValidIndex(static_cast<int32>(Curve)))
            {
                Out.Widths.Append(Merge.Widths[static_cast<int32>(Curve)]);
            }
        }
    }
}

namespace JUSYNCRawGeoCurves
{
    bool ParsePayload(const uint8* Data, SIZE_T Size,
                      const anari_usd_protocol::JusyncRawGeoHeader& Hdr,
                      FJUSYNCRawGeoCurvePayload& Out)
    {
        using namespace anari_usd_protocol;

        if (Data == nullptr ||
            Hdr.version != JUSYNC_RAWGEO_VERSION ||
            Hdr.topology != JUSYNC_RAWGEO_TOPOLOGY_LINEAR_CURVES)
        {
            return false;
        }
        if (Hdr.num_attributes > 16 || Hdr.subset_count == 0)
        {
            return false;
        }

        const uint64 HeaderBytes = static_cast<uint64>(JusyncRawGeoHeader::header_bytes(Hdr.version));
        const uint64 DescBytes = static_cast<uint64>(Hdr.num_attributes) * sizeof(JusyncRawGeoAttrDesc);
        if (static_cast<uint64>(Size) < HeaderBytes + DescBytes)
        {
            return false;
        }

        const JusyncRawGeoAttrDesc* Descs =
            reinterpret_cast<const JusyncRawGeoAttrDesc*>(Data + HeaderBytes);
        uint64 AttrComponentsPerPoint = 0;
        for (uint32 a = 0; a < Hdr.num_attributes; ++a)
        {
            if (Descs[a].components < 1 || Descs[a].components > 4)
            {
                return false;
            }
            AttrComponentsPerPoint += Descs[a].components;
        }

        const uint64 N = Hdr.num_points;
        const uint64 PosOffset = HeaderBytes + DescBytes;
        const uint64 ColOffset = PosOffset + N * 3 * sizeof(float);
        const uint64 AttrOffset = ColOffset + N * Hdr.color_components * sizeof(float);
        const uint64 CurveOffset = AttrOffset + N * AttrComponentsPerPoint * sizeof(float);
        const uint64 WidthOffset = CurveOffset + static_cast<uint64>(Hdr.curve_count) * sizeof(uint32);
        const uint64 End = WidthOffset + N * Hdr.width_components * sizeof(float);
        if (static_cast<uint64>(Size) < End)
        {
            return false;
        }

        Out.Points.SetNumUninitialized(static_cast<int32>(N));
        const float* Positions = reinterpret_cast<const float*>(Data + PosOffset);
        for (uint64 i = 0; i < N; ++i)
        {
            const float* P = Positions + i * 3;
            Out.Points[static_cast<int32>(i)] = FVector3f(P[0], P[2], -P[1]);
        }

        if (Hdr.color_components != 0)
        {
            Out.Colors.SetNumUninitialized(static_cast<int32>(N));
            const float* Colors = reinterpret_cast<const float*>(Data + ColOffset);
            for (uint64 i = 0; i < N; ++i)
            {
                Out.Colors[static_cast<int32>(i)] =
                    CurveColor(Colors + i * Hdr.color_components, Hdr.color_components);
            }
        }

        Out.CurveCounts.SetNumUninitialized(static_cast<int32>(Hdr.curve_count));
        const uint32* Counts = reinterpret_cast<const uint32*>(Data + CurveOffset);
        uint64 PointSum = 0;
        for (uint32 c = 0; c < Hdr.curve_count; ++c)
        {
            if (Counts[c] < 2)
            {
                return false;
            }
            Out.CurveCounts[static_cast<int32>(c)] = static_cast<int32>(Counts[c]);
            PointSum += Counts[c];
        }
        if (PointSum != N)
        {
            return false;
        }

        if (Hdr.width_components != 0)
        {
            Out.Widths.SetNumUninitialized(static_cast<int32>(N));
            const float* Widths = reinterpret_cast<const float*>(Data + WidthOffset);
            for (uint64 i = 0; i < N; ++i)
            {
                Out.Widths[static_cast<int32>(i)] = Widths[i];
            }
        }

        Out.ElementName = UTF8_TO_TCHAR(Hdr.prim_name);
        Out.Time = Hdr.time;
        Out.SubsetIndex = static_cast<int64>(Hdr.subset_index);
        Out.SubsetCount = static_cast<int64>(Hdr.subset_count);
        Out.FullCurveCount = static_cast<int64>(Hdr.full_curve_count);
        Out.FullNumPoints = static_cast<int64>(Hdr.full_num_points);
        Out.bHasColors = Hdr.color_components != 0;
        Out.bHasWidths = Hdr.width_components != 0;
        return true;
    }

    bool IngestAndBuild(const TArray<uint8>& Buffer, const FString& Filename,
                        const anari_usd_protocol::JusyncRawGeoHeader& Hdr,
                        FJUSYNCCompactMeshRef& OutMesh)
    {
        OutMesh.Reset();

        FJUSYNCRawGeoCurvePayload Payload;
        if (!ParsePayload(Buffer.GetData(), static_cast<SIZE_T>(Buffer.Num()), Hdr, Payload))
        {
            UE_LOG(LogJUSYNC, Warning,
                TEXT("RawGeo curve payload rejected: '%s' (%d bytes, %llu curves)"),
                UTF8_TO_TCHAR(Hdr.prim_name), Buffer.Num(), static_cast<unsigned long long>(Hdr.curve_count));
            return false;
        }

        const int64 Subset = Payload.SubsetIndex;
        const int64 Count = FMath::Max<int64>(1, Payload.SubsetCount);
        const int64 Curves = Payload.FullCurveCount;
        if (Subset < 0 || Subset >= Count || Count > GCurveMaxSubsets || Curves < 0 ||
            static_cast<int64>(Payload.CurveCounts.Num()) > Curves)
        {
            return false;
        }

        const int32 Policy = CVarJusyncCurveProgressive.GetValueOnAnyThread();
        if (Policy == 0)
        {
            // Coarse-only mode: keep subset 0, drop every refinement.
            if (Subset == 0)
            {
                EmitCurveTubes(Payload, OutMesh);
            }
            return true;
        }

        if (Count == 1)
        {
            EmitCurveTubes(Payload, OutMesh);
            return true;
        }

        const double Now = FPlatformTime::Seconds();
        const FString Key = StripSubsetSuffix(Filename);

        FScopeLock Lock(&GRawGeoCurveMergeLock);
        PruneCurveMergesLocked(Now);

        FJUSYNCRawGeoCurveMerge* Merge = GRawGeoCurveMerges.Find(Key);
        if (Merge && (Merge->SubsetCount != Count || Merge->FullCurveCount != Curves))
        {
            if (!GRawGeoCurveWarnedKeys.Contains(Key))
            {
                GRawGeoCurveWarnedKeys.Add(Key);
                UE_LOG(LogJUSYNC, Warning,
                    TEXT("RawGeo curve merge '%s' restarted: subset metadata changed (S %d->%d, C %d->%d)"),
                    *Key, static_cast<int32>(Merge->SubsetCount), static_cast<int32>(Count),
                    static_cast<int32>(Merge->FullCurveCount), static_cast<int32>(Curves));
            }
            GRawGeoCurveMergeTotalPoints -= Merge->FullNumPoints;
            GRawGeoCurveMerges.Remove(Key);
            Merge = nullptr;
        }

        if (Merge == nullptr)
        {
            if (!CurveMergeKeyFitsBudgetLocked(Payload.FullNumPoints))
            {
                // Over the shared budget: show this frame's coarse subset and
                // let its refinements fall away.
                if (Subset == 0)
                {
                    EmitCurveTubes(Payload, OutMesh);
                }
                return true;
            }

            FJUSYNCRawGeoCurveMerge NewMerge;
            NewMerge.SubsetCount = Count;
            NewMerge.FullCurveCount = Curves;
            NewMerge.FullNumPoints = Payload.FullNumPoints;
            NewMerge.Time = Payload.Time;
            NewMerge.LastUseSeconds = Now;
            NewMerge.PrimName = Payload.ElementName;
            NewMerge.bHasColors = Payload.bHasColors;
            NewMerge.bHasWidths = Payload.bHasWidths;
            NewMerge.FilledSlots.SetNumZeroed(static_cast<int32>(Count));
            NewMerge.Points.SetNum(static_cast<int32>(Curves));
            NewMerge.Colors.SetNum(static_cast<int32>(Curves));
            NewMerge.Widths.SetNum(static_cast<int32>(Curves));
            Merge = &GRawGeoCurveMerges.Add(Key, MoveTemp(NewMerge));
            GRawGeoCurveMergeTotalPoints += Merge->FullNumPoints;
        }

        Merge->LastUseSeconds = Now;

        if (!ScatterCurveSubset(*Merge, Payload))
        {
            GRawGeoCurveMergeTotalPoints -= Merge->FullNumPoints;
            GRawGeoCurveMerges.Remove(Key);
            return false;
        }

        const bool bComplete = Merge->Filled == Merge->SubsetCount;
        const bool bEmit = (Policy == 1) || (Subset == 0) || bComplete;
        if (bEmit)
        {
            FJUSYNCRawGeoCurvePayload Merged;
            BuildMergedCurvePayload(*Merge, Merged);
            EmitCurveTubes(Merged, OutMesh);
        }

        if (bComplete)
        {
            GRawGeoCurveMergeTotalPoints -= Merge->FullNumPoints;
            GRawGeoCurveMerges.Remove(Key);
        }
        return true;
    }

    FString StripSubsetSuffix(const FString& Filename)
    {
        int32 DotIndex = INDEX_NONE;
        FString Base = Filename;
        if (Base.FindLastChar(TEXT('.'), DotIndex))
        {
            Base = Base.Left(DotIndex);
        }

        int32 UnderscoreIndex = INDEX_NONE;
        if (!Base.FindLastChar(TEXT('_'), UnderscoreIndex))
        {
            return Base;
        }
        if (UnderscoreIndex + 1 >= Base.Len() || Base[UnderscoreIndex + 1] != TEXT('s'))
        {
            return Base;
        }

        const FString Digits = Base.Mid(UnderscoreIndex + 2);
        if (Digits.IsEmpty())
        {
            return Base;
        }
        for (const TCHAR Ch : Digits)
        {
            if (!FChar::IsDigit(Ch))
            {
                return Base;
            }
        }
        return Base.Left(UnderscoreIndex);
    }

    void FlushMerges()
    {
        FScopeLock Lock(&GRawGeoCurveMergeLock);
        GRawGeoCurveMerges.Empty();
        GRawGeoCurveMergeTotalPoints = 0;
        GRawGeoCurveWarnedKeys.Empty();
    }
}
