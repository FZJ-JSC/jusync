#include "JUSYNCStreamBuilder.h"

bool JUSYNCBuildRealtimeMeshStreams(const FJUSYNCMeshData& MeshData, RealtimeMesh::FRealtimeMeshStreamSet& OutStreams)
{
    if (!MeshData.IsValid() || MeshData.Vertices.Num() == 0 ||
        MeshData.Triangles.Num() < 3 || (MeshData.Triangles.Num() % 3) != 0)
    {
        return false;
    }

    auto Builder = RealtimeMesh::TRealtimeMeshBuilderLocal<uint32>(OutStreams);
    Builder.EnableTangents();
    Builder.EnableTexCoords();
    Builder.EnableColors();
    Builder.EnablePolyGroups();

    const int32 VertexCount = MeshData.Vertices.Num();
    const int32 TriCount = MeshData.Triangles.Num() / 3;
    const bool bHasNormals = MeshData.HasNormals() && MeshData.Normals.Num() == VertexCount;
    const bool bHasUVs = MeshData.HasUVs();
    const bool bHasVertexColors = MeshData.HasVertexColors();

    TArray<FVector3f> ComputedNormals;
    if (!bHasNormals)
    {
        ComputedNormals.Init(FVector3f::ZeroVector, VertexCount);

        const int32* TrianglesPtr = MeshData.Triangles.GetData();
        for (int32 Face = 0; Face < TriCount; ++Face)
        {
            int32 baseIdx = Face * 3;
            int32 i0 = TrianglesPtr[baseIdx];
            int32 i1 = TrianglesPtr[baseIdx + 1];
            int32 i2 = TrianglesPtr[baseIdx + 2];

            if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= VertexCount || i1 >= VertexCount || i2 >= VertexCount)
            {
                continue;
            }

            const FVector3f V0(MeshData.Vertices[i0]);
            const FVector3f V1(MeshData.Vertices[i1]);
            const FVector3f V2(MeshData.Vertices[i2]);
            const FVector3f FaceNormal = FVector3f::CrossProduct(V1 - V0, V2 - V0);

            if (FaceNormal.SquaredLength() > 1.e-12f)
            {
                ComputedNormals[i0] += FaceNormal;
                ComputedNormals[i1] += FaceNormal;
                ComputedNormals[i2] += FaceNormal;
            }
        }

        for (FVector3f& N : ComputedNormals)
        {
            N = N.GetSafeNormal();
            if (N.SquaredLength() < 1.e-12f)
            {
                N = FVector3f(0.0f, 0.0f, 1.0f);
            }
        }
    }

    for (int32 i = 0; i < VertexCount; ++i)
    {
        Builder.AddVertex(FVector3f(MeshData.Vertices[i]));

        FVector3f N = bHasNormals && MeshData.Normals.IsValidIndex(i)
            ? FVector3f(MeshData.Normals[i])
            : ComputedNormals[i];
        if (N.SquaredLength() < 1.e-12f)
        {
            N = FVector3f(0.0f, 0.0f, 1.0f);
        }
        Builder.SetNormal(i, N);

        if (bHasUVs && MeshData.UVs.IsValidIndex(i))
        {
            Builder.SetTexCoord(i, 0, FVector2DHalf(FVector2f(MeshData.UVs[i])));
        }
        else
        {
            Builder.SetTexCoord(i, 0, FVector2DHalf(FVector2f::ZeroVector));
        }

        if (bHasVertexColors && MeshData.VertexColors.IsValidIndex(i))
        {
            Builder.SetColor(i, MeshData.VertexColors[i]);
        }
        else
        {
            Builder.SetColor(i, FColor::White);
        }
    }

    const int32* TrianglesPtr = MeshData.Triangles.GetData();
    for (int32 Face = 0; Face < TriCount; ++Face)
    {
        int32 baseIdx = Face * 3;
        int32 i0 = TrianglesPtr[baseIdx];
        int32 i1 = TrianglesPtr[baseIdx + 1];
        int32 i2 = TrianglesPtr[baseIdx + 2];

        if (i0 >= 0 && i1 >= 0 && i2 >= 0 &&
            i0 < VertexCount && i1 < VertexCount && i2 < VertexCount)
        {
            Builder.AddTriangle(i0, i1, i2);
        }
    }

    return true;
}

