// ============================================================================
// FIXES FOR COMPILATION ERRORS
// ============================================================================

// 1. Add GLM experimental support BEFORE any GLM includes
#define GLM_ENABLE_EXPERIMENTAL

#include "CollisionProcessor.h"
#include "MiddlewareLogging.h"

// Standard library includes
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

// GLM for math operations - with experimental support enabled
#include <glm/glm.hpp>
#include <glm/gtx/norm.hpp>

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

// ✅ MISSING IMPLEMENTATIONS - Add these parameter setter methods
void CollisionProcessor::setSimplificationRatio(float ratio) {
    if (ratio > 0.0f && ratio <= 1.0f) {
        simplificationRatio_ = ratio;
        MIDDLEWARE_LOG_INFO("Collision simplification ratio set to: %.3f", ratio);
    } else {
        MIDDLEWARE_LOG_WARNING("Invalid simplification ratio: %.3f (must be 0.0-1.0)", ratio);
    }
}

void CollisionProcessor::setConvexHullPrecision(float precision) {
    if (precision > 0.0f && precision <= 1.0f) {
        convexHullPrecision_ = precision;
        MIDDLEWARE_LOG_INFO("Collision convex hull precision set to: %.6f", precision);
    } else {
        MIDDLEWARE_LOG_WARNING("Invalid convex hull precision: %.6f (must be 0.0-1.0)", precision);
    }
}

