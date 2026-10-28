#pragma once

#include "CoreMinimal.h"
#include "JUSYNCTypes.h"
#include "AnariUsdProtocol.h"

struct FJUSYNCRawGeoCurvePayload
{
    FString ElementName;
    double Time = -1.0;
    int64 SubsetIndex = 0;
    int64 SubsetCount = 1;
    int64 FullCurveCount = 0;
    int64 FullNumPoints = 0;
    bool bHasColors = false;
    bool bHasWidths = false;
    TArray<int32> CurveCounts;
    TArray<FVector3f> Points;  // UE space: (x, z, -y)
    TArray<FColor> Colors;  // empty, or Points.Num() entries
    TArray<float> Widths;  // empty, or Points.Num() entries
};

namespace JUSYNCRawGeoCurves
{
    bool ValidateCurveHeader(const anari_usd_protocol::JusyncRawGeoHeader& Hdr);

    bool ParsePayload(const uint8* Data, SIZE_T Size,
                      const anari_usd_protocol::JusyncRawGeoHeader& Hdr,
                      FJUSYNCRawGeoCurvePayload& Out);

    bool IngestAndBuild(const TArray<uint8>& Buffer, const FString& Filename,
                        const anari_usd_protocol::JusyncRawGeoHeader& Hdr,
                        FJUSYNCCompactMeshRef& OutMesh);

    FString StripSubsetSuffix(const FString& Filename);

    void FlushMerges();
}
