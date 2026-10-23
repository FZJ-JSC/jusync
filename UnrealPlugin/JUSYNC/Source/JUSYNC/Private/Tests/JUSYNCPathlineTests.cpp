#include "Misc/AutomationTest.h"

#include "JUSYNCPathlineManager.h"
#include "JUSYNCTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    FJUSYNCPointCloudRef MakeCloud(
        const TArray<uint64>& Ids,
        const TArray<FVector>& Positions,
        const TArray<float>& Scalars,
        int32 IdComponents = 1,
        const TCHAR* ElementName = TEXT("LivePoints"))
    {
        auto Cloud = MakeShared<FJUSYNCPointCloudData, ESPMode::ThreadSafe>();
        Cloud->ElementName = ElementName;
        Cloud->TypeName = TEXT("GeomPoints");
        Cloud->PointCount = Ids.Num();
        Cloud->bFromRawGeo = true;
        Cloud->bComplete = true;
        Cloud->FullPointCount = Ids.Num();
        Cloud->bHasLidarPoints = true;
        Cloud->LidarPoints.SetNum(Ids.Num());
        for (int32 i = 0; i < Ids.Num(); ++i)
        {
            const FVector P = Positions.IsValidIndex(i) ? Positions[i] : FVector::ZeroVector;
            Cloud->LidarPoints[i] = FLidarPointCloudPoint(
                static_cast<float>(P.X), static_cast<float>(P.Y), static_cast<float>(P.Z),
                1.0f, 1.0f, 1.0f, 1.0f);
        }

        FJUSYNCPointAttribute IdAttribute;
        IdAttribute.Name = TEXT("particle_id");
        IdAttribute.Components = IdComponents;
        IdAttribute.Values.Reserve(Ids.Num() * IdComponents);
        for (const uint64 Id : Ids)
        {
            for (int32 Component = 0; Component < IdComponents; ++Component)
            {
                IdAttribute.Values.Add(
                    static_cast<float>((Id >> (16 * Component)) & 0xFFFFu));
            }
        }
        Cloud->Attributes.Add(IdAttribute);

        FJUSYNCPointAttribute ScalarAttribute;
        ScalarAttribute.Name = TEXT("attribute0");
        ScalarAttribute.Components = 1;
        ScalarAttribute.Values.Reserve(Scalars.Num());
        for (const float Scalar : Scalars)
        {
            ScalarAttribute.Values.Add(Scalar);
        }
        Cloud->Attributes.Add(ScalarAttribute);
        return Cloud;
    }

    FJUSYNCPathlineSettings MakeSettings()
    {
        FJUSYNCPathlineSettings Settings;
        Settings.bEnabled = true;
        Settings.MaxTracks = 16;
        Settings.HistorySamples = 32;
        Settings.MaxNodes = 4096;
        Settings.TubeSides = 4;
        Settings.Radius = 0.5f;
        Settings.ColorMin = 0.0f;
        Settings.ColorMax = 1.0f;
        return Settings;
    }

    void Ingest(FJUSYNCPathlineManager& Manager, uint64 SampleId,
                const TArray<uint64>& Ids, const TArray<FVector>& Positions,
                const TArray<float>& Scalars, int32 IdComponents = 1,
                const TArray<FColor>& Lut = {})
    {
        Manager.IngestSampleNow(TEXT("LivePoints"),
            MakeCloud(Ids, Positions, Scalars, IdComponents), 0, SampleId, Lut);
    }

    bool Build(FJUSYNCPathlineManager& Manager, uint64 Cutoff,
               FJUSYNCCompactMeshData& OutMesh, int32& OutVisible)
    {
        return Manager.BuildGeometry(TEXT("LivePoints"), Cutoff, OutMesh, OutVisible);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCPathlineId16Test, "JUSYNC.Pathlines.DecodeParticleId16",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCPathlineId16Test::RunTest(const FString& Parameters)
{
    FJUSYNCPathlineManager Manager;
    Manager.SetSettings(MakeSettings());
    Manager.Reset();
    Ingest(Manager, 1, { 60000 }, { FVector::ZeroVector }, { 0.0f });

    const TArray<uint64> Ids = Manager.GetTrackIds(TEXT("LivePoints"), 0);
    TestEqual(TEXT("one track"), Ids.Num(), 1);
    if (Ids.Num() == 1)
    {
        TestEqual(TEXT("16-bit id"), Ids[0], 60000ull);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCPathlineId32Test, "JUSYNC.Pathlines.DecodeParticleId32",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCPathlineId32Test::RunTest(const FString& Parameters)
{
    FJUSYNCPathlineManager Manager;
    Manager.SetSettings(MakeSettings());
    Manager.Reset();
    const uint64 Id = 0x0000000000012345ull;
    Ingest(Manager, 1, { Id }, { FVector::ZeroVector }, { 0.0f }, 2);

    const TArray<uint64> Ids = Manager.GetTrackIds(TEXT("LivePoints"), 0);
    TestEqual(TEXT("one track"), Ids.Num(), 1);
    if (Ids.Num() == 1)
    {
        TestEqual(TEXT("32-bit id"), Ids[0], Id);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCPathlineId64Test, "JUSYNC.Pathlines.DecodeParticleId64",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCPathlineId64Test::RunTest(const FString& Parameters)
{
    FJUSYNCPathlineManager Manager;
    Manager.SetSettings(MakeSettings());
    Manager.Reset();
    const uint64 Id = 0x0123456789ABCDEFull;
    Ingest(Manager, 1, { Id }, { FVector::ZeroVector }, { 0.0f }, 4);

    const TArray<uint64> Ids = Manager.GetTrackIds(TEXT("LivePoints"), 0);
    TestEqual(TEXT("one track"), Ids.Num(), 1);
    if (Ids.Num() == 1)
    {
        TestEqual(TEXT("64-bit id"), Ids[0], Id);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCPathlineSelectionTest, "JUSYNC.Pathlines.SelectionIsDeterministic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCPathlineSelectionTest::RunTest(const FString& Parameters)
{
    TArray<uint64> AllIds;
    TArray<FVector> Positions;
    TArray<float> Scalars;
    for (int32 i = 0; i < 12; ++i)
    {
        AllIds.Add(static_cast<uint64>(100 + i));
        Positions.Add(FVector(i, 0, 0));
        Scalars.Add(static_cast<float>(i) / 12.0f);
    }

    FJUSYNCPathlineSettings Settings = MakeSettings();
    Settings.MaxTracks = 4;
    Settings.MaxNodes = 64;

    FJUSYNCPathlineManager Manager;
    Manager.SetSettings(Settings);
    Manager.Reset();
    Ingest(Manager, 1, AllIds, Positions, Scalars);
    const TArray<uint64> First = Manager.GetTrackIds(TEXT("LivePoints"), 0);

    Manager.Reset();
    Manager.SetSettings(Settings);
    Ingest(Manager, 1, AllIds, Positions, Scalars);
    const TArray<uint64> Second = Manager.GetTrackIds(TEXT("LivePoints"), 0);

    TestEqual(TEXT("selection is deterministic"), First, Second);
    TestEqual(TEXT("selection honors MaxTracks"), First.Num(), 4);

    Manager.Reset();
    Settings.SelectionSeed = 99;
    Manager.SetSettings(Settings);
    Ingest(Manager, 1, AllIds, Positions, Scalars);
    const TArray<uint64> Third = Manager.GetTrackIds(TEXT("LivePoints"), 0);
    TestFalse(TEXT("seed changes selection"), First == Third);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCPathlineMissingIdTest, "JUSYNC.Pathlines.MissingIdBreaksTrack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCPathlineMissingIdTest::RunTest(const FString& Parameters)
{
    FJUSYNCPathlineManager Manager;
    Manager.SetSettings(MakeSettings());
    Manager.Reset();

    Ingest(Manager, 1, { 1 }, { FVector(0, 0, 0) }, { 0.0f });
    Ingest(Manager, 2, { 1 }, { FVector(1, 0, 0) }, { 0.0f });
    Ingest(Manager, 3, { 2 }, { FVector(0, 1, 0) }, { 0.0f });
    Ingest(Manager, 4, { 1 }, { FVector(3, 0, 0) }, { 0.0f });

    FJUSYNCCompactMeshData Mesh;
    int32 Visible = 0;
    TestTrue(TEXT("geometry builds"), Build(Manager, MAX_uint64, Mesh, Visible));
    TestEqual(TEXT("gap splits the track into one drawable run"),
        Mesh.Triangles.Num() / 3, 8);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCPathlineDuplicateIdTest, "JUSYNC.Pathlines.DuplicateIdUsesFirstOccurrence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCPathlineDuplicateIdTest::RunTest(const FString& Parameters)
{
    FJUSYNCPathlineManager Manager;
    Manager.SetSettings(MakeSettings());
    Manager.Reset();
    Ingest(Manager, 1, { 7, 7 }, { FVector(0, 0, 0), FVector(9, 9, 9) }, { 0.0f, 0.0f });

    TestEqual(TEXT("duplicate does not create a second track"), Manager.GetTrackIds(TEXT("LivePoints"), 0).Num(), 1);
    TestEqual(TEXT("duplicate is counted"), Manager.GetStats().DuplicateIds, 1ll);
    TestEqual(TEXT("duplicate does not add a node"), Manager.GetElementNodeCount(TEXT("LivePoints")), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCPathlineOmittedElementTest, "JUSYNC.Pathlines.OmittedElementDoesNotAge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCPathlineOmittedElementTest::RunTest(const FString& Parameters)
{
    FJUSYNCPathlineSettings Settings = MakeSettings();
    Settings.TrackExpirySamples = 1;
    FJUSYNCPathlineManager Manager;
    Manager.SetSettings(Settings);
    Manager.Reset();

    Manager.IngestSampleNow(TEXT("A"), MakeCloud({ 1 }, { FVector::ZeroVector }, { 0.0f }, 1, TEXT("A")),
        0, 1, {});
    Manager.IngestSampleNow(TEXT("A"), MakeCloud({ 1 }, { FVector(1, 0, 0) }, { 0.0f }, 1, TEXT("A")),
        0, 2, {});
    const int32 Before = Manager.GetElementNodeCount(TEXT("A"));

    Manager.IngestSampleNow(TEXT("B"), MakeCloud({ 2 }, { FVector::ZeroVector }, { 0.0f }, 1, TEXT("B")),
        0, 1, {});

    TestEqual(TEXT("unfed element keeps its nodes"), Manager.GetElementNodeCount(TEXT("A")), Before);
    TestEqual(TEXT("unfed element keeps its track"), Manager.GetTrackIds(TEXT("A"), 0).Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCPathlineFilterColorTest, "JUSYNC.Pathlines.FilterHidesNonMatching",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCPathlineFilterColorTest::RunTest(const FString& Parameters)
{
    FJUSYNCPathlineSettings Settings = MakeSettings();
    Settings.bFilterEnabled = true;
    Settings.FilterMin = 0.5f;
    Settings.FilterMax = 1.0f;
    Settings.bColorByScalar = true;
    Settings.ColorMin = 0.0f;
    Settings.ColorMax = 1.0f;
    FJUSYNCPathlineManager Manager;
    Manager.SetSettings(Settings);
    Manager.Reset();

    const TArray<FColor> Lut = { FColor::Red, FColor::Green };
    Ingest(Manager, 1, { 1, 2 }, { FVector(0, 0, 0), FVector(0, 1, 0) }, { 0.2f, 0.8f }, 1, Lut);
    Ingest(Manager, 2, { 1, 2 }, { FVector(1, 0, 0), FVector(1, 1, 0) }, { 0.2f, 0.8f }, 1, Lut);

    FJUSYNCCompactMeshData Mesh;
    int32 Visible = 0;
    TestTrue(TEXT("geometry builds"), Build(Manager, MAX_uint64, Mesh, Visible));
    TestEqual(TEXT("only the matching track is visible"), Visible, 1);
    TestEqual(TEXT("one tube run"), Mesh.Triangles.Num() / 3, 8);
    TestTrue(TEXT("latest scalar drives the LUT color"),
        Mesh.VertexColors.Num() > 0 && Mesh.VertexColors[0] == FColor::Green);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCPathlineCutoffTest, "JUSYNC.Pathlines.DisplayCutoffScrubs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCPathlineCutoffTest::RunTest(const FString& Parameters)
{
    FJUSYNCPathlineManager Manager;
    Manager.SetSettings(MakeSettings());
    Manager.Reset();
    Ingest(Manager, 1, { 1 }, { FVector(0, 0, 0) }, { 0.0f });
    Ingest(Manager, 2, { 1 }, { FVector(1, 0, 0) }, { 0.0f });
    Ingest(Manager, 3, { 1 }, { FVector(2, 0, 0) }, { 0.0f });

    FJUSYNCCompactMeshData Full;
    FJUSYNCCompactMeshData Cut;
    int32 Visible = 0;
    TestTrue(TEXT("full build"), Build(Manager, MAX_uint64, Full, Visible));
    TestTrue(TEXT("cut build"), Build(Manager, 2, Cut, Visible));
    TestEqual(TEXT("three retained nodes"), Manager.GetElementNodeCount(TEXT("LivePoints")), 3);
    TestEqual(TEXT("full cutoff draws two segments"), Full.Triangles.Num() / 3, 16);
    TestEqual(TEXT("earlier cutoff draws one segment"), Cut.Triangles.Num() / 3, 8);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCPathlineGeometryTest, "JUSYNC.Pathlines.GeometryBuildsTubes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCPathlineGeometryTest::RunTest(const FString& Parameters)
{
    FJUSYNCPathlineSettings Settings = MakeSettings();
    Settings.TubeSides = 4;
    Settings.Radius = 0.5f;
    FJUSYNCPathlineManager Manager;
    Manager.SetSettings(Settings);
    Manager.Reset();
    Ingest(Manager, 1, { 1 }, { FVector(0, 0, 0) }, { 0.0f });
    Ingest(Manager, 2, { 1 }, { FVector(10, 0, 0) }, { 0.0f });

    FJUSYNCCompactMeshData Mesh;
    int32 Visible = 0;
    TestTrue(TEXT("geometry builds"), Build(Manager, MAX_uint64, Mesh, Visible));
    TestEqual(TEXT("one visible track"), Visible, 1);
    TestEqual(TEXT("two rings"), Mesh.Points.Num(), 8);
    TestEqual(TEXT("one tube band"), Mesh.Triangles.Num() / 3, 8);
    TestTrue(TEXT("normals present"), Mesh.HasNormals());
    TestTrue(TEXT("colors present"), Mesh.HasVertexColors());
    TestTrue(TEXT("baked colors"), Mesh.bHasBakedColors);

    FVector3f RingCenter = FVector3f::ZeroVector;
    for (int32 i = 0; i < 4; ++i)
    {
        RingCenter += Mesh.Points[i];
    }
    RingCenter *= 0.25f;
    TestTrue(TEXT("first ring centers on the first node"), RingCenter.Equals(FVector3f(0, 0, 0)));
    for (int32 i = 0; i < 4; ++i)
    {
        const float Radius = (Mesh.Points[i] - RingCenter).Size();
        TestTrue(TEXT("ring uses the configured radius"), FMath::IsNearlyEqual(Radius, 0.5f, 1.0e-3f));
        TestTrue(TEXT("normals are unit length"),
            FMath::IsNearlyEqual(Mesh.Normals[i].Size(), 1.0f, 1.0e-3f));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCPathlineBudgetTest, "JUSYNC.Pathlines.HistoryAndNodeBudgetClamp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCPathlineBudgetTest::RunTest(const FString& Parameters)
{
    FJUSYNCPathlineSettings Settings = MakeSettings();
    Settings.MaxTracks = 2;
    Settings.HistorySamples = 4;
    Settings.MaxNodes = 8;
    FJUSYNCPathlineManager Manager;
    Manager.SetSettings(Settings);
    Manager.Reset();

    for (int32 Sample = 1; Sample <= 10; ++Sample)
    {
        Ingest(Manager, Sample, { 1, 2 },
            { FVector(Sample, 0, 0), FVector(Sample, 1, 0) }, { 0.0f, 0.0f });
    }

    TestTrue(TEXT("global node budget respected"), Manager.GetStats().TotalNodes <= 8);
    TestEqual(TEXT("per-track history clamped"), Manager.GetTrackNodeCount(TEXT("LivePoints"), 0, 1), 4);
    TestEqual(TEXT("per-track history clamped for id 2"), Manager.GetTrackNodeCount(TEXT("LivePoints"), 0, 2), 4);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
