#pragma once

#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "MiddlewareLogging.h"

#ifndef ANARI_USD_MIDDLEWARE_API
#ifdef _WIN32
#define ANARI_USD_MIDDLEWARE_API __declspec(dllexport)
#else
#define ANARI_USD_MIDDLEWARE_API __attribute__((visibility("default")))
#endif
#endif

// Forward declarations for TinyUSDZ
namespace tinyusdz {
    class Prim;
    class Stage;
    class GeomMesh;
}

namespace anari_usd_middleware {

/**
 * Thread-safe USD processing engine with comprehensive error handling and memory safety
 * features for Unreal Engine 5.5 compatibility. Handles USD file conversion, mesh extraction,
 * texture processing, and reference resolution with robust validation.
 */
class ANARI_USD_MIDDLEWARE_API UsdProcessor {
public:
    /**
     * Enhanced mesh data structure with validation and bounds checking
     */
    struct MeshData {
        std::string elementName;        ///< Name of the USD element (validated)
        std::string typeName;           ///< Type of the USD element (validated)
        std::vector<glm::vec3> points;  ///< 3D vertex positions (size-limited)
        std::vector<uint32_t> indices;  ///< Triangle indices (validated)
        std::vector<glm::vec3> normals; ///< Normal vectors (normalized)
        std::vector<glm::vec2> uvs;     ///< Texture coordinates (clamped)

        // Validation methods
        bool isValid() const {
            return !elementName.empty() &&
                   points.size() <= safety::MAX_MESH_VERTICES &&
                   indices.size() <= safety::MAX_MESH_INDICES &&
                   normals.size() <= safety::MAX_MESH_VERTICES &&
                   uvs.size() <= safety::MAX_MESH_VERTICES &&
                   (indices.size() % 3 == 0) &&
                   validateGeometry();
        }

        size_t getVertexCount() const { return points.size(); }
        size_t getTriangleCount() const { return indices.size() / 3; }
        bool hasNormals() const { return !normals.empty(); }
        bool hasUVs() const { return !uvs.empty(); }

        // Calculate bounding box
        std::pair<glm::vec3, glm::vec3> getBounds() const;

        // Validate mesh geometry integrity
        bool validateGeometry() const;

        // Clear all data safely
        void clear() {
            elementName.clear();
            typeName.clear();
            points.clear();
            indices.clear();
            normals.clear();
            uvs.clear();
        }
    };

    /**
     * Enhanced texture data structure with comprehensive validation
     */
    struct TextureData {
        int width = 0;                  ///< Width in pixels (validated > 0)
        int height = 0;                 ///< Height in pixels (validated > 0)
        int channels = 0;               ///< Number of color channels (1-4)
        std::vector<uint8_t> data;      ///< Raw pixel data (size-validated)

        // Validation methods
        bool isValid() const {
            return width > 0 && height > 0 &&
                   channels > 0 && channels <= 4 &&
                   !data.empty() &&
                   data.size() == getExpectedDataSize() &&
                   width <= 16384 && height <= 16384; // Reasonable texture limits
        }

        size_t getExpectedDataSize() const {
            return static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels);
        }

        size_t getMemoryUsage() const {
            return data.size() + sizeof(*this);
        }

        // Clear texture data safely
        void clear() {
            width = height = channels = 0;
            data.clear();
        }
    };

    /**
     * Processing statistics for monitoring and debugging - FIXED VERSION
     */
    struct ProcessingStats {
        std::atomic<uint64_t> filesProcessed{0};
        std::atomic<uint64_t> meshesExtracted{0};
        std::atomic<uint64_t> texturesProcessed{0};
        std::atomic<uint64_t> referencesResolved{0};
        std::atomic<uint64_t> processingErrors{0};
        std::atomic<uint64_t> totalBytesProcessed{0};

        // Delete copy constructor and assignment operator since atomics can't be copied
        ProcessingStats() = default;
        ProcessingStats(const ProcessingStats&) = delete;
        ProcessingStats& operator=(const ProcessingStats&) = delete;

        // Move constructor and assignment
        ProcessingStats(ProcessingStats&& other) noexcept {
            filesProcessed.store(other.filesProcessed.load());
            meshesExtracted.store(other.meshesExtracted.load());
            texturesProcessed.store(other.texturesProcessed.load());
            referencesResolved.store(other.referencesResolved.load());
            processingErrors.store(other.processingErrors.load());
            totalBytesProcessed.store(other.totalBytesProcessed.load());
        }

        ProcessingStats& operator=(ProcessingStats&& other) noexcept {
            if (this != &other) {
                filesProcessed.store(other.filesProcessed.load());
                meshesExtracted.store(other.meshesExtracted.load());
                texturesProcessed.store(other.texturesProcessed.load());
                referencesResolved.store(other.referencesResolved.load());
                processingErrors.store(other.processingErrors.load());
                totalBytesProcessed.store(other.totalBytesProcessed.load());
            }
            return *this;
        }

