#include "JUSYNCTypes.h"

// Implementation of FJUSYNCRealtimeMeshData conversion methods
FJUSYNCRealtimeMeshData FJUSYNCRealtimeMeshData::FromStandardMesh(const FJUSYNCMeshData& StandardMesh)
{
    FJUSYNCRealtimeMeshData RealtimeMesh;
    RealtimeMesh.ElementName = StandardMesh.ElementName;

    // Convert flat arrays from your middleware to structured vertices for RealtimeMeshComponent
    RealtimeMesh.Vertices.Reserve(StandardMesh.GetVertexCount());

    for (int32 i = 0; i < StandardMesh.GetVertexCount(); ++i)
    {
        FJUSYNCRealtimeMeshVertex Vertex;

        // Position from your middleware's flat array format
        Vertex.Position = StandardMesh.Vertices[i];

        // Normal from your middleware's flat array format (if available)
        if (i < StandardMesh.Normals.Num())
        {
            Vertex.Normal = StandardMesh.Normals[i];
        }
        else
        {
            Vertex.Normal = FVector::UpVector; // Default normal
        }

        // UV from your middleware's flat array format (if available)
        if (i < StandardMesh.UVs.Num())
        {
            Vertex.UV = StandardMesh.UVs[i];
        }
        else
        {
            Vertex.UV = FVector2D::ZeroVector; // Default UV
        }

        // ✅ NEW: Vertex color from your middleware's vertex color data
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

    // Copy triangle indices directly (compatible format)
    RealtimeMesh.Triangles = StandardMesh.Triangles;
    return RealtimeMesh;
}


FJUSYNCMeshData FJUSYNCRealtimeMeshData::ToStandardMesh() const
{
    FJUSYNCMeshData StandardMesh;
    StandardMesh.ElementName = ElementName;

    // Convert structured vertices back to flat arrays for your middleware
    StandardMesh.Vertices.Reserve(Vertices.Num());
    StandardMesh.Normals.Reserve(Vertices.Num());
    StandardMesh.UVs.Reserve(Vertices.Num());

    for (const auto& Vertex : Vertices)
    {
        // Convert back to your middleware's flat array format
        StandardMesh.Vertices.Add(Vertex.Position);
        StandardMesh.Normals.Add(Vertex.Normal);
        StandardMesh.UVs.Add(Vertex.UV);
    }

    // Copy triangle indices directly (compatible format)
    StandardMesh.Triangles = Triangles;

    return StandardMesh;
}
