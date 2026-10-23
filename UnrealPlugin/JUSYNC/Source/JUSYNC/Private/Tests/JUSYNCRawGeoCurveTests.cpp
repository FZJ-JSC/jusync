#include "Misc/AutomationTest.h"

#include "JUSYNCRawGeoCurves.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