bool JUSYNCBuildRealtimeMeshStreams(const FJUSYNCCompactMeshData& MeshData, RealtimeMesh::FRealtimeMeshStreamSet& OutStreams)
{
    if (!MeshData.IsValid() || MeshData.Points.Num() == 0 ||
        MeshData.Triangles.Num() < 3 || (MeshData.Triangles.Num() % 3) != 0)
    {
        return false;
    }

    auto Builder = RealtimeMesh::TRealtimeMeshBuilderLocal<uint32>(OutStreams);
    Builder.EnableTangents();
    Builder.EnableTexCoords();
    Builder.EnableColors();
    Builder.EnablePolyGroups();

    const int32 VertexCount = MeshData.Points.Num();
    const int32 TriCount = MeshData.Triangles.Num() / 3;
    const bool bHasNormals = MeshData.HasNormals();
    const bool bHasUVs = MeshData.HasUVs();
    const bool bHasVertexColors = MeshData.HasVertexColors();

    TArray<FVector3f> ComputedNormals;
    if (!bHasNormals)
    {
        ComputedNormals.Init(FVector3f::ZeroVector, VertexCount);

        const int32* TrianglesPtr = MeshData.Triangles.GetData();
        for (int32 Face = 0; Face < TriCount; ++Face)
        {
            int32 baseIdx = Face * 3;
            int32 i0 = TrianglesPtr[baseIdx];
            int32 i1 = TrianglesPtr[baseIdx + 1];
            int32 i2 = TrianglesPtr[baseIdx + 2];

            if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= VertexCount || i1 >= VertexCount || i2 >= VertexCount)
            {
                continue;
            }

            const FVector3f V0(MeshData.Points[i0]);
            const FVector3f V1(MeshData.Points[i1]);
            const FVector3f V2(MeshData.Points[i2]);
            const FVector3f FaceNormal = FVector3f::CrossProduct(V1 - V0, V2 - V0);

            if (FaceNormal.SquaredLength() > 1.e-12f)
            {
                ComputedNormals[i0] += FaceNormal;
                ComputedNormals[i1] += FaceNormal;
                ComputedNormals[i2] += FaceNormal;
            }
        }

        for (FVector3f& N : ComputedNormals)
        {
            N = N.GetSafeNormal();
            if (N.SquaredLength() < 1.e-12f)
            {
                N = FVector3f(0.0f, 0.0f, 1.0f);
            }
        }
    }

    for (int32 i = 0; i < VertexCount; ++i)
    {
        Builder.AddVertex(MeshData.Points[i]);

        FVector3f N = bHasNormals && MeshData.Normals.IsValidIndex(i)
            ? MeshData.Normals[i]
            : ComputedNormals[i];
        if (N.SquaredLength() < 1.e-12f)
        {
            N = FVector3f(0.0f, 0.0f, 1.0f);
        }
        Builder.SetNormal(i, N);

        if (bHasUVs && MeshData.UVs.IsValidIndex(i))
        {
            Builder.SetTexCoord(i, 0, FVector2DHalf(MeshData.UVs[i]));
        }
        else
        {
            Builder.SetTexCoord(i, 0, FVector2DHalf(FVector2f::ZeroVector));
        }

        if (bHasVertexColors && MeshData.VertexColors.IsValidIndex(i))
        {
            Builder.SetColor(i, MeshData.VertexColors[i]);
        }
        else
        {
            Builder.SetColor(i, FColor::White);
        }
    }

    const int32* TrianglesPtr = MeshData.Triangles.GetData();
    for (int32 Face = 0; Face < TriCount; ++Face)
    {
        int32 baseIdx = Face * 3;
        int32 i0 = TrianglesPtr[baseIdx];
        int32 i1 = TrianglesPtr[baseIdx + 1];
        int32 i2 = TrianglesPtr[baseIdx + 2];

        if (i0 >= 0 && i1 >= 0 && i2 >= 0 &&
            i0 < VertexCount && i1 < VertexCount && i2 < VertexCount)
        {
            Builder.AddTriangle(i0, i1, i2);
        }
    }

    return true;
}
