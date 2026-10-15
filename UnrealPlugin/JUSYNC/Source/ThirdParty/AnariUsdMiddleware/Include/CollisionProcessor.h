#pragma once

#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <chrono>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "MiddlewareLogging.h"

#ifndef ANARI_USD_MIDDLEWARE_API
#ifdef _WIN32
#ifdef ANARI_USD_MIDDLEWARE_EXPORTS
#define ANARI_USD_MIDDLEWARE_API __declspec(dllexport)
#else
#define ANARI_USD_MIDDLEWARE_API __declspec(dllimport)
#endif
#else
#define ANARI_USD_MIDDLEWARE_API __attribute__((visibility("default")))
#endif
#endif

namespace anari_usd_middleware {

enum class ECollisionComplexity : uint8_t {
    None = 0,           // No collision generation
    Simple = 1,         // Bounding box collision
    ConvexHull = 2,     // Convex hull around mesh
    Complex = 3,        // Full mesh collision (default)
    Simplified = 4,     // Decimated mesh for performance
    ConvexDecomp = 5    // V-HACD convex decomposition
};

struct ANARI_USD_MIDDLEWARE_API CollisionData {
    ECollisionComplexity collisionType = ECollisionComplexity::None;

    std::vector<float> vertices;    // Flat array [x,y,z, x,y,z, ...]
    std::vector<uint32_t> indices;  // Triangle indices

    glm::vec3 boundingBoxMin{0.0f};
    glm::vec3 boundingBoxMax{0.0f};
    glm::vec3 sphereCenter{0.0f};
    float sphereRadius = 0.0f;

    std::vector<float> convexVertices;
    std::vector<uint32_t> convexIndices;

    bool isValid() const {
        return collisionType != ECollisionComplexity::None &&
               (vertices.size() % 3 == 0) &&
               (indices.size() % 3 == 0) &&
               vertices.size() <= safety::MAX_MESH_VERTICES * 3 &&
               indices.size() <= safety::MAX_MESH_INDICES &&
               validateFiniteValues();
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

    size_t getVertexCount() const {
        return vertices.size() / 3;
    }

    size_t getTriangleCount() const {
        return indices.size() / 3;
    }

    bool hasConvexHull() const {
        return !convexVertices.empty() && !convexIndices.empty();
    }

    size_t getMemoryUsage() const {
        return vertices.size() * sizeof(float) +
               indices.size() * sizeof(uint32_t) +
               convexVertices.size() * sizeof(float) +
               convexIndices.size() * sizeof(uint32_t) +
               sizeof(*this);
    }

private:
    bool validateFiniteValues() const {
        for (size_t i = 0; i < vertices.size(); ++i) {
            if (!std::isfinite(vertices[i])) {
                return false;
            }
        }
        for (size_t i = 0; i < convexVertices.size(); ++i) {
            if (!std::isfinite(convexVertices[i])) {
                return false;
            }
        }
        return std::isfinite(boundingBoxMin.x) && std::isfinite(boundingBoxMin.y) && std::isfinite(boundingBoxMin.z) &&
               std::isfinite(boundingBoxMax.x) && std::isfinite(boundingBoxMax.y) && std::isfinite(boundingBoxMax.z) &&
               std::isfinite(sphereCenter.x) && std::isfinite(sphereCenter.y) && std::isfinite(sphereCenter.z) &&
               std::isfinite(sphereRadius);
    }
};

struct ANARI_USD_MIDDLEWARE_API MeshDataWithCollision {
    std::string elementName;
    std::string typeName;
    std::vector<float> points;
    std::vector<uint32_t> indices;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<float> vertex_colors;

    CollisionData collisionData;

    bool isValid() const {
        return !elementName.empty() &&
               !points.empty() &&
               !indices.empty() &&
               (points.size() % 3 == 0) &&
               (indices.size() % 3 == 0) &&
               points.size() <= safety::MAX_MESH_VERTICES * 3 &&
               indices.size() <= safety::MAX_MESH_INDICES;
    }

    size_t getVisualVertexCount() const { return points.size() / 3; }
    size_t getVisualTriangleCount() const { return indices.size() / 3; }
    bool hasNormals() const { return !normals.empty(); }
    bool hasUVs() const { return !uvs.empty(); }
    bool hasVertexColors() const { return !vertex_colors.empty(); }
    bool hasCollision() const { return collisionData.isValid(); }

    void clear() {
        elementName.clear();
        typeName.clear();
        points.clear();
        indices.clear();
        normals.clear();
        uvs.clear();
        vertex_colors.clear();
        collisionData.clear();
    }
};

class ANARI_USD_MIDDLEWARE_API CollisionProcessor {
public:
    CollisionProcessor();
    ~CollisionProcessor();

    CollisionProcessor(const CollisionProcessor&) = delete;
    CollisionProcessor& operator=(const CollisionProcessor&) = delete;
    CollisionProcessor(CollisionProcessor&&) = delete;
    CollisionProcessor& operator=(CollisionProcessor&&) = delete;