        void reset() {
            filesProcessed.store(0);
            meshesExtracted.store(0);
            texturesProcessed.store(0);
            referencesResolved.store(0);
            processingErrors.store(0);
            totalBytesProcessed.store(0);
        }

        // Create a copyable snapshot for returning from functions
        struct Snapshot {
            uint64_t filesProcessed;
            uint64_t meshesExtracted;
            uint64_t texturesProcessed;
            uint64_t referencesResolved;
            uint64_t processingErrors;
            uint64_t totalBytesProcessed;
        };

        Snapshot getSnapshot() const {
            return {
                filesProcessed.load(),
                meshesExtracted.load(),
                texturesProcessed.load(),
                referencesResolved.load(),
                processingErrors.load(),
                totalBytesProcessed.load()
            };
        }
    };

    /**
     * Progress callback for long-running operations
     */
    using ProgressCallback = std::function<void(float progress, const std::string& status)>;

    /**
     * Constructor with enhanced initialization
     */
    UsdProcessor();

    /**
     * Destructor with safe cleanup
     */
    ~UsdProcessor();

    // Disable copy constructor and assignment operator for safety
    UsdProcessor(const UsdProcessor&) = delete;
    UsdProcessor& operator=(const UsdProcessor&) = delete;

    /**
     * Create texture from raw buffer data with enhanced validation
     * @param buffer Raw image data buffer (size-limited)
     * @param expectedFormat Expected image format for validation (optional)
     * @return TextureData structure containing the processed texture
     */
    TextureData CreateTextureFromBuffer(const std::vector<uint8_t>& buffer,
                                       const std::string& expectedFormat = "");

    /**
     * Load USD data from buffer with comprehensive error handling
     * @param buffer Raw USD data buffer (validated)
     * @param fileName Original filename for format detection (validated)
     * @param outMeshData Output vector for extracted mesh data (cleared first)
     * @param progressCallback Optional progress callback
     * @return True if loading was successful, false otherwise
     */
    bool LoadUSDBuffer(const std::vector<uint8_t>& buffer,
                      const std::string& fileName,
                      std::vector<MeshData>& outMeshData,
                      ProgressCallback progressCallback = nullptr);

    /**
     * Load USD data directly from disk with file validation
     * @param filePath Path to the USD file (validated)
     * @param outMeshData Output vector for extracted mesh data
     * @param progressCallback Optional progress callback
     * @return True if loading was successful, false otherwise
     */
    bool LoadUSDFromDisk(const std::string& filePath,
                        std::vector<MeshData>& outMeshData,
                        ProgressCallback progressCallback = nullptr);

    /**
     * Set maximum recursion depth for USD hierarchy processing
     * @param maxDepth Maximum depth (1-1000, default 100)
     */
    void setMaxRecursionDepth(int32_t maxDepth);

    /**
     * Get current maximum recursion depth
     * @return Current maximum recursion depth
     */
    int32_t getMaxRecursionDepth() const;

    /**
     * Set memory limit for processing operations
     * @param limitMB Memory limit in megabytes (1-4096, default 1024)
     */
    void setMemoryLimit(size_t limitMB);

    /**
     * Get current memory limit
     * @return Current memory limit in megabytes
     */
    size_t getMemoryLimit() const;

    /**
     * Enable or disable reference resolution
     * @param enable True to enable reference resolution, false to disable
     */
    void setReferenceResolutionEnabled(bool enable);

    /**
     * Check if reference resolution is enabled
     * @return True if enabled, false otherwise
     */
    bool isReferenceResolutionEnabled() const;

    /**
     * Get processing statistics - FIXED VERSION
     * @return Snapshot of current processing statistics (copyable)
     */
    ProcessingStats::Snapshot getProcessingStats() const;

    /**
     * Reset processing statistics
     */
    void resetProcessingStats();

    /**
     * Validate USD file format and structure
     * @param buffer USD data buffer
     * @param fileName Filename for context
     * @return True if valid USD format, false otherwise
     */
    bool validateUSDFormat(const std::vector<uint8_t>& buffer, const std::string& fileName);

    /**
     * Get supported USD file extensions
     * @return Vector of supported extensions
     */
    static std::vector<std::string> getSupportedExtensions();

    /**
     * Check if file extension is supported
     * @param extension File extension (with or without dot)
     * @return True if supported, false otherwise
     */
    static bool isSupportedExtension(const std::string& extension);

private:
    // Internal implementation with enhanced safety
    class UsdProcessorImpl;
    std::unique_ptr<UsdProcessorImpl> pImpl;

    // Thread safety
    mutable std::shared_mutex processingMutex;
    std::atomic<bool> processingInProgress{false};
    std::atomic<bool> shutdownRequested{false};

    // Configuration
    std::atomic<int32_t> maxRecursionDepth{safety::MAX_RECURSION_DEPTH};
    std::atomic<size_t> memoryLimitMB{1024};
    std::atomic<bool> referenceResolutionEnabled{true};

    // Statistics - using the fixed version
    ProcessingStats stats;

