#include "Misc/AutomationTest.h"

#include "JUSYNCSubsystem.h"
#include "JUSYNCTypes.h"
#include "AnariUsdMiddleware_C.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    struct FAttributeSpec
    {
        FString Name;
        int32 Components = 1;
        TArray<float> Values;
    };

    TArray<uint8> MakeRawGeoPointBuffer(
        int32 NumPoints, int32 FullNumPoints, int32 SubsetIndex, int32 SubsetCount,
        uint32 Version, const TArray<FAttributeSpec>& Attributes,
        int32 ColorComponents = 0, const TCHAR* PrimName = TEXT("LivePoints"))
    {
        uint64 PayloadBytes = static_cast<uint64>(NumPoints) * 3 * sizeof(float);
        PayloadBytes += static_cast<uint64>(NumPoints) * ColorComponents * sizeof(float);
        for (const FAttributeSpec& Attr : Attributes)
        {
            PayloadBytes += static_cast<uint64>(NumPoints) * Attr.Components * sizeof(float);
        }

        const size_t HeaderBytes = anari_usd_protocol::JusyncRawGeoHeader::header_bytes(Version);
        const size_t DescBytes =
            static_cast<size_t>(Attributes.Num()) * sizeof(anari_usd_protocol::JusyncRawGeoAttrDesc);
        TArray<uint8> Buffer;
        Buffer.SetNumZeroed(static_cast<int32>(HeaderBytes + DescBytes + PayloadBytes));

        anari_usd_protocol::JusyncRawGeoHeaderV2 Header{};
        Header.magic = anari_usd_protocol::JUSYNC_RAWGEO_MAGIC;
        Header.version = Version;
        FCStringAnsi::Strncpy(Header.prim_name, TCHAR_TO_UTF8(PrimName),
            UE_ARRAY_COUNT(Header.prim_name) - 1);
        Header.num_points = NumPoints;
        Header.time = 1.25;
        Header.position_components = 3;
        Header.color_components = ColorComponents;
        Header.num_attributes = Attributes.Num();
        Header.attr_uniform_count = NumPoints;
        Header.payload_bytes = PayloadBytes;
        Header.subset_index = SubsetIndex;
        Header.subset_count = SubsetCount;
        Header.full_num_points = FullNumPoints;

        FMemory::Memcpy(Buffer.GetData(), &Header, HeaderBytes);
        uint8* Cursor = Buffer.GetData() + HeaderBytes;

        for (const FAttributeSpec& Attr : Attributes)
        {
            anari_usd_protocol::JusyncRawGeoAttrDesc Desc{};
            FCStringAnsi::Strncpy(Desc.name, TCHAR_TO_UTF8(*Attr.Name),
                UE_ARRAY_COUNT(Desc.name) - 1);
            Desc.components = static_cast<uint32>(Attr.Components);
            FMemory::Memcpy(Cursor, &Desc, sizeof(Desc));
            Cursor += sizeof(Desc);
        }

        for (int32 Point = 0; Point < NumPoints; ++Point)
        {
            const int32 SourceIndex = SubsetIndex + Point * SubsetCount;
            const float Position[3] = {
                static_cast<float>(SourceIndex * 10),
                static_cast<float>(SourceIndex * 10 + 1),
                static_cast<float>(SourceIndex * 10 + 2)
            };
            FMemory::Memcpy(Cursor, Position, sizeof(Position));
            Cursor += sizeof(Position);
        }

        for (int32 Point = 0; Point < NumPoints; ++Point)
        {
            for (int32 Component = 0; Component < ColorComponents; ++Component)
            {
                const float Value = 1.0f;
                FMemory::Memcpy(Cursor, &Value, sizeof(Value));
                Cursor += sizeof(Value);
            }
        }

        for (const FAttributeSpec& Attr : Attributes)
        {
            for (int32 Point = 0; Point < NumPoints; ++Point)
            {
                for (int32 Component = 0; Component < Attr.Components; ++Component)
                {
                    const int32 ValueIndex = Point * Attr.Components + Component;
                    const float Value = Attr.Values.IsValidIndex(ValueIndex)
                        ? Attr.Values[ValueIndex] : 0.0f;
                    FMemory::Memcpy(Cursor, &Value, sizeof(Value));
                    Cursor += sizeof(Value);
                }
            }
        }

        return Buffer;
    }

    UJUSYNCSubsystem* MakeSubsystem()
    {
        UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
        return GameInstance ? NewObject<UJUSYNCSubsystem>(GameInstance) : nullptr;
    }

    bool LoadRawGeo(UJUSYNCSubsystem* Subsystem, const TArray<uint8>& Buffer,
                    FJUSYNCPointCloudRef& OutCloud)
    {
        TArray<FJUSYNCCompactMeshRef> Meshes;
        TArray<FJUSYNCPointCloudRef> Clouds;
        if (!Subsystem->LoadUSDFullCompactFromBuffer(
                Buffer, TEXT("clips/LivePoints_Geom__0.geo"), Meshes, Clouds))
        {
            return false;
        }
        if (Clouds.Num() != 1)
        {
            return false;
        }
        OutCloud = Clouds[0];
        return OutCloud.IsValid();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoAttributeWholeFrameTest,
    "JUSYNC.RawGeo.Attributes.WholeFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoAttributeWholeFrameTest::RunTest(const FString& Parameters)
{
    UJUSYNCSubsystem* Subsystem = MakeSubsystem();
    if (!Subsystem)
    {
        AddError(TEXT("failed to create a transient JUSYNC subsystem"));
        return false;
    }

    TArray<FAttributeSpec> Attributes;
    Attributes.Add({ TEXT("particle_id"), 1, { 10.0f, 20.0f, 30.0f } });
    Attributes.Add({ TEXT("attribute0"), 1, { 0.5f, 1.5f, 2.5f } });

    FJUSYNCPointCloudRef Cloud;
    TestTrue(TEXT("whole-frame v2 cloud loads"),
        LoadRawGeo(Subsystem, MakeRawGeoPointBuffer(3, 3, 0, 1, 2, Attributes), Cloud));
    if (!Cloud.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("cloud is RawGeo"), Cloud->bFromRawGeo);
    TestTrue(TEXT("cloud is complete"), Cloud->bComplete);
    TestEqual(TEXT("full point count"), Cloud->FullPointCount, 3);
    TestEqual(TEXT("attribute count"), Cloud->Attributes.Num(), 2);

    const FJUSYNCPointAttribute* Ids = Cloud->FindAttribute(TEXT("particle_id"));
    TestNotNull(TEXT("particle_id preserved"), Ids);
    if (Ids)
    {
        TestEqual(TEXT("particle_id components"), Ids->Components, 1);
        TestEqual(TEXT("particle_id point 1"), Ids->Values[1], 20.0f);
    }

    float Scalar = 0.0f;
    TestTrue(TEXT("attribute0 scalar readable"),
        Cloud->GetAttributeScalar(2, TEXT("attribute0"), 0, Scalar));
    TestEqual(TEXT("attribute0 point 2"), Scalar, 2.5f);

    TestEqual(TEXT("position is mapped to UE space"),
        Cloud->GetPosition(1), FVector(10.0, 12.0, -11.0));
    TestEqual(TEXT("attribute0 also supplies the width"), Cloud->GetWidth(1), 1.5f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoAttributeV1Test,
    "JUSYNC.RawGeo.Attributes.V1",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoAttributeV1Test::RunTest(const FString& Parameters)
{
    UJUSYNCSubsystem* Subsystem = MakeSubsystem();
    if (!Subsystem)
    {
        AddError(TEXT("failed to create a transient JUSYNC subsystem"));
        return false;
    }

    FJUSYNCPointCloudRef Cloud;
    TestTrue(TEXT("v1 cloud loads"),
        LoadRawGeo(Subsystem, MakeRawGeoPointBuffer(2, 2, 0, 1, 1, {}), Cloud));
    if (!Cloud.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("v1 is RawGeo"), Cloud->bFromRawGeo);
    TestTrue(TEXT("v1 is whole-frame complete"), Cloud->bComplete);
    TestEqual(TEXT("v1 full point count"), Cloud->FullPointCount, 2);
    TestEqual(TEXT("v1 has no attributes"), Cloud->Attributes.Num(), 0);
    TestEqual(TEXT("v1 has no widths"), Cloud->GetWidth(0), 1.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoAttributeSubsetTest,
    "JUSYNC.RawGeo.Attributes.SubsetMerge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoAttributeSubsetTest::RunTest(const FString& Parameters)
{
    UJUSYNCSubsystem* Subsystem = MakeSubsystem();
    if (!Subsystem)
    {
        AddError(TEXT("failed to create a transient JUSYNC subsystem"));
        return false;
    }

    TArray<FAttributeSpec> Subset0;
    Subset0.Add({ TEXT("particle_id"), 1, { 10.0f, 30.0f } });
    Subset0.Add({ TEXT("attribute0"), 1, { 0.1f, 0.3f } });

    TArray<FAttributeSpec> Subset1;
    Subset1.Add({ TEXT("particle_id"), 1, { 20.0f, 40.0f } });
    Subset1.Add({ TEXT("attribute0"), 1, { 0.2f, 0.4f } });

    FJUSYNCPointCloudRef Partial;
    TestTrue(TEXT("coarse subset loads"),
        LoadRawGeo(Subsystem, MakeRawGeoPointBuffer(2, 4, 0, 2, 2, Subset0), Partial));
    if (!Partial.IsValid())
    {
        return false;
    }
    TestFalse(TEXT("coarse subset is incomplete"), Partial->bComplete);
    TestEqual(TEXT("coarse subset full count"), Partial->FullPointCount, 4);
    TestEqual(TEXT("coarse subset point count"), Partial->PointCount, 2);
    TestEqual(TEXT("coarse subset first id"),
        Partial->FindAttribute(TEXT("particle_id"))->Values[0], 10.0f);
    TestEqual(TEXT("coarse subset second id"),
        Partial->FindAttribute(TEXT("particle_id"))->Values[1], 30.0f);

    FJUSYNCPointCloudRef Complete;
    TestTrue(TEXT("complementary subset loads"),
        LoadRawGeo(Subsystem, MakeRawGeoPointBuffer(2, 4, 1, 2, 2, Subset1), Complete));
    if (!Complete.IsValid())
    {
        return false;
    }
    TestTrue(TEXT("merge completes"), Complete->bComplete);
    TestEqual(TEXT("complete point count"), Complete->PointCount, 4);
    TestEqual(TEXT("complete full count"), Complete->FullPointCount, 4);

    const FJUSYNCPointAttribute* Ids = Complete->FindAttribute(TEXT("particle_id"));
    TestNotNull(TEXT("complete particle_id"), Ids);
    if (Ids)
    {
        TestEqual(TEXT("complete id 0"), Ids->Values[0], 10.0f);
        TestEqual(TEXT("complete id 1"), Ids->Values[1], 20.0f);
        TestEqual(TEXT("complete id 2"), Ids->Values[2], 30.0f);
        TestEqual(TEXT("complete id 3"), Ids->Values[3], 40.0f);
    }
    TestEqual(TEXT("complete attributes scatter in lattice order"),
        Complete->GetPosition(3), FVector(30.0, 32.0, -31.0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCRawGeoAttributePositionsTest,
    "JUSYNC.RawGeo.Attributes.PositionsArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJUSYNCRawGeoAttributePositionsTest::RunTest(const FString& Parameters)
{
    FJUSYNCPointCloudData Cloud;
    Cloud.PointCount = 2;
    Cloud.Positions = { FVector(1.0, 2.0, 3.0), FVector(4.0, 5.0, 6.0) };
    TestEqual(TEXT("Positions fallback point 0"), Cloud.GetPosition(0), FVector(1.0, 2.0, 3.0));
    TestEqual(TEXT("Positions fallback point 1"), Cloud.GetPosition(1), FVector(4.0, 5.0, 6.0));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
