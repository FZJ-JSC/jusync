// ============================================================================
// FIXES FOR COMPILATION ERRORS
// ============================================================================

// 1. Add GLM experimental support BEFORE any GLM includes
#define GLM_ENABLE_EXPERIMENTAL

#include "CollisionProcessor.h"
#include "MiddlewareLogging.h"

// Standard library includes
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <queue>
#include <atomic>  // ✅ FIX: Add missing atomic include

// GLM for math operations - with experimental support enabled
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/norm.hpp>  // Now this will work with GLM_ENABLE_EXPERIMENTAL

namespace anari_usd_middleware {

// Constructor
CollisionProcessor::CollisionProcessor() {
    MIDDLEWARE_LOG_INFO("CollisionProcessor created with enhanced safety features");
}

// Destructor
CollisionProcessor::~CollisionProcessor() {
    MIDDLEWARE_LOG_INFO("CollisionProcessor destroyed - Generated %llu collisions with %llu errors",
                        static_cast<unsigned long long>(collisionsGenerated_.load()),
                        static_cast<unsigned long long>(generationErrors_.load()));
}

bool CollisionProcessor::generateCollision(const std::vector<float>& vertices,
                                          const std::vector<uint32_t>& indices,
                                          ECollisionComplexity complexity,
                                          CollisionData& outCollisionData) {
    // Clear output data first
    outCollisionData.clear();

    // Validate inputs
    if (vertices.empty() || vertices.size() % 3 != 0) {
        MIDDLEWARE_LOG_ERROR("Invalid vertex data: size=%zu (must be multiple of 3)", vertices.size());
        generationErrors_.fetch_add(1);
        return false;
    }

    if (!indices.empty() && indices.size() % 3 != 0) {
        MIDDLEWARE_LOG_ERROR("Invalid index data: size=%zu (must be multiple of 3)", indices.size());
        generationErrors_.fetch_add(1);
        return false;
    }

    if (vertices.size() > safety::MAX_MESH_VERTICES * 3) {
        MIDDLEWARE_LOG_ERROR("Too many vertices for collision: %zu (max: %zu)",
                            vertices.size() / 3, safety::MAX_MESH_VERTICES);
        generationErrors_.fetch_add(1);
        return false;
    }

    // Validate complexity
    if (!isValidComplexity(complexity)) {
        MIDDLEWARE_LOG_ERROR("Invalid collision complexity: %d", static_cast<int>(complexity));
        generationErrors_.fetch_add(1);
        return false;
    }

    // Set collision type
    outCollisionData.collisionType = complexity;

    MIDDLEWARE_LOG_INFO("Generating %s collision for %zu vertices, %zu triangles",
                       getComplexityName(complexity).c_str(),
                       vertices.size() / 3,
                       indices.size() / 3);

    try {
        bool result = false;

        switch (complexity) {
            case ECollisionComplexity::None:
                MIDDLEWARE_LOG_DEBUG("No collision generation requested");
                result = true;
                break;

            case ECollisionComplexity::Simple:
                result = generateBoundingBoxCollision(vertices, outCollisionData);
                break;

            case ECollisionComplexity::ConvexHull:
                result = generateConvexHullCollision(vertices, indices, outCollisionData);
                break;

            case ECollisionComplexity::Complex:
                result = generateComplexCollision(vertices, indices, outCollisionData);
                break;

            case ECollisionComplexity::Simplified:
                result = generateSimplifiedCollision(vertices, indices, outCollisionData);
                break;

            case ECollisionComplexity::ConvexDecomp:
                result = generateConvexDecomposition(vertices, indices, outCollisionData);
                break;

            default:
                MIDDLEWARE_LOG_ERROR("Unhandled collision complexity: %d", static_cast<int>(complexity));
                generationErrors_.fetch_add(1);
                return false;
        }

        if (result) {
            // Validate generated collision data
            if (!outCollisionData.isValid()) {
                MIDDLEWARE_LOG_ERROR("Generated collision data failed validation");
                generationErrors_.fetch_add(1);
                return false;
            }

            collisionsGenerated_.fetch_add(1);
            MIDDLEWARE_LOG_INFO("Successfully generated %s collision: %zu vertices, %zu triangles",
                               getComplexityName(complexity).c_str(),
                               outCollisionData.vertices.size() / 3,
                               outCollisionData.indices.size() / 3);
        } else {
            MIDDLEWARE_LOG_ERROR("Failed to generate %s collision", getComplexityName(complexity).c_str());
            generationErrors_.fetch_add(1);
        }

        return result;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in generateCollision: %s", e.what());
        outCollisionData.clear();
        generationErrors_.fetch_add(1);
        return false;
    }
}

bool CollisionProcessor::generateCollisionForMeshes(std::vector<MeshDataWithCollision>& meshes,
                                                   ECollisionComplexity complexity) {
    if (meshes.empty()) {
        MIDDLEWARE_LOG_WARNING("No meshes provided for collision generation");
        return true; // Not an error, just nothing to do
    }

    MIDDLEWARE_LOG_INFO("Generating %s collision for %zu meshes",
                       getComplexityName(complexity).c_str(), meshes.size());

    bool allSuccessful = true;
    size_t processedCount = 0;

    for (auto& mesh : meshes) {
        try {
            // Validate mesh data
            if (!mesh.isValid()) {
                MIDDLEWARE_LOG_WARNING("Skipping invalid mesh: %s", mesh.elementName.c_str());
                allSuccessful = false;
                continue;
            }

            // Generate collision for this mesh
            bool result = generateCollision(mesh.points, mesh.indices, complexity, mesh.collisionData);

            if (result) {
                processedCount++;
                MIDDLEWARE_LOG_DEBUG("Generated collision for mesh: %s", mesh.elementName.c_str());
            } else {
                MIDDLEWARE_LOG_WARNING("Failed to generate collision for mesh: %s", mesh.elementName.c_str());
                allSuccessful = false;
            }

        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception processing mesh '%s': %s", mesh.elementName.c_str(), e.what());
            allSuccessful = false;
        }
    }

    MIDDLEWARE_LOG_INFO("Collision generation complete: %zu/%zu meshes processed successfully",
                       processedCount, meshes.size());

    return allSuccessful;
}

// Static utility methods
bool CollisionProcessor::isValidComplexity(ECollisionComplexity complexity) {
    return complexity >= ECollisionComplexity::None &&
           complexity <= ECollisionComplexity::ConvexDecomp;
}

std::string CollisionProcessor::getComplexityName(ECollisionComplexity complexity) {
    switch (complexity) {
        case ECollisionComplexity::None:        return "None";
        case ECollisionComplexity::Simple:      return "Simple (Bounding Box)";
        case ECollisionComplexity::ConvexHull:  return "Convex Hull";
        case ECollisionComplexity::Complex:     return "Complex (Full Mesh)";
        case ECollisionComplexity::Simplified:  return "Simplified";
        case ECollisionComplexity::ConvexDecomp: return "Convex Decomposition";
        default:                                return "Unknown";
    }
}

// Private implementation methods
bool CollisionProcessor::generateBoundingBoxCollision(const std::vector<float>& vertices,
                                                     CollisionData& outCollisionData) {
    if (vertices.size() < 9) { // Need at least 3 vertices
        MIDDLEWARE_LOG_ERROR("Not enough vertices for bounding box: %zu", vertices.size() / 3);
        return false;
    }

    try {
        // Calculate bounding box
        glm::vec3 minBounds(vertices[0], vertices[1], vertices[2]);
        glm::vec3 maxBounds = minBounds;

        for (size_t i = 0; i < vertices.size(); i += 3) {
            glm::vec3 vertex(vertices[i], vertices[i + 1], vertices[i + 2]);

            // Validate vertex
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
                MIDDLEWARE_LOG_WARNING("Non-finite vertex detected, skipping");
                continue;
            }

            minBounds = glm::min(minBounds, vertex);
            maxBounds = glm::max(maxBounds, vertex);
        }

        // Store bounding box
        outCollisionData.boundingBoxMin = minBounds;
        outCollisionData.boundingBoxMax = maxBounds;

        // Calculate sphere (optional - for additional simple collision)
        glm::vec3 center = (minBounds + maxBounds) * 0.5f;
        float radius = glm::length(maxBounds - center);

        outCollisionData.sphereCenter = center;
        outCollisionData.sphereRadius = radius;

        // Generate box vertices (8 vertices)
        std::vector<glm::vec3> boxVertices = {
            glm::vec3(minBounds.x, minBounds.y, minBounds.z), // 0
            glm::vec3(maxBounds.x, minBounds.y, minBounds.z), // 1
            glm::vec3(maxBounds.x, maxBounds.y, minBounds.z), // 2
            glm::vec3(minBounds.x, maxBounds.y, minBounds.z), // 3
            glm::vec3(minBounds.x, minBounds.y, maxBounds.z), // 4
            glm::vec3(maxBounds.x, minBounds.y, maxBounds.z), // 5
            glm::vec3(maxBounds.x, maxBounds.y, maxBounds.z), // 6
            glm::vec3(minBounds.x, maxBounds.y, maxBounds.z)  // 7
        };

        // Convert to flat array
        outCollisionData.vertices.clear();
        outCollisionData.vertices.reserve(24); // 8 vertices * 3 components

        for (const auto& vertex : boxVertices) {
            outCollisionData.vertices.push_back(vertex.x);
            outCollisionData.vertices.push_back(vertex.y);
            outCollisionData.vertices.push_back(vertex.z);
        }

        // Generate box indices (12 triangles = 36 indices)
        std::vector<uint32_t> boxIndices = {
            // Bottom face
            0, 1, 2,  0, 2, 3,
            // Top face
            4, 7, 6,  4, 6, 5,
            // Front face
            0, 4, 5,  0, 5, 1,
            // Back face
            2, 6, 7,  2, 7, 3,
            // Left face
            0, 3, 7,  0, 7, 4,
            // Right face
            1, 5, 6,  1, 6, 2
        };

        outCollisionData.indices = std::move(boxIndices);

        MIDDLEWARE_LOG_DEBUG("Generated bounding box collision: min(%.2f,%.2f,%.2f) max(%.2f,%.2f,%.2f)",
                           minBounds.x, minBounds.y, minBounds.z,
                           maxBounds.x, maxBounds.y, maxBounds.z);

        return true;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in generateBoundingBoxCollision: %s", e.what());
        return false;
    }
}

