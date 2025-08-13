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

// Add DLL export macro definition if not already defined
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
 * Designed to integrate seamlessly with RealtimeMeshComponent and Physics System
 */
struct ANARI_USD_MIDDLEWARE_API CollisionData {
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

    // Enhanced validation with comprehensive safety checks
    bool isValid() const {
        return collisionType != ECollisionComplexity::None &&
               (vertices.size() % 3 == 0) &&
               (indices.size() % 3 == 0) &&
               vertices.size() <= safety::MAX_MESH_VERTICES * 3 &&
               indices.size() <= safety::MAX_MESH_INDICES &&
               validateFiniteValues();
    }

    // Clear all collision data safely
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

    // Get collision vertex count
    size_t getVertexCount() const {
        return vertices.size() / 3;
    }

    // Get collision triangle count
    size_t getTriangleCount() const {
        return indices.size() / 3;
    }

    // Check if has convex hull data
    bool hasConvexHull() const {
        return !convexVertices.empty() && !convexIndices.empty();
    }

    // Get memory usage estimate
    size_t getMemoryUsage() const {
        return vertices.size() * sizeof(float) +
               indices.size() * sizeof(uint32_t) +
               convexVertices.size() * sizeof(float) +
               convexIndices.size() * sizeof(uint32_t) +
               sizeof(*this);
    }

private:
    // Validate that all vertex coordinates are finite
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

/**
 * Enhanced mesh data structure that includes collision information
 * Seamlessly integrates with your existing JUSYNC pipeline
 */
struct ANARI_USD_MIDDLEWARE_API MeshDataWithCollision {
    // Original visual mesh data (unchanged from your existing structure)
    std::string elementName;
    std::string typeName;
    std::vector<float> points;
    std::vector<uint32_t> indices;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<float> vertex_colors;

    // NEW: Collision data
    CollisionData collisionData;

    // Enhanced validation
    bool isValid() const {
        return !elementName.empty() &&
               !points.empty() &&
               !indices.empty() &&
               (points.size() % 3 == 0) &&
               (indices.size() % 3 == 0) &&
               points.size() <= safety::MAX_MESH_VERTICES * 3 &&
               indices.size() <= safety::MAX_MESH_INDICES;
    }

    // Get visual mesh statistics
    size_t getVisualVertexCount() const { return points.size() / 3; }
    size_t getVisualTriangleCount() const { return indices.size() / 3; }
    bool hasNormals() const { return !normals.empty(); }
    bool hasUVs() const { return !uvs.empty(); }
    bool hasVertexColors() const { return !vertex_colors.empty(); }
    bool hasCollision() const { return collisionData.isValid(); }

    // Clear all data
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

/**
 * Main collision processor class with proper DLL export decoration
 * Thread-safe and optimized for real-time collision generation
 */
class ANARI_USD_MIDDLEWARE_API CollisionProcessor {
public:
    // Constructor/Destructor with enhanced safety
    CollisionProcessor();
    ~CollisionProcessor();

    // Disable copy/move for safety and resource management
    CollisionProcessor(const CollisionProcessor&) = delete;
    CollisionProcessor& operator=(const CollisionProcessor&) = delete;
    CollisionProcessor(CollisionProcessor&&) = delete;
    CollisionProcessor& operator=(CollisionProcessor&&) = delete;

    /**
     * Generate collision data from visual mesh
     * Main function for collision generation with comprehensive error handling
     *
     * @param vertices Input visual mesh vertices (flat array x,y,z,x,y,z...)
     * @param indices Input visual mesh triangle indices
     * @param complexity Desired collision complexity
     * @param outCollisionData Output collision data structure
     * @return True if generation succeeded, false otherwise
     */
    bool generateCollision(const std::vector<float>& vertices,
                          const std::vector<uint32_t>& indices,
                          ECollisionComplexity complexity,
                          CollisionData& outCollisionData);

    /**
     * Generate collision for multiple meshes (batch processing)
     * Optimized for processing multiple meshes from USD files
     *
     * @param meshes Input/output mesh array with collision data populated
     * @param complexity Collision complexity to apply to all meshes
     * @return True if all collisions generated successfully
     */
    bool generateCollisionForMeshes(std::vector<MeshDataWithCollision>& meshes,
                                   ECollisionComplexity complexity);

    /**
     * Generate collision for single enhanced mesh structure
     * Convenience function for single mesh processing
     *
     * @param mesh Input/output mesh with collision data populated
     * @param complexity Collision complexity to apply
     * @return True if collision generated successfully
     */
    bool generateCollisionForMesh(MeshDataWithCollision& mesh,
                                 ECollisionComplexity complexity);

    /**
     * ✅ FIXED: Remove redundant export decoration from static functions
     * The class-level ANARI_USD_MIDDLEWARE_API export covers all public members
     */
    static bool isValidComplexity(ECollisionComplexity complexity);

