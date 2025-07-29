#pragma once

#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <glm/glm.hpp>
#include "MiddlewareLogging.h"

namespace anari_usd_middleware {

/**
 * Collision complexity options for different use cases
 * These will be exposed to Unreal Engine Blueprints
 */
enum class ECollisionComplexity : uint8_t {
    None = 0,           // No collision generation
    Simple = 1,         // Bounding box collision
    ConvexHull = 2,     // Convex hull around mesh
    Complex = 3,        // Full mesh collision (default)
    Simplified = 4,     // Decimated mesh for performance
    ConvexDecomp = 5    // V-HACD convex decomposition
};

/**
 * Collision data structure compatible with Unreal Engine
 */
struct CollisionData {
    ECollisionComplexity collisionType = ECollisionComplexity::None;

    // Collision mesh data (similar to visual mesh)
    std::vector<float> vertices;    // Flat array [x,y,z, x,y,z, ...]
    std::vector<uint32_t> indices;  // Triangle indices

    // Simple collision data (for bounding boxes, spheres, etc.)
    glm::vec3 boundingBoxMin{0.0f};
    glm::vec3 boundingBoxMax{0.0f};
    glm::vec3 sphereCenter{0.0f};
    float sphereRadius = 0.0f;

    // Convex hull data (for convex collision)
    std::vector<float> convexVertices;
    std::vector<uint32_t> convexIndices;

    // Validation
    bool isValid() const {
        return collisionType != ECollisionComplexity::None &&
               (vertices.size() % 3 == 0) &&
               (indices.size() % 3 == 0) &&
               vertices.size() <= safety::MAX_MESH_VERTICES * 3 &&
               indices.size() <= safety::MAX_MESH_INDICES;
    }

    void clear() {
        collisionType = ECollisionComplexity::None;
        vertices.clear();
        indices.clear();
        convexVertices.clear();
        convexIndices.clear();
        boundingBoxMin = glm::vec3(0.0f);
        boundingBoxMax = glm::vec3(0.0f);
        sphereCenter = glm::vec3(0.0f);
        sphereRadius = 0.0f;
    }
};

/**
 * Enhanced mesh data structure that includes collision information
 */
struct MeshDataWithCollision {
    // Original visual mesh data
    std::string elementName;
    std::string typeName;
    std::vector<float> points;
    std::vector<uint32_t> indices;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<float> vertex_colors;

    // NEW: Collision data
    CollisionData collisionData;

    bool isValid() const {
        return !elementName.empty() &&
               !points.empty() &&
               !indices.empty() &&
               (points.size() % 3 == 0) &&
               (indices.size() % 3 == 0);
    }
};

/**
 * Main collision processor class
 */
class CollisionProcessor {
public:
    // Constructor/Destructor
    CollisionProcessor();
    ~CollisionProcessor();

    // Disable copy/move for safety
    CollisionProcessor(const CollisionProcessor&) = delete;
    CollisionProcessor& operator=(const CollisionProcessor&) = delete;

    /**
     * Generate collision data from visual mesh
     * @param visualMesh Input visual mesh data
     * @param complexity Desired collision complexity
     * @param outCollisionData Output collision data
     * @return True if generation succeeded
     */
    bool generateCollision(const std::vector<float>& vertices,
                          const std::vector<uint32_t>& indices,
                          ECollisionComplexity complexity,
                          CollisionData& outCollisionData);

    /**
     * Generate collision for multiple meshes
     * @param meshes Input mesh array
     * @param complexity Collision complexity to apply
     * @return True if all collisions generated successfully
     */
    bool generateCollisionForMeshes(std::vector<MeshDataWithCollision>& meshes,
                                   ECollisionComplexity complexity);

    /**
     * Validate collision complexity option
     * @param complexity Complexity to validate
     * @return True if valid option
     */
    static bool isValidComplexity(ECollisionComplexity complexity);

    /**
     * Get string name for collision complexity (for debugging)
     * @param complexity Complexity enum value
     * @return Human-readable string
     */
    static std::string getComplexityName(ECollisionComplexity complexity);

    /**
     * Set collision generation parameters
     */
    void setSimplificationRatio(float ratio) { simplificationRatio_ = ratio; }
    void setConvexHullPrecision(float precision) { convexHullPrecision_ = precision; }
    void setMaxConvexHulls(int maxHulls) { maxConvexHulls_ = maxHulls; }

private:
    // Internal collision generation methods
    bool generateBoundingBoxCollision(const std::vector<float>& vertices,
                                     CollisionData& outCollisionData);

    bool generateConvexHullCollision(const std::vector<float>& vertices,
                                    const std::vector<uint32_t>& indices,
                                    CollisionData& outCollisionData);

    bool generateComplexCollision(const std::vector<float>& vertices,
                                 const std::vector<uint32_t>& indices,
                                 CollisionData& outCollisionData);

    bool generateSimplifiedCollision(const std::vector<float>& vertices,
                                    const std::vector<uint32_t>& indices,
                                    CollisionData& outCollisionData);

    bool generateConvexDecomposition(const std::vector<float>& vertices,
                                    const std::vector<uint32_t>& indices,
                                    CollisionData& outCollisionData);

    // Helper methods
    void calculateBoundingBox(const std::vector<float>& vertices,
                             glm::vec3& minBounds,
                             glm::vec3& maxBounds);

    bool decimateMesh(const std::vector<float>& vertices,
                     const std::vector<uint32_t>& indices,
                     float ratio,
                     std::vector<float>& outVertices,
                     std::vector<uint32_t>& outIndices);

    bool validateCollisionMesh(const std::vector<float>& vertices,
                              const std::vector<uint32_t>& indices);

    // Configuration parameters
    float simplificationRatio_ = 0.25f;    // 25% of original triangles
    float convexHullPrecision_ = 0.001f;   // Precision for convex hull
    int maxConvexHulls_ = 32;              // Max hulls for decomposition

    // Statistics
    std::atomic<uint64_t> collisionsGenerated_{0};
    std::atomic<uint64_t> generationErrors_{0};
};

} // namespace anari_usd_middleware