bool CollisionProcessor::generateConvexHullCollision(const std::vector<float>& vertices,
                                                    const std::vector<uint32_t>& indices,
                                                    CollisionData& outCollisionData) {
    // For now, implement a simple convex hull using existing vertices
    // In a full implementation, you would use a proper convex hull algorithm like QuickHull

    try {
        MIDDLEWARE_LOG_INFO("Generating convex hull collision (simplified implementation)");

        // For this implementation, we'll create a simplified convex hull
        // by removing interior vertices and keeping only the outer shell

        // Step 1: Find extreme points in each direction
        if (vertices.size() < 12) { // Need at least 4 vertices
            MIDDLEWARE_LOG_ERROR("Not enough vertices for convex hull: %zu", vertices.size() / 3);
            return false;
        }

        std::vector<size_t> extremePoints;

        // Find min/max in each axis
        size_t minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;

        for (size_t i = 0; i < vertices.size(); i += 3) {
            size_t vertexIndex = i / 3;

            if (vertices[i] < vertices[minX * 3]) minX = vertexIndex;
            if (vertices[i] > vertices[maxX * 3]) maxX = vertexIndex;
            if (vertices[i + 1] < vertices[minY * 3 + 1]) minY = vertexIndex;
            if (vertices[i + 1] > vertices[maxY * 3 + 1]) maxY = vertexIndex;
            if (vertices[i + 2] < vertices[minZ * 3 + 2]) minZ = vertexIndex;
            if (vertices[i + 2] > vertices[maxZ * 3 + 2]) maxZ = vertexIndex;
        }

        // Add extreme points to our hull (remove duplicates)
        std::unordered_set<size_t> extremeSet = {minX, maxX, minY, maxY, minZ, maxZ};
        extremePoints.assign(extremeSet.begin(), extremeSet.end());

        // Copy extreme vertices
        outCollisionData.convexVertices.clear();
        outCollisionData.convexVertices.reserve(extremePoints.size() * 3);

        for (size_t vertexIndex : extremePoints) {
            size_t baseIndex = vertexIndex * 3;
            outCollisionData.convexVertices.push_back(vertices[baseIndex]);
            outCollisionData.convexVertices.push_back(vertices[baseIndex + 1]);
            outCollisionData.convexVertices.push_back(vertices[baseIndex + 2]);
        }

        // For simplicity, we'll also copy these to the main collision mesh
        outCollisionData.vertices = outCollisionData.convexVertices;

        // Generate a simple hull by connecting extreme points
        // This is a very simplified approach - a real convex hull would be more complex
        if (extremePoints.size() >= 4) {
            // Create tetrahedron-like structure from first 4 extreme points
            outCollisionData.convexIndices = {
                0, 1, 2,  0, 2, 3,  0, 3, 1,  1, 3, 2
            };
            outCollisionData.indices = outCollisionData.convexIndices;
        }

        MIDDLEWARE_LOG_INFO("Generated convex hull with %zu vertices and %zu triangles",
                          outCollisionData.convexVertices.size() / 3,
                          outCollisionData.convexIndices.size() / 3);

        return true;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in generateConvexHullCollision: %s", e.what());
        return false;
    }
}