    bool generateCollision(const std::vector<float>& vertices,
                          const std::vector<uint32_t>& indices,
                          ECollisionComplexity complexity,
                          CollisionData& outCollisionData);

    bool generateCollisionForMeshes(std::vector<MeshDataWithCollision>& meshes,
                                   ECollisionComplexity complexity);

    bool generateCollisionForMesh(MeshDataWithCollision& mesh,
                                 ECollisionComplexity complexity);

    static bool isValidComplexity(ECollisionComplexity complexity);

    static std::string getComplexityName(ECollisionComplexity complexity);

    static std::string getComplexityDescription(ECollisionComplexity complexity);

    static std::string getComplexityRecommendation(ECollisionComplexity complexity);

    void setSimplificationRatio(float ratio);
    void setConvexHullPrecision(float precision);
    void setMaxConvexHulls(int maxHulls);
    void setMaxProcessingTime(float seconds);
    void setQualityVsPerformance(float balance); // 0.0 = performance, 1.0 = quality

    float getSimplificationRatio() const { return simplificationRatio_; }
    float getConvexHullPrecision() const { return convexHullPrecision_; }
    int getMaxConvexHulls() const { return maxConvexHulls_; }
    float getMaxProcessingTime() const { return maxProcessingTime_; }
    float getQualityVsPerformance() const { return qualityVsPerformance_; }

    struct ProcessingStats {
        uint64_t collisionsGenerated;
        uint64_t generationErrors;
        uint64_t totalVerticesProcessed;
        uint64_t totalTrianglesProcessed;
        double averageProcessingTime;
        double totalProcessingTime;
    };

    ProcessingStats getProcessingStats() const;
    void resetProcessingStats();

    void setMultiThreadingEnabled(bool enabled) { multiThreadingEnabled_ = enabled; }
    bool isMultiThreadingEnabled() const { return multiThreadingEnabled_; }

    void setProgressCallback(std::function<void(float)> callback) {
        progressCallback_ = callback;
    }

private:
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

    void calculateBoundingBox(const std::vector<float>& vertices,
                             glm::vec3& minBounds,
                             glm::vec3& maxBounds);

    void calculateBoundingSphere(const std::vector<float>& vertices,
                                glm::vec3& center,
                                float& radius);

    bool decimateMesh(const std::vector<float>& vertices,
                     const std::vector<uint32_t>& indices,
                     float ratio,
                     std::vector<float>& outVertices,
                     std::vector<uint32_t>& outIndices);

    bool validateCollisionMesh(const std::vector<float>& vertices,
                              const std::vector<uint32_t>& indices);

    bool validateInputMesh(const std::vector<float>& vertices,
                          const std::vector<uint32_t>& indices,
                          const std::string& context);

    void optimizeCollisionMesh(std::vector<float>& vertices,
                              std::vector<uint32_t>& indices);

    void reportProgress(float progress);
    void updateProcessingStats(bool success, size_t vertexCount, size_t triangleCount, double processingTime);

    std::atomic<float> simplificationRatio_{0.25f};    // 25% of original triangles
    std::atomic<float> convexHullPrecision_{0.001f};   // Precision for convex hull
    std::atomic<int> maxConvexHulls_{32};               // Max hulls for decomposition
    std::atomic<float> maxProcessingTime_{30.0f};      // Max processing time in seconds
    std::atomic<float> qualityVsPerformance_{0.5f};    // Quality vs performance balance
    std::atomic<bool> multiThreadingEnabled_{true};    // Enable multi-threading

    mutable std::atomic<uint64_t> collisionsGenerated_{0};
    mutable std::atomic<uint64_t> generationErrors_{0};
    mutable std::atomic<uint64_t> totalVerticesProcessed_{0};
    mutable std::atomic<uint64_t> totalTrianglesProcessed_{0};
    mutable std::atomic<double> totalProcessingTime_{0.0};

    std::function<void(float)> progressCallback_;
    mutable std::mutex callbackMutex_;

    std::atomic<bool> processingInProgress_{false};
    mutable std::chrono::steady_clock::time_point lastStatsUpdate_;
};

namespace CollisionUtils {
    ANARI_USD_MIDDLEWARE_API bool ConvertToUnrealFormat(const CollisionData& collisionData,
                                                        void* unrealBodySetup);

    ANARI_USD_MIDDLEWARE_API double EstimateProcessingTime(size_t vertexCount,
                                                          size_t triangleCount,
                                                          ECollisionComplexity complexity);

    ANARI_USD_MIDDLEWARE_API ECollisionComplexity GetRecommendedComplexity(size_t vertexCount,
                                                                           size_t triangleCount,
                                                                           bool isStaticMesh = true);

    ANARI_USD_MIDDLEWARE_API bool ValidateMeshForCollision(const std::vector<float>& vertices,
                                                          const std::vector<uint32_t>& indices,
                                                          std::string& errorMessage);
}

} // namespace anari_usd_middleware
