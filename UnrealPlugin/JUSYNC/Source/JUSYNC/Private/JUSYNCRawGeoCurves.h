#pragma once

#include "CoreMinimal.h"
#include "JUSYNCTypes.h"
#include "AnariUsdProtocol.h"

// One RawGeo v3 curve payload, already converted to UE space. `CurveCounts`
// partitions `Points`; colors/widths are per point when present.
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
    TArray<FVector3f> Points;   // UE space: (x, z, -y)
    TArray<FColor> Colors;      // empty, or Points.Num() entries
    TArray<float> Widths;       // empty, or Points.Num() entries
};

namespace JUSYNCRawGeoCurves
{
    // Decode positions/colors/curve counts/widths from a validated v3 header.
    bool ParsePayload(const uint8* Data, SIZE_T Size,
                      const anari_usd_protocol::JusyncRawGeoHeader& Hdr,
                      FJUSYNCRawGeoCurvePayload& Out);

    // Parse one payload and, when it completes a frame (or is the coarse base),
    // expand its curves into the compact triangle mesh the spawner can render.
    // OutMesh stays null for intermediate subsets.
    bool IngestAndBuild(const TArray<uint8>& Buffer, const FString& Filename,
                        const anari_usd_protocol::JusyncRawGeoHeader& Hdr,
                        FJUSYNCCompactMeshRef& OutMesh);

    // Drop the "_s<k>" refinement suffix (and the ".geo" extension) so every
    // subset of one rank-scoped clip shares a merge key.
    FString StripSubsetSuffix(const FString& Filename);

    // Drop all in-flight merges (tests, disconnect, teardown).
    void FlushMerges();
}