bool CollisionProcessor::generateComplexCollision(const std::vector<float>& vertices,
                                                 const std::vector<uint32_t>& indices,
                                                 CollisionData& outCollisionData) {
    try {
        MIDDLEWARE_LOG_INFO("Generating complex collision (full mesh)");

        // For complex collision, we use the full mesh geometry
        // Validate the mesh first
        if (!validateCollisionMesh(vertices, indices)) {
            MIDDLEWARE_LOG_ERROR("Mesh validation failed for complex collision");
            return false;
        }

        // Direct copy of vertices and indices
        outCollisionData.vertices = vertices;
        outCollisionData.indices = indices;

        MIDDLEWARE_LOG_INFO("Generated complex collision with %zu vertices and %zu triangles",
                          vertices.size() / 3, indices.size() / 3);

        return true;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in generateComplexCollision: %s", e.what());
        return false;
    }
}

bool CollisionProcessor::generateSimplifiedCollision(const std::vector<float>& vertices,
                                                    const std::vector<uint32_t>& indices,
                                                    CollisionData& outCollisionData) {
    try {
        MIDDLEWARE_LOG_INFO("Generating simplified collision (decimated mesh)");

        // Validate inputs
        if (!validateCollisionMesh(vertices, indices)) {
            MIDDLEWARE_LOG_ERROR("Mesh validation failed for simplified collision");
            return false;
        }

        // Use mesh decimation to reduce triangle count
        std::vector<float> decimatedVertices;
        std::vector<uint32_t> decimatedIndices;

        bool result = decimateMesh(vertices, indices, simplificationRatio_,
                                  decimatedVertices, decimatedIndices);

        if (!result) {
            MIDDLEWARE_LOG_WARNING("Mesh decimation failed, using original mesh");
            decimatedVertices = vertices;
            decimatedIndices = indices;
        }

        outCollisionData.vertices = std::move(decimatedVertices);
        outCollisionData.indices = std::move(decimatedIndices);

        MIDDLEWARE_LOG_INFO("Generated simplified collision: %zu vertices, %zu triangles (%.1f%% of original)",
                          outCollisionData.vertices.size() / 3,
                          outCollisionData.indices.size() / 3,
                          (float(outCollisionData.indices.size()) / float(indices.size())) * 100.0f);

        return true;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in generateSimplifiedCollision: %s", e.what());
        return false;
    }
}