    /**
     * ✅ FIXED: Get string name for collision complexity
     * Essential for Unreal Engine Blueprint integration and debugging
     */
    static std::string getComplexityName(ECollisionComplexity complexity);

    /**
     * ✅ FIXED: Get detailed description for collision complexity
     * Useful for tooltips and documentation in Unreal Editor
     */
    static std::string getComplexityDescription(ECollisionComplexity complexity);

    /**
     * ✅ FIXED: Get recommended use case for collision complexity
     * Helps users choose appropriate complexity for their needs
     */
    static std::string getComplexityRecommendation(ECollisionComplexity complexity);

    /**
     * Set collision generation parameters for fine-tuning
     * Advanced configuration for different quality/performance trade-offs
     */
    void setSimplificationRatio(float ratio);
    void setConvexHullPrecision(float precision);
    void setMaxConvexHulls(int maxHulls);
    void setMaxProcessingTime(float seconds);
    void setQualityVsPerformance(float balance); // 0.0 = performance, 1.0 = quality

    /**
     * Get current configuration parameters
     */
    float getSimplificationRatio() const { return simplificationRatio_; }
    float getConvexHullPrecision() const { return convexHullPrecision_; }
    int getMaxConvexHulls() const { return maxConvexHulls_; }
    float getMaxProcessingTime() const { return maxProcessingTime_; }
    float getQualityVsPerformance() const { return qualityVsPerformance_; }

    /**
     * Get processing statistics
     * Useful for performance monitoring and optimization
     */
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

    /**
     * Enable or disable multi-threading
     * @param enabled True to enable multi-threading for batch operations
     */
    void setMultiThreadingEnabled(bool enabled) { multiThreadingEnabled_ = enabled; }
    bool isMultiThreadingEnabled() const { return multiThreadingEnabled_; }

    /**
     * Set progress callback for long operations
     * @param callback Function to call with progress updates (0.0 to 1.0)
     */
    void setProgressCallback(std::function<void(float)> callback) {
        progressCallback_ = callback;
    }

private:
    // Internal collision generation methods with enhanced error handling
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

    // Enhanced helper methods
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

    // Progress reporting
    void reportProgress(float progress);
    void updateProcessingStats(bool success, size_t vertexCount, size_t triangleCount, double processingTime);

    // Configuration parameters with thread safety
    std::atomic<float> simplificationRatio_{0.25f};    // 25% of original triangles
    std::atomic<float> convexHullPrecision_{0.001f};   // Precision for convex hull
    std::atomic<int> maxConvexHulls_{32};               // Max hulls for decomposition
    std::atomic<float> maxProcessingTime_{30.0f};      // Max processing time in seconds
    std::atomic<float> qualityVsPerformance_{0.5f};    // Quality vs performance balance
    std::atomic<bool> multiThreadingEnabled_{true};    // Enable multi-threading

    // Statistics with thread safety
    mutable std::atomic<uint64_t> collisionsGenerated_{0};
    mutable std::atomic<uint64_t> generationErrors_{0};
    mutable std::atomic<uint64_t> totalVerticesProcessed_{0};
    mutable std::atomic<uint64_t> totalTrianglesProcessed_{0};
    mutable std::atomic<double> totalProcessingTime_{0.0};

    // Progress callback
    std::function<void(float)> progressCallback_;
    mutable std::mutex callbackMutex_;

    // Internal state
    std::atomic<bool> processingInProgress_{false};
    mutable std::chrono::steady_clock::time_point lastStatsUpdate_;
};

// Utility functions for Unreal Engine integration
namespace CollisionUtils {
    /**
     * Convert CollisionData to Unreal Engine format
     * Helper for integration with UBodySetup and physics system
     */
    ANARI_USD_MIDDLEWARE_API bool ConvertToUnrealFormat(const CollisionData& collisionData,
                                                        void* unrealBodySetup);

    /**
     * Estimate collision generation time
     * Helps users choose appropriate complexity based on mesh size
     */
    ANARI_USD_MIDDLEWARE_API double EstimateProcessingTime(size_t vertexCount,
                                                          size_t triangleCount,
                                                          ECollisionComplexity complexity);

    /**
     * Get recommended collision complexity for mesh size
     * Automatic complexity selection based on mesh characteristics
     */
    ANARI_USD_MIDDLEWARE_API ECollisionComplexity GetRecommendedComplexity(size_t vertexCount,
                                                                           size_t triangleCount,
                                                                           bool isStaticMesh = true);

    /**
     * Validate mesh for collision generation
     * Pre-check to avoid processing invalid meshes
     */
    ANARI_USD_MIDDLEWARE_API bool ValidateMeshForCollision(const std::vector<float>& vertices,
                                                          const std::vector<uint32_t>& indices,
                                                          std::string& errorMessage);
}

} // namespace anari_usd_middleware