    /**
     * Process a USD primitive and its children recursively with safety checks
     * @param prim Pointer to the USD primitive (validated)
     * @param meshDataArray Output array for mesh data
     * @param parentTransform Parent transformation matrix (validated)
     * @param depth Current recursion depth (limited)
     * @return True if processing succeeded, false otherwise
     */
    bool ProcessPrim(void* prim,
                    std::vector<MeshData>& meshDataArray,
                    const glm::mat4& parentTransform,
                    int32_t depth);

    /**
     * Extract mesh data from USD mesh primitive with validation
     * @param mesh Pointer to the USD mesh (validated)
     * @param outMeshData Output mesh data structure
     * @param worldTransform World transformation matrix (validated)
     * @return True if extraction succeeded, false otherwise
     */
    bool ExtractMeshData(void* mesh,
                        MeshData& outMeshData,
                        const glm::mat4& worldTransform);

    /**
     * Get local transformation matrix with validation
     * @param prim Pointer to the USD primitive (validated)
     * @return Local transformation matrix (validated for finite values)
     */
    glm::mat4 GetLocalTransform(void* prim);

    /**
     * Extract reference paths from USD stage with validation
     * @param stage USD stage reference (validated)
     * @param outReferencePaths Output vector for reference paths
     */
    void ExtractReferencePaths(const tinyusdz::Stage& stage,
                              std::vector<std::string>& outReferencePaths);

    /**
     * Extract clips from raw USD content with pattern matching
     * @param buffer Raw USD content buffer
     * @return Vector of clip paths found
     */
    std::vector<std::string> ExtractClipsFromRawContent(const std::vector<uint8_t>& buffer);

    /**
     * Extract reference paths from primitive recursively
     * @param prim USD primitive reference
     * @param outReferencePaths Output vector for reference paths
     */
    void ExtractReferencePathsFromPrim(const tinyusdz::Prim& prim,
                                      std::vector<std::string>& outReferencePaths);

    /**
     * List primitive hierarchy for debugging
     * @param prim USD primitive reference
     * @param depth Current depth for indentation
     */
    void ListPrimHierarchy(const tinyusdz::Prim& prim, int depth);

    /**
     * Validate transformation matrix for finite values
     * @param transform Matrix to validate
     * @return True if all values are finite, false otherwise
     */
    bool validateTransform(const glm::mat4& transform) const;

    /**
     * Validate file path for security and existence
     * @param filePath Path to validate
     * @param checkExistence Whether to check if file exists
     * @return True if path is safe and valid, false otherwise
     */
    bool validateFilePath(const std::string& filePath, bool checkExistence = true) const;

    /**
     * Preprocess USD content to fix common issues
     * @param buffer Input USD content buffer
     * @return Preprocessed USD content buffer
     */
    std::vector<uint8_t> preprocessUsdContent(const std::vector<uint8_t>& buffer);

    /**
     * Check memory usage and enforce limits
     * @param additionalBytes Additional bytes to be allocated
     * @return True if within limits, false otherwise
     */
    bool checkMemoryLimit(size_t additionalBytes = 0) const;

    /**
     * Normalize and validate UV coordinates
     * @param uvs Input/output UV coordinates
     */
    void normalizeUVCoordinates(std::vector<glm::vec2>& uvs);

    /**
     * Validate and fix mesh indices
     * @param indices Input/output mesh indices
     * @param vertexCount Number of vertices for validation
     * @return True if indices are valid, false otherwise
     */
    bool validateMeshIndices(std::vector<uint32_t>& indices, size_t vertexCount);

    /**
     * Calculate and validate mesh normals
     * @param points Vertex positions
     * @param indices Triangle indices
     * @param outNormals Output normal vectors
     * @return True if normals calculated successfully, false otherwise
     */
    bool calculateMeshNormals(const std::vector<glm::vec3>& points,
                             const std::vector<uint32_t>& indices,
                             std::vector<glm::vec3>& outNormals);

    /**
     * Extract UV coordinates from mesh with validation
     * @param mesh GeomMesh pointer
     * @param meshData Output mesh data
     */
    void extractUVCoordinates(tinyusdz::GeomMesh* mesh, MeshData& meshData);

    /**
     * Check if mesh data has empty geometry
     * @param meshData Vector of mesh data to check
     * @return True if any mesh has empty geometry
     */
    bool hasEmptyGeometry(const std::vector<MeshData>& meshData) const;

    /**
     * Resolve USD references with progress tracking
     * @param stage USD stage
     * @param buffer Raw USD buffer
     * @param fileName Original filename
     * @param outMeshData Output mesh data
     * @param progressCallback Progress callback
     * @return True if successful
     */
    bool resolveReferences(const tinyusdz::Stage& stage,
                          const std::vector<uint8_t>& buffer,
                          const std::string& fileName,
                          std::vector<MeshData>& outMeshData,
                          ProgressCallback progressCallback);

    /**
     * Load referenced file and extract meshes
     * @param filePath Path to referenced file
     * @param outMeshData Output mesh data
     * @return True if successful
     */
    bool loadReferencedFile(const std::string& filePath, std::vector<MeshData>& outMeshData);
};

} // namespace anari_usd_middleware
