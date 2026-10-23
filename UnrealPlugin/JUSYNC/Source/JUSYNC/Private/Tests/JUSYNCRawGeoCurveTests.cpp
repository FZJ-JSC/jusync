#include "Misc/AutomationTest.h"

#include "HAL/IConsoleManager.h"
#include "RawGeo/JUSYNCRawGeoCurves.h"
#include "JUSYNCElementNames.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    TArray<uint8> MakeCurveBuffer(const TArray<TArray<FVector3f>>& Curves,
                                  int32 SubsetIndex, int32 SubsetCount,
                                  const TCHAR* PrimName = TEXT("Streamlines"))
    {
        TArray<int32> Selected;
        for (int32 Curve = SubsetIndex; Curve < Curves.Num(); Curve += SubsetCount)
        {
            Selected.Add(Curve);
        }

        int32 NumPoints = 0;
        int32 FullNumPoints = 0;
        for (const TArray<FVector3f>& Curve : Curves)
        {
            FullNumPoints += Curve.Num();
        }
        for (const int32 Curve : Selected)
        {
            NumPoints += Curves[Curve].Num();
        }

        const int32 PayloadBytes =
            NumPoints * 3 * static_cast<int32>(sizeof(float))
            + NumPoints * 3 * static_cast<int32>(sizeof(float))
            + Selected.Num() * static_cast<int32>(sizeof(uint32))
            + NumPoints * static_cast<int32>(sizeof(float));

        TArray<uint8> Buffer;
        Buffer.SetNumZeroed(static_cast<int32>(sizeof(anari_usd_protocol::JusyncRawGeoHeader)) + PayloadBytes);

        anari_usd_protocol::JusyncRawGeoHeader Header{};
        Header.magic = anari_usd_protocol::JUSYNC_RAWGEO_MAGIC;
        Header.version = anari_usd_protocol::JUSYNC_RAWGEO_VERSION;
        FCStringAnsi::Strncpy(Header.prim_name, TCHAR_TO_UTF8(PrimName), UE_ARRAY_COUNT(Header.prim_name) - 1);
        Header.num_points = NumPoints;
        Header.time = 0.0;
        Header.position_components = 3;
        Header.color_components = 3;
        Header.num_attributes = 0;
        Header.attr_uniform_count = NumPoints;
        Header.payload_bytes = PayloadBytes;
        Header.subset_index = SubsetIndex;
        Header.subset_count = SubsetCount;
        Header.full_num_points = FullNumPoints;
        Header.topology = anari_usd_protocol::JUSYNC_RAWGEO_TOPOLOGY_LINEAR_CURVES;
        Header.curve_count = Selected.Num();
        Header.full_curve_count = Curves.Num();
        Header.width_components = 1;
        Header.flags = 0;

        FMemory::Memcpy(Buffer.GetData(), &Header, sizeof(Header));
        uint8* Cursor = Buffer.GetData() + sizeof(Header);

        for (const int32 Curve : Selected)
        {
            for (const FVector3f& P : Curves[Curve])
            {
                const float Values[3] = { P.X, P.Y, P.Z };
                FMemory::Memcpy(Cursor, Values, sizeof(Values));
                Cursor += sizeof(Values);
            }
        }
        for (int32 i = 0; i < NumPoints; ++i)
        {
            const float Values[3] = { 1.0f, 0.5f, 0.25f };
            FMemory::Memcpy(Cursor, Values, sizeof(Values));
            Cursor += sizeof(Values);
        }
        for (const int32 Curve : Selected)
        {
            const uint32 Count = static_cast<uint32>(Curves[Curve].Num());
            FMemory::Memcpy(Cursor, &Count, sizeof(Count));
            Cursor += sizeof(Count);
        }
        for (int32 i = 0; i < NumPoints; ++i)
        {
            const float Width = 0.1f;
            FMemory::Memcpy(Cursor, &Width, sizeof(Width));
            Cursor += sizeof(Width);
        }
        return Buffer;
    }

    bool ReadHeader(const TArray<uint8>& Buffer, anari_usd_protocol::JusyncRawGeoHeader& Out)
    {
        if (Buffer.Num() < static_cast<int32>(sizeof(Out)))
        {
            return false;
        }
        FMemory::Memcpy(&Out, Buffer.GetData(), sizeof(Out));
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoCurveSingleTest, "JUSYNC.RawGeo.CurveSingle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoCurveSingleTest::RunTest(const FString& Parameters)
{
    JUSYNCRawGeoCurves::FlushMerges();

    TArray<TArray<FVector3f>> Curves;
    Curves.Add({ FVector3f(1.0f, 2.0f, 3.0f), FVector3f(2.0f, 2.0f, 4.0f), FVector3f(3.0f, 2.0f, 5.0f) });
    Curves.Add({ FVector3f(4.0f, 0.0f, 0.0f), FVector3f(5.0f, 0.0f, 1.0f) });
    Curves.Add({ FVector3f(6.0f, 1.0f, 1.0f), FVector3f(7.0f, 1.0f, 2.0f),
                 FVector3f(8.0f, 1.0f, 3.0f), FVector3f(9.0f, 1.0f, 4.0f) });

    TArray<uint8> Buffer = MakeCurveBuffer(Curves, 0, 1);
    anari_usd_protocol::JusyncRawGeoHeader Header{};
    TestTrue(TEXT("header readable"), ReadHeader(Buffer, Header));

    FJUSYNCCompactMeshRef Mesh;
    TestTrue(TEXT("payload ingests"),
        JUSYNCRawGeoCurves::IngestAndBuild(Buffer, TEXT("clips/Wave_Geom__0.geo"), Header, Mesh));
    TestTrue(TEXT("mesh produced"), Mesh.IsValid());
    if (!Mesh.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("element name"), Mesh->ElementName, FString(TEXT("Streamlines")));
    TestEqual(TEXT("type name"), Mesh->TypeName, FString(TEXT("BasisCurves")));
    TestEqual(TEXT("vertex count"), Mesh->Points.Num(), 9 * 4);
    TestEqual(TEXT("triangle count"), Mesh->Triangles.Num() / 3, (2 + 1 + 3) * 4 * 2);
    TestTrue(TEXT("has normals"), Mesh->HasNormals());
    TestTrue(TEXT("has baked colors"), Mesh->bHasBakedColors);

    for (const FVector3f& Normal : Mesh->Normals)
    {
        TestTrue(TEXT("unit normal"), FMath::IsNearlyEqual(Normal.Size(), 1.0f, 1.0e-3f));
    }

    // The first four vertices form the tube ring around the first curve node,
    // so their centroid is that node after the (x, y, z) -> (x, z, -y) map.
    const FVector3f RingCenter =
        (Mesh->Points[0] + Mesh->Points[1] + Mesh->Points[2] + Mesh->Points[3]) * 0.25f;
    TestTrue(TEXT("positions mapped to UE space"), RingCenter.Equals(FVector3f(1.0f, 3.0f, -2.0f)));
    for (int32 s = 0; s < 4; ++s)
    {
        const FVector3f Radial = (Mesh->Points[s] - RingCenter).GetSafeNormal();
        TestTrue(TEXT("outward ring normal"), FVector3f::DotProduct(Mesh->Normals[s], Radial) > 0.9f);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoCurveDegenerateTest, "JUSYNC.RawGeo.CurveDegenerate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoCurveDegenerateTest::RunTest(const FString& Parameters)
{
    JUSYNCRawGeoCurves::FlushMerges();

    TArray<TArray<FVector3f>> Curves;
    Curves.Add({ FVector3f(1.0f, 1.0f, 1.0f), FVector3f(1.0f, 1.0f, 1.0f) });

    TArray<uint8> Buffer = MakeCurveBuffer(Curves, 0, 1);
    anari_usd_protocol::JusyncRawGeoHeader Header{};
    TestTrue(TEXT("header readable"), ReadHeader(Buffer, Header));

    FJUSYNCCompactMeshRef Mesh;
    TestTrue(TEXT("degenerate payload ingests"),
        JUSYNCRawGeoCurves::IngestAndBuild(Buffer, TEXT("clips/Degenerate_Geom__0.geo"), Header, Mesh));
    TestTrue(TEXT("mesh produced"), Mesh.IsValid());
    if (!Mesh.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("ring vertices"), Mesh->Points.Num(), 2 * 4);
    TestEqual(TEXT("no band triangles"), Mesh->Triangles.Num(), 0);
    for (const FVector3f& Normal : Mesh->Normals)
    {
        TestTrue(TEXT("finite unit normal"), FMath::IsNearlyEqual(Normal.Size(), 1.0f, 1.0e-3f));
        TestTrue(TEXT("normal is finite"), FMath::IsFinite(Normal.X) && FMath::IsFinite(Normal.Y) && FMath::IsFinite(Normal.Z));
    }

    return true;
}

namespace
{
    void SetIntCurveCVar(const TCHAR* Name, int32 Value)
    {
        if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
        {
            Var->Set(Value, ECVF_SetByCode);
        }
    }

    bool IngestCurveBuffer(const TArray<uint8>& Buffer, const TCHAR* Filename,
                           FJUSYNCCompactMeshRef& OutMesh)
    {
        anari_usd_protocol::JusyncRawGeoHeader Header{};
        if (!ReadHeader(Buffer, Header))
        {
            return false;
        }
        return JUSYNCRawGeoCurves::IngestAndBuild(Buffer, Filename, Header, OutMesh);
    }

    FVector3f CurveRingCenter(const FJUSYNCCompactMeshData& Mesh, int32 NodeIndex)
    {
        const int32 Base = NodeIndex * 4;
        return (Mesh.Points[Base] + Mesh.Points[Base + 1] + Mesh.Points[Base + 2] + Mesh.Points[Base + 3])
            * 0.25f;
    }

    TArray<TArray<FVector3f>> MergeCurves()
    {
        TArray<TArray<FVector3f>> Curves;
        Curves.Add({ FVector3f(1.0f, 1.0f, 1.0f), FVector3f(2.0f, 1.0f, 2.0f) });
        Curves.Add({ FVector3f(3.0f, 1.0f, 3.0f), FVector3f(4.0f, 1.0f, 4.0f), FVector3f(5.0f, 1.0f, 5.0f) });
        Curves.Add({ FVector3f(6.0f, 1.0f, 6.0f), FVector3f(7.0f, 1.0f, 7.0f) });
        Curves.Add({ FVector3f(8.0f, 1.0f, 8.0f), FVector3f(9.0f, 1.0f, 9.0f),
                     FVector3f(10.0f, 1.0f, 10.0f), FVector3f(11.0f, 1.0f, 11.0f) });
        return Curves;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoCurveMergeTest, "JUSYNC.RawGeo.CurveMerge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoCurveMergeTest::RunTest(const FString& Parameters)
{
    JUSYNCRawGeoCurves::FlushMerges();

    const TArray<TArray<FVector3f>> Curves = MergeCurves();
    const int32 TotalNodes = 2 + 3 + 2 + 4;

    FJUSYNCCompactMeshRef Mesh;
    TestTrue(TEXT("subset 1 ingests"),
        IngestCurveBuffer(MakeCurveBuffer(Curves, 1, 2), TEXT("clips/Wave_Geom__0_s1.geo"), Mesh));
    TestFalse(TEXT("subset 1 alone is not a completed frame"), Mesh.IsValid());

    TestTrue(TEXT("subset 0 ingests"),
        IngestCurveBuffer(MakeCurveBuffer(Curves, 0, 2), TEXT("clips/Wave_Geom__0_s0.geo"), Mesh));
    TestTrue(TEXT("completion emits a mesh"), Mesh.IsValid());
    if (Mesh.IsValid())
    {
        TestEqual(TEXT("merged vertex count"), Mesh->Points.Num(), TotalNodes * 4);
        TestEqual(TEXT("merged triangle count"), Mesh->Triangles.Num() / 3, (1 + 2 + 1 + 3) * 4 * 2);
        TestTrue(TEXT("first curve is ordinal 0 from subset 0"),
            CurveRingCenter(*Mesh, 0).Equals(FVector3f(1.0f, 1.0f, -1.0f)));
        TestTrue(TEXT("last curve is ordinal 3 from subset 1"),
            CurveRingCenter(*Mesh, TotalNodes - 1).Equals(FVector3f(11.0f, 11.0f, -1.0f)));
    }

    // A completed merge is dropped, so the same subset starts a fresh frame and
    // policy 2 (default) emits that frame's coarse preview.
    Mesh.Reset();
    TestTrue(TEXT("fresh subset 0 ingests"),
        IngestCurveBuffer(MakeCurveBuffer(Curves, 0, 2), TEXT("clips/Wave_Geom__0_s0.geo"), Mesh));
    TestTrue(TEXT("fresh coarse mesh"), Mesh.IsValid());
    if (Mesh.IsValid())
    {
        TestEqual(TEXT("coarse vertex count"), Mesh->Points.Num(), (2 + 2) * 4);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoCurveEmptySubsetTest, "JUSYNC.RawGeo.CurveEmptySubset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoCurveEmptySubsetTest::RunTest(const FString& Parameters)
{
    JUSYNCRawGeoCurves::FlushMerges();

    TArray<TArray<FVector3f>> Curves;
    Curves.Add({ FVector3f(1.0f, 1.0f, 1.0f), FVector3f(2.0f, 1.0f, 2.0f), FVector3f(3.0f, 1.0f, 3.0f) });

    FJUSYNCCompactMeshRef Mesh;
    for (const int32 Subset : { 3, 2, 1 })
    {
        const FString Name = FString::Printf(TEXT("clips/Empty_Geom__0_s%d.geo"), Subset);
        Mesh.Reset();
        TestTrue(TEXT("empty subset ingests"), IngestCurveBuffer(MakeCurveBuffer(Curves, Subset, 4), *Name, Mesh));
        TestFalse(TEXT("empty subset does not complete"), Mesh.IsValid());
    }

    Mesh.Reset();
    TestTrue(TEXT("last subset ingests"),
        IngestCurveBuffer(MakeCurveBuffer(Curves, 0, 4), TEXT("clips/Empty_Geom__0_s0.geo"), Mesh));
    TestTrue(TEXT("frame completes after empty subsets"), Mesh.IsValid());
    if (Mesh.IsValid())
    {
        TestEqual(TEXT("single curve vertex count"), Mesh->Points.Num(), 3 * 4);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoCurveRankTest, "JUSYNC.RawGeo.CurveRank",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoCurveRankTest::RunTest(const FString& Parameters)
{
    JUSYNCRawGeoCurves::FlushMerges();

    TArray<TArray<FVector3f>> Rank0;
    Rank0.Add({ FVector3f(1.0f, 1.0f, 1.0f), FVector3f(2.0f, 1.0f, 2.0f) });
    Rank0.Add({ FVector3f(3.0f, 1.0f, 3.0f), FVector3f(4.0f, 1.0f, 4.0f) });

    TArray<TArray<FVector3f>> Rank1;
    Rank1.Add({ FVector3f(10.0f, 1.0f, 10.0f), FVector3f(11.0f, 1.0f, 11.0f), FVector3f(12.0f, 1.0f, 12.0f) });
    Rank1.Add({ FVector3f(13.0f, 1.0f, 13.0f), FVector3f(14.0f, 1.0f, 14.0f) });

    FJUSYNCCompactMeshRef Mesh0;
    TestTrue(TEXT("rank 0 subset 1 ingests"),
        IngestCurveBuffer(MakeCurveBuffer(Rank0, 1, 2), TEXT("clips/Wave_Geom__r0_0_s1.geo"), Mesh0));
    TestFalse(TEXT("rank 0 subset 1 incomplete"), Mesh0.IsValid());
    TestTrue(TEXT("rank 0 subset 0 ingests"),
        IngestCurveBuffer(MakeCurveBuffer(Rank0, 0, 2), TEXT("clips/Wave_Geom__r0_0_s0.geo"), Mesh0));

    FJUSYNCCompactMeshRef Mesh1;
    TestTrue(TEXT("rank 1 subset 1 ingests"),
        IngestCurveBuffer(MakeCurveBuffer(Rank1, 1, 2), TEXT("clips/Wave_Geom__r1_0_s1.geo"), Mesh1));
    TestFalse(TEXT("rank 1 subset 1 incomplete"), Mesh1.IsValid());
    TestTrue(TEXT("rank 1 subset 0 ingests"),
        IngestCurveBuffer(MakeCurveBuffer(Rank1, 0, 2), TEXT("clips/Wave_Geom__r1_0_s0.geo"), Mesh1));

    TestTrue(TEXT("rank 0 completes separately"), Mesh0.IsValid());
    TestTrue(TEXT("rank 1 completes separately"), Mesh1.IsValid());
    if (Mesh0.IsValid() && Mesh1.IsValid())
    {
        TestEqual(TEXT("rank 0 vertex count"), Mesh0->Points.Num(), 4 * 4);
        TestEqual(TEXT("rank 1 vertex count"), Mesh1->Points.Num(), 5 * 4);
        TestTrue(TEXT("rank 0 starts at its own curve"),
            CurveRingCenter(*Mesh0, 0).Equals(FVector3f(1.0f, 1.0f, -1.0f)));
        TestTrue(TEXT("rank 1 starts at its own curve"),
            CurveRingCenter(*Mesh1, 0).Equals(FVector3f(10.0f, 10.0f, -1.0f)));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoCurveDriftTest, "JUSYNC.RawGeo.CurveDrift",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoCurveDriftTest::RunTest(const FString& Parameters)
{
    JUSYNCRawGeoCurves::FlushMerges();

    const TArray<TArray<FVector3f>> Curves = MergeCurves();

    FJUSYNCCompactMeshRef Mesh;
    TestTrue(TEXT("subset 1 under S=2 ingests"),
        IngestCurveBuffer(MakeCurveBuffer(Curves, 1, 2), TEXT("clips/Drift_Geom__0_s1.geo"), Mesh));
    TestFalse(TEXT("S=2 subset 1 is incomplete"), Mesh.IsValid());

    Mesh.Reset();
    TestTrue(TEXT("subset 1 under S=4 ingests"),
        IngestCurveBuffer(MakeCurveBuffer(Curves, 1, 4), TEXT("clips/Drift_Geom__0_s1.geo"), Mesh));
    TestFalse(TEXT("S drift resets instead of completing"), Mesh.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoRankParseTest, "JUSYNC.RawGeo.RankParse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoRankParseTest::RunTest(const FString& Parameters)
{
    int32 Rank = INDEX_NONE;
    TestTrue(TEXT("rank after the last __"), JUSYNCElementNames::ParseFileRank(
        TEXT("Wave_Geom__r3_7_s2.geo"), Rank));
    TestEqual(TEXT("parsed rank"), Rank, 3);

    Rank = INDEX_NONE;
    TestTrue(TEXT("rank zero"), JUSYNCElementNames::ParseFileRank(
        TEXT("Wave_Geom__r0_0_s1.geo"), Rank));
    TestEqual(TEXT("parsed rank zero"), Rank, 0);

    TestFalse(TEXT("no rank mark"), JUSYNCElementNames::ParseFileRank(
        TEXT("Wave_Geom__7_s2.geo"), Rank));
    TestFalse(TEXT("rank mark before the last __"), JUSYNCElementNames::ParseFileRank(
        TEXT("Foo_r2_Geom__0.geo"), Rank));
    TestFalse(TEXT("rank mark without a trailing token"), JUSYNCElementNames::ParseFileRank(
        TEXT("Wave_Geom__r3.geo"), Rank));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoCurveDecimateTest, "JUSYNC.RawGeo.CurveDecimate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoCurveDecimateTest::RunTest(const FString& Parameters)
{
    JUSYNCRawGeoCurves::FlushMerges();

    TArray<TArray<FVector3f>> Curves;
    TArray<FVector3f> LongCurve;
    for (int32 i = 0; i < 100; ++i)
    {
        LongCurve.Add(FVector3f(static_cast<float>(i), 0.0f, 0.0f));
    }
    Curves.Add(LongCurve);

    SetIntCurveCVar(TEXT("jusync.Curve.MaxNodes"), 10);
    FJUSYNCCompactMeshRef Mesh;
    const bool bIngested = IngestCurveBuffer(MakeCurveBuffer(Curves, 0, 1), TEXT("clips/Long_Geom__0.geo"), Mesh);
    SetIntCurveCVar(TEXT("jusync.Curve.MaxNodes"), 4000000);

    TestTrue(TEXT("decimated payload ingests"), bIngested);
    TestTrue(TEXT("mesh produced"), Mesh.IsValid());
    if (Mesh.IsValid())
    {
        const int32 Rings = Mesh->Points.Num() / 4;
        TestTrue(TEXT("at most budget rings"), Rings <= 10);
        TestTrue(TEXT("first node kept"), CurveRingCenter(*Mesh, 0).Equals(FVector3f(0.0f, 0.0f, 0.0f)));
        TestTrue(TEXT("last node kept"), CurveRingCenter(*Mesh, Rings - 1).Equals(FVector3f(99.0f, 0.0f, 0.0f)));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoCurveTransientTest, "JUSYNC.RawGeo.CurveTransient",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoCurveTransientTest::RunTest(const FString& Parameters)
{
    JUSYNCRawGeoCurves::FlushMerges();
    SetIntCurveCVar(TEXT("jusync.RawGeo.CurveProgressive"), 2);

    const TArray<TArray<FVector3f>> Curves = MergeCurves();

    FJUSYNCCompactMeshRef Coarse;
    TestTrue(TEXT("coarse subset ingests"),
        IngestCurveBuffer(MakeCurveBuffer(Curves, 0, 2), TEXT("clips/Transient_Geom__0_s0.geo"), Coarse));
    TestTrue(TEXT("coarse mesh produced"), Coarse.IsValid());
    if (Coarse.IsValid())
    {
        TestTrue(TEXT("coarse frame is transient"), Coarse->bTransient);
    }

    FJUSYNCCompactMeshRef Complete;
    TestTrue(TEXT("completing subset ingests"),
        IngestCurveBuffer(MakeCurveBuffer(Curves, 1, 2), TEXT("clips/Transient_Geom__0_s1.geo"), Complete));
    TestTrue(TEXT("completed mesh produced"), Complete.IsValid());
    if (Complete.IsValid())
    {
        TestFalse(TEXT("completed frame is cacheable"), Complete->bTransient);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoCurveSidesTest, "JUSYNC.RawGeo.CurveSides",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoCurveSidesTest::RunTest(const FString& Parameters)
{
    JUSYNCRawGeoCurves::FlushMerges();

    TArray<TArray<FVector3f>> Curves;
    Curves.Add({ FVector3f(0.0f, 0.0f, 0.0f), FVector3f(1.0f, 0.0f, 0.0f),
                 FVector3f(2.0f, 0.0f, 0.0f), FVector3f(3.0f, 0.0f, 0.0f) });

    SetIntCurveCVar(TEXT("jusync.Curve.Sides"), 3);
    FJUSYNCCompactMeshRef Three;
    TestTrue(TEXT("three-sided ingest"),
        IngestCurveBuffer(MakeCurveBuffer(Curves, 0, 1), TEXT("clips/Sides_Geom__0.geo"), Three));
    TestTrue(TEXT("three-sided mesh"), Three.IsValid());
    if (Three.IsValid())
    {
        TestEqual(TEXT("three sides per node"), Three->Points.Num(), 4 * 3);
    }

    SetIntCurveCVar(TEXT("jusync.Curve.Sides"), 9);
    FJUSYNCCompactMeshRef Clamped;
    TestTrue(TEXT("nine-sided ingest"),
        IngestCurveBuffer(MakeCurveBuffer(Curves, 0, 1), TEXT("clips/Sides_Geom__0.geo"), Clamped));
    TestTrue(TEXT("nine-sided mesh"), Clamped.IsValid());
    if (Clamped.IsValid())
    {
        TestEqual(TEXT("sides clamp to 8"), Clamped->Points.Num(), 4 * 8);
    }

    SetIntCurveCVar(TEXT("jusync.Curve.Sides"), 4);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