bool CollisionProcessor::generateConvexDecomposition(const std::vector<float>& vertices,
                                                    const std::vector<uint32_t>& indices,
                                                    CollisionData& outCollisionData) {
    try {
        MIDDLEWARE_LOG_INFO("Generating convex decomposition collision");

        // This is a placeholder for V-HACD integration
        // For now, we'll create multiple convex hulls by splitting the mesh

        MIDDLEWARE_LOG_WARNING("Convex decomposition not fully implemented - using simplified approach");

        // Fall back to convex hull for now
        return generateConvexHullCollision(vertices, indices, outCollisionData);

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in generateConvexDecomposition: %s", e.what());
        return false;
    }
}

// Helper methods implementation
void CollisionProcessor::calculateBoundingBox(const std::vector<float>& vertices,
                                             glm::vec3& minBounds,
                                             glm::vec3& maxBounds) {
    if (vertices.size() < 3) {
        minBounds = maxBounds = glm::vec3(0.0f);
        return;
    }

    minBounds = glm::vec3(vertices[0], vertices[1], vertices[2]);
    maxBounds = minBounds;

    for (size_t i = 3; i < vertices.size(); i += 3) {
        glm::vec3 vertex(vertices[i], vertices[i + 1], vertices[i + 2]);

        if (std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z)) {
            minBounds = glm::min(minBounds, vertex);
            maxBounds = glm::max(maxBounds, vertex);
        }
    }
}