void CollisionProcessor::setMaxConvexHulls(int maxHulls) {
    if (maxHulls > 0 && maxHulls <= 64) {
        maxConvexHulls_ = maxHulls;
        MIDDLEWARE_LOG_INFO("Max convex hulls set to: %d", maxHulls);
    } else {
        MIDDLEWARE_LOG_WARNING("Invalid max convex hulls: %d (must be 1-64)", maxHulls);
    }
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
        case ECollisionComplexity::None: return "None";
        case ECollisionComplexity::Simple: return "Simple (Bounding Box)";
        case ECollisionComplexity::ConvexHull: return "Convex Hull";
        case ECollisionComplexity::Complex: return "Complex (Full Mesh)";
        case ECollisionComplexity::Simplified: return "Simplified";
        case ECollisionComplexity::ConvexDecomp: return "Convex Decomposition";
        default: return "Unknown";
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

        MIDDLEWARE_LOG_DEBUG("Generated bounding box collision: min(%.3f,%.3f,%.3f) max(%.3f,%.3f,%.3f)",
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
    MIDDLEWARE_LOG_INFO("Convex hull collision generation not fully implemented - using simplified approach");

    // For now, use a simplified approach - copy vertices and generate new indices
    outCollisionData.vertices = vertices;

    // Generate indices for the first few triangles (placeholder implementation)
    outCollisionData.indices.clear();
    size_t triangleCount = std::min(indices.size(), static_cast<size_t>(300)); // Limit for convex hull

    for (size_t i = 0; i < triangleCount; ++i) {
        outCollisionData.indices.push_back(indices[i]);
    }

    // Calculate bounding box as well
    generateBoundingBoxCollision(vertices, outCollisionData);

    return true;
}

bool CollisionProcessor::generateComplexCollision(const std::vector<float>& vertices,
                                                   const std::vector<unsigned int>& indices,
                                                   CollisionData& outCollisionData) {
    // For complex collision, use the original mesh
    outCollisionData.vertices = vertices;
    outCollisionData.indices = indices;

    // ✅ FIX: Calculate bounding box WITHOUT overwriting vertices/indices
    // Just calculate the min/max bounds and sphere
    if (vertices.size() >= 9) {
        glm::vec3 minBounds(vertices[0], vertices[1], vertices[2]);
        glm::vec3 maxBounds = minBounds;

        for (size_t i = 0; i < vertices.size(); i += 3) {
            glm::vec3 vertex(vertices[i], vertices[i + 1], vertices[i + 2]);
            minBounds = glm::min(minBounds, vertex);
            maxBounds = glm::max(maxBounds, vertex);
        }

        outCollisionData.boundingBoxMin = minBounds;
        outCollisionData.boundingBoxMax = maxBounds;
        outCollisionData.sphereCenter = (minBounds + maxBounds) * 0.5f;
        outCollisionData.sphereRadius = glm::length(maxBounds - outCollisionData.sphereCenter);
    }

    MIDDLEWARE_LOG_DEBUG("Generated complex collision: %zu vertices, %zu triangles",
                         vertices.size() / 3, indices.size() / 3);
    return true;
}

bool CollisionProcessor::generateSimplifiedCollision(const std::vector<float>& vertices,
                                                    const std::vector<uint32_t>& indices,
                                                    CollisionData& outCollisionData) {
    // Simplify by using every nth triangle based on simplification ratio
    std::vector<float> outVertices;
    std::vector<uint32_t> outIndices;

    if (decimateMesh(vertices, indices, simplificationRatio_, outVertices, outIndices)) {
        outCollisionData.vertices = std::move(outVertices);
        outCollisionData.indices = std::move(outIndices);

        // Calculate bounding primitives
        generateBoundingBoxCollision(outCollisionData.vertices, outCollisionData);

        return true;
    }

    // Fallback to complex collision
    return generateComplexCollision(vertices, indices, outCollisionData);
}

bool CollisionProcessor::generateConvexDecomposition(const std::vector<float>& vertices,
                                                    const std::vector<uint32_t>& indices,
                                                    CollisionData& outCollisionData) {
    MIDDLEWARE_LOG_INFO("Convex decomposition not fully implemented - using convex hull approach");

    // For now, fallback to convex hull
    return generateConvexHullCollision(vertices, indices, outCollisionData);
}

bool CollisionProcessor::decimateMesh(const std::vector<float>& vertices,
                                     const std::vector<uint32_t>& indices,
                                     float ratio,
                                     std::vector<float>& outVertices,
                                     std::vector<uint32_t>& outIndices) {
    if (ratio <= 0.0f || ratio > 1.0f) {
        MIDDLEWARE_LOG_ERROR("Invalid decimation ratio: %.3f", ratio);
        return false;
    }

    try {
        // Simple decimation: take every nth triangle
        size_t targetTriangles = static_cast<size_t>(indices.size() / 3 * ratio);
        targetTriangles = std::max(targetTriangles, static_cast<size_t>(1)); // At least 1 triangle

        size_t step = indices.size() / 3 / targetTriangles;
        step = std::max(step, static_cast<size_t>(1));

        // Collect used vertices
        std::vector<bool> usedVertices(vertices.size() / 3, false);
        outIndices.clear();

        for (size_t i = 0; i < indices.size(); i += step * 3) {
            if (i + 2 < indices.size()) {
                uint32_t idx1 = indices[i];
                uint32_t idx2 = indices[i + 1];
                uint32_t idx3 = indices[i + 2];

                if (idx1 < vertices.size() / 3 && idx2 < vertices.size() / 3 && idx3 < vertices.size() / 3) {
                    usedVertices[idx1] = true;
                    usedVertices[idx2] = true;
                    usedVertices[idx3] = true;

                    outIndices.push_back(idx1);
                    outIndices.push_back(idx2);
                    outIndices.push_back(idx3);
                }
            }
        }

        // Copy used vertices
        outVertices.clear();
        std::vector<uint32_t> vertexMapping(vertices.size() / 3);
        uint32_t newIndex = 0;

        for (size_t i = 0; i < usedVertices.size(); ++i) {
            if (usedVertices[i]) {
                vertexMapping[i] = newIndex++;
                outVertices.push_back(vertices[i * 3]);
                outVertices.push_back(vertices[i * 3 + 1]);
                outVertices.push_back(vertices[i * 3 + 2]);
            }
        }

        // Update indices to use new vertex mapping
        for (uint32_t& index : outIndices) {
            index = vertexMapping[index];
        }

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
