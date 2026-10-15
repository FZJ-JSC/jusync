#include "JUSYNCTypes.h"

FJUSYNCRealtimeMeshData FJUSYNCRealtimeMeshData::FromStandardMesh(const FJUSYNCMeshData& StandardMesh)
{
    FJUSYNCRealtimeMeshData RealtimeMesh;
    RealtimeMesh.ElementName = StandardMesh.ElementName;

    RealtimeMesh.Vertices.Reserve(StandardMesh.GetVertexCount());

    for (int32 i = 0; i < StandardMesh.GetVertexCount(); ++i)
    {
        FJUSYNCRealtimeMeshVertex Vertex;

        Vertex.Position = StandardMesh.Vertices[i];

        if (i < StandardMesh.Normals.Num())
        {
            Vertex.Normal = StandardMesh.Normals[i];
        }
        else
        {
            Vertex.Normal = FVector::UpVector; // Default normal
        }

        if (i < StandardMesh.UVs.Num())
        {
            Vertex.UV = StandardMesh.UVs[i];
        }
        else
        {
            Vertex.UV = FVector2D::ZeroVector; // Default UV
        }

        if (i < StandardMesh.VertexColors.Num())
        {
            Vertex.Color = StandardMesh.VertexColors[i];
        }
        else
        {
            Vertex.Color = FColor::White; // Default color
        }

        RealtimeMesh.Vertices.Add(Vertex);
    }

    RealtimeMesh.Triangles = StandardMesh.Triangles;
    return RealtimeMesh;
}


FJUSYNCMeshData FJUSYNCRealtimeMeshData::ToStandardMesh() const
{
    FJUSYNCMeshData StandardMesh;
    StandardMesh.ElementName = ElementName;

    StandardMesh.Vertices.Reserve(Vertices.Num());
    StandardMesh.Normals.Reserve(Vertices.Num());
    StandardMesh.UVs.Reserve(Vertices.Num());

    for (const auto& Vertex : Vertices)
    {
        StandardMesh.Vertices.Add(Vertex.Position);
        StandardMesh.Normals.Add(Vertex.Normal);
        StandardMesh.UVs.Add(Vertex.UV);
    }

    StandardMesh.Triangles = Triangles;

    return StandardMesh;
}