bool CollisionProcessor::decimateMesh(const std::vector<float>& vertices,
                                     const std::vector<uint32_t>& indices,
                                     float ratio,
                                     std::vector<float>& outVertices,
                                     std::vector<uint32_t>& outIndices) {
    if (ratio <= 0.0f || ratio >= 1.0f) {
        MIDDLEWARE_LOG_ERROR("Invalid decimation ratio: %f", ratio);
        return false;
    }

    try {
        // Simple decimation: keep every Nth triangle
        size_t targetTriangles = static_cast<size_t>(indices.size() / 3 * ratio);
        size_t step = indices.size() / 3 / targetTriangles;

        if (step < 1) step = 1;

        std::unordered_set<uint32_t> usedVertices;
        std::vector<uint32_t> newIndices;

        // Keep every 'step'th triangle
        for (size_t i = 0; i < indices.size(); i += step * 3) {
            if (i + 2 >= indices.size()) break;

            uint32_t i0 = indices[i];
            uint32_t i1 = indices[i + 1];
            uint32_t i2 = indices[i + 2];

            // Validate indices
            if (i0 * 3 + 2 >= vertices.size() ||
                i1 * 3 + 2 >= vertices.size() ||
                i2 * 3 + 2 >= vertices.size()) {
                continue;
            }

            usedVertices.insert(i0);
            usedVertices.insert(i1);
            usedVertices.insert(i2);

            newIndices.push_back(i0);
            newIndices.push_back(i1);
            newIndices.push_back(i2);
        }

        // Copy used vertices (note: this is simplified - indices would need remapping in a full implementation)
        outVertices.clear();
        outVertices.reserve(usedVertices.size() * 3);

        for (uint32_t vertexIndex : usedVertices) {
            size_t baseIndex = vertexIndex * 3;
            outVertices.push_back(vertices[baseIndex]);
            outVertices.push_back(vertices[baseIndex + 1]);
            outVertices.push_back(vertices[baseIndex + 2]);
        }

        outIndices = std::move(newIndices);

        MIDDLEWARE_LOG_DEBUG("Decimated mesh: %zu -> %zu triangles (%.1f%%)",
                           indices.size() / 3, outIndices.size() / 3,
                           (float(outIndices.size()) / float(indices.size())) * 100.0f);

        return true;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in decimateMesh: %s", e.what());
        return false;
    }
}

bool CollisionProcessor::validateCollisionMesh(const std::vector<float>& vertices,
                                              const std::vector<uint32_t>& indices) {
    // Validate vertex count
    if (vertices.empty() || vertices.size() % 3 != 0) {
        MIDDLEWARE_LOG_ERROR("Invalid vertex data for collision mesh");
        return false;
    }

    // Validate index count
    if (!indices.empty() && indices.size() % 3 != 0) {
        MIDDLEWARE_LOG_ERROR("Invalid index data for collision mesh");
        return false;
    }

    // Check size limits
    if (vertices.size() / 3 > safety::MAX_MESH_VERTICES) {
        MIDDLEWARE_LOG_ERROR("Too many vertices for collision mesh: %zu", vertices.size() / 3);
        return false;
    }

    if (indices.size() > safety::MAX_MESH_INDICES) {
        MIDDLEWARE_LOG_ERROR("Too many indices for collision mesh: %zu", indices.size());
        return false;
    }

    // Validate finite values
    for (size_t i = 0; i < vertices.size(); i += 3) {
        if (!std::isfinite(vertices[i]) ||
            !std::isfinite(vertices[i + 1]) ||
            !std::isfinite(vertices[i + 2])) {
            MIDDLEWARE_LOG_ERROR("Non-finite vertex detected at index %zu", i / 3);
            return false;
        }
    }

    // Validate index bounds
    size_t vertexCount = vertices.size() / 3;
    for (uint32_t index : indices) {
        if (index >= vertexCount) {
            MIDDLEWARE_LOG_ERROR("Index out of bounds: %u >= %zu", index, vertexCount);
            return false;
        }
    }

    return true;
}

} // namespace anari_usd_middleware
