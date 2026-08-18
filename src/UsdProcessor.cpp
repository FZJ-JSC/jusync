#include "UsdProcessor.h"
#include "MiddlewareLogging.h"

// GPU acceleration includes (optional - gracefully degrade if not available)
#ifdef ENABLE_CUDA_ACCELERATION
#include "GpuContext.h"
#include "GpuKernels.h"
#include "GpuValidation.h"
#endif

// Standard library includes with enhanced safety
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <chrono>
#include <thread>
#include <cmath>
#include <limits>
#include <memory>
#include <atomic>
#include <future>
#include <vector>
#include <string_view>
// <execution> header: available in libstdc++ (GCC) but NOT in libc++ (clang)
// PAR_POLICY macro expands to "std::execution::par," on GCC, empty on clang
#if defined(__clang__)
// libc++ has no parallel execution policies - algorithms run sequentially
#define PAR_POLICY
#else
#include <execution>
#define PAR_POLICY std::execution::par,
#endif
#include <numeric>      // For parallel reduce

// zstd: broker USD payloads arrive zstd-frame-wrapped (magic-detected,
// self-describing). JUSYNC_HAS_ZSTD is defined by CMake when libzstd was found.
#if defined(JUSYNC_HAS_ZSTD)
#include <zstd.h>
#endif

// Include TinyUSDZ with error handling
#include "tinyusdz.hh"

// STB Image with safety wrappers
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#define STBI_MAX_DIMENSIONS 16384
#include <io-util.hh>

#include "stb_image.h"

namespace anari_usd_middleware {

// Implementation class with enhanced safety features
class UsdProcessor::UsdProcessorImpl {
public:
    UsdProcessorImpl() {
        MIDDLEWARE_LOG_INFO("UsdProcessorImpl created with enhanced safety features");
        processingStartTime = std::chrono::steady_clock::now();
        
        // Pre-compile regex patterns for performance
        try {
            regexPatterns.nonePattern = std::regex("0: None");
            regexPatterns.assetPattern = std::regex("asset:images/");
            regexPatterns.texCoordPattern = std::regex("texCoord2f");
        } catch (const std::regex_error& e) {
            MIDDLEWARE_LOG_ERROR("Failed to compile regex patterns: %s", e.what());
            // Fall back to runtime compilation if pre-compilation fails
        }
    }

    ~UsdProcessorImpl() {
        MIDDLEWARE_LOG_INFO("UsdProcessorImpl destroyed");
    }

    // Stream-based preprocessing for large files - OPTIMIZED VERSION
    std::vector<uint8_t> preprocessUsdContentStreaming(const std::vector<uint8_t>& buffer) {
        MIDDLEWARE_LOG_INFO("Streaming preprocessing of USD content, size: %zu", buffer.size());
        
        if (buffer.empty() || buffer.size() > safety::MAX_BUFFER_SIZE) {
            return buffer;
        }
        
        // For very large files, use chunked processing
        const size_t CHUNK_SIZE = 64 * 1024; // 64KB chunks
        std::vector<uint8_t> result;
        result.reserve(buffer.size() * 1.1); // Reserve 10% extra
        
        const char* data = reinterpret_cast<const char*>(buffer.data());
        size_t remaining = buffer.size();
        size_t position = 0;
        
        // Pre-compile regex patterns once
        if (regexPatterns.nonePattern.mark_count() == 0) {
            regexPatterns.nonePattern = std::regex("0: None");
            regexPatterns.assetPattern = std::regex("asset:images/");
            regexPatterns.texCoordPattern = std::regex("texCoord2f");
        }
        
        // Use faster string search/replace for common patterns
        const std::string nonePatternStr = "0: None";
        const std::string assetPatternStr = "asset:images/";
        const std::string texCoordPatternStr = "texCoord2f";
        const std::string noneReplacement = "0: []";
        const std::string assetReplacement = "@./images/";
        const std::string texCoordReplacement = "texCoord2f[]";
        
        while (remaining > 0) {
            size_t chunkSize = std::min(CHUNK_SIZE, remaining);
            std::string chunk(data + position, chunkSize);
            
            // Apply optimized replacements
            size_t pos = 0;
            while ((pos = chunk.find(nonePatternStr, pos)) != std::string::npos) {
                chunk.replace(pos, nonePatternStr.length(), noneReplacement);
                pos += noneReplacement.length();
            }
            
            pos = 0;
            while ((pos = chunk.find(assetPatternStr, pos)) != std::string::npos) {
                chunk.replace(pos, assetPatternStr.length(), assetReplacement);
                pos += assetReplacement.length();
            }
            
            pos = 0;
            while ((pos = chunk.find(texCoordPatternStr, pos)) != std::string::npos) {
                chunk.replace(pos, texCoordPatternStr.length(), texCoordReplacement);
                pos += texCoordReplacement.length();
            }
            
            // Append processed chunk to result
            result.insert(result.end(), chunk.begin(), chunk.end());
            
            position += chunkSize;
            remaining -= chunkSize;
        }
        
        MIDDLEWARE_LOG_INFO("Streaming preprocessing complete: %zu -> %zu bytes", 
                          buffer.size(), result.size());
        return result;
    }
    
    // Enhanced preprocessing with comprehensive validation
    std::vector<uint8_t> preprocessUsdContent(const std::vector<uint8_t>& buffer) {
        MIDDLEWARE_LOG_INFO("Preprocessing USD content of size %zu", buffer.size());

        // Validate input buffer
        if (buffer.empty()) {
            MIDDLEWARE_LOG_ERROR("Cannot preprocess empty buffer");
            return buffer;
        }

        if (buffer.size() > safety::MAX_BUFFER_SIZE) {
            MIDDLEWARE_LOG_ERROR("Buffer too large for preprocessing: %zu bytes (max: %zu)",
                                buffer.size(), safety::MAX_BUFFER_SIZE);
            return buffer;
        }

        // Check for large geometry early to avoid unnecessary processing.
        // Use a buffered string_view find over the head of the buffer instead of
        // a per-byte strncmp loop (up to 3 comparisons per byte for the first 1MB
        // of every large file).
        const size_t dataSize = buffer.size();
        bool hasLargeGeometry = false;
        {
            const size_t scanLimit = std::min(dataSize, static_cast<size_t>(1024 * 1024)); // first 1 MB
            if (scanLimit > 0) {
                const std::string_view head(reinterpret_cast<const char*>(buffer.data()), scanLimit);
                if (head.find("int[] faceVertexIndices") != std::string_view::npos ||
                    head.find("point3f[] points") != std::string_view::npos ||
                    head.find("float3[] points") != std::string_view::npos) {
                    hasLargeGeometry = true;
                }
            }
        }

        if (hasLargeGeometry) {
            MIDDLEWARE_LOG_INFO("Large geometry detected via streaming scan - applying CRITICAL '0: None' fix only");
            // MUST still fix "0: None" -> "0: []" even for geometry files, or TinyUSDZ will fail
            // Only apply the single critical replacement that TinyUSDZ requires
            std::string fileContent(reinterpret_cast<const char*>(buffer.data()), buffer.size());
            const std::string nonePatternStr = "0: None";
            const std::string noneReplacement = "0: []";
            size_t pos = 0;
            while ((pos = fileContent.find(nonePatternStr, pos)) != std::string::npos) {
                fileContent.replace(pos, nonePatternStr.length(), noneReplacement);
                pos += noneReplacement.length();
            }
            MIDDLEWARE_LOG_INFO("Geometry file preprocessing complete: %zu -> %zu bytes", buffer.size(), fileContent.size());
            return std::vector<uint8_t>(fileContent.begin(), fileContent.end());
        }

        // Use streaming processing for large files, regular processing for small files
        const size_t STREAMING_THRESHOLD = 10 * 1024 * 1024; // 10MB
        if (buffer.size() > STREAMING_THRESHOLD) {
            MIDDLEWARE_LOG_INFO("Large file detected (%zu bytes), using streaming preprocessing", buffer.size());
            return preprocessUsdContentStreaming(buffer);
        }

        try {
            // Convert buffer to string with size validation (only for small files)
            std::string fileContent;
            fileContent.assign(reinterpret_cast<const char*>(buffer.data()), buffer.size());

            // Apply optimized string replacements (faster than regex)
            try {
                const std::string nonePatternStr = "0: None";
                const std::string assetPatternStr = "asset:images/";
                const std::string texCoordPatternStr = "texCoord2f";
                const std::string noneReplacement = "0: []";
                const std::string assetReplacement = "@./images/";
                const std::string texCoordReplacement = "texCoord2f[]";

                size_t pos = 0;
                while ((pos = fileContent.find(nonePatternStr, pos)) != std::string::npos) {
                    fileContent.replace(pos, nonePatternStr.length(), noneReplacement);
                    pos += noneReplacement.length();
                }

                pos = 0;
                while ((pos = fileContent.find(assetPatternStr, pos)) != std::string::npos) {
                    fileContent.replace(pos, assetPatternStr.length(), assetReplacement);
                    pos += assetReplacement.length();
                }

                pos = 0;
                while ((pos = fileContent.find(texCoordPatternStr, pos)) != std::string::npos) {
                    fileContent.replace(pos, texCoordPatternStr.length(), texCoordReplacement);
                    pos += texCoordReplacement.length();
                }

                MIDDLEWARE_LOG_DEBUG("Applied optimized string replacements successfully");

            } catch (const std::exception& e) {
                MIDDLEWARE_LOG_ERROR("String replacement error during preprocessing: %s", e.what());
                return buffer;
            }

            // Conditional "line 34" patch: find line by scanning newlines, no split
            // Find the start of line 34 (0-indexed: the 34th newline-delimited line)
            {
                size_t lineStart = 0;
                size_t lineNum = 0;
                for (; lineNum < 33; ++lineNum) {
                    size_t newlinePos = fileContent.find('\n', lineStart);
                    if (newlinePos == std::string::npos) break;
                    lineStart = newlinePos + 1;
                }
                if (lineNum == 33 && lineStart < fileContent.size()) {
                    size_t lineEnd = fileContent.find('\n', lineStart);
                    if (lineEnd == std::string::npos) lineEnd = fileContent.size();
                    std::string_view line34(fileContent.data() + lineStart, lineEnd - lineStart);

                    if ((line34.find("texture") != std::string::npos ||
                          line34.find("albedoTex") != std::string::npos) &&
                         line34.find("uniform") == std::string::npos) {

                        std::string prefix = "uniform token info:id = \"UsdPreviewSurface\";";
                        if ((prefix.size() + line34.size()) < 1000) {
                            fileContent.replace(lineStart, 0, prefix);
                            MIDDLEWARE_LOG_DEBUG("Modified line 34 successfully");
                        } else {
                            MIDDLEWARE_LOG_WARNING("Modified line would be too long, skipping");
                        }
                    }
                }
            }

            MIDDLEWARE_LOG_INFO("Preprocessing complete: %zu -> %zu bytes",
                              buffer.size(), fileContent.size());

            return std::vector<uint8_t>(fileContent.begin(), fileContent.end());

        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in preprocessUsdContent: %s", e.what());
            return buffer; // Return original on any failure
        }
    }

    // Enhanced memory monitoring
    bool checkMemoryUsage(size_t additionalBytes = 0) const {
        // Simple memory usage estimation
        static std::atomic<size_t> currentMemoryUsage{0};
        size_t newUsage = currentMemoryUsage.load() + additionalBytes;

        if (newUsage > memoryLimitBytes.load()) {
            MIDDLEWARE_LOG_ERROR("Memory limit exceeded: %zu bytes (limit: %zu)",
                                newUsage, memoryLimitBytes.load());
            return false;
        }

        currentMemoryUsage.store(newUsage);
        return true;
    }

public:
    // GPU acceleration methods (public for use from UsdProcessor methods)
 #ifdef ENABLE_CUDA_ACCELERATION
    /**
     * Transform vertices with GPU acceleration and CPU fallback
     * @param points Input points from TinyUSDZ
     * @param transform World transformation matrix
     * @param outPoints Output transformed points
     */
    void transformVerticesWithGpu(
        const std::vector<tinyusdz::value::point3f>& points,
        const glm::mat4& transform,
        std::vector<glm::vec3>& outPoints)
    {
        if (points.empty()) {
            return;
        }

        // Pre-allocate output
        outPoints.resize(points.size());

        // Try GPU if threshold met and available
        bool gpuSuccess = false;
        std::vector<glm::vec3> gpuResult;
        
 #ifdef ENABLE_CUDA_ACCELERATION
        // Log GPU availability status for debugging
        MIDDLEWARE_LOG_INFO("🔥 GPU Acceleration Check: vertices=%zu, threshold=10000, isAvailable=%d",
                           points.size(), GpuContext::isAvailable() ? 1 : 0);
        
        if (GpuContext::isAvailable() && points.size() >= 10000) {
            MIDDLEWARE_LOG_INFO("🚀 Attempting GPU vertex transformation for %zu vertices", points.size());
            
            // Convert input to glm::vec3 for GPU kernel
            std::vector<glm::vec3> inputVertices(points.size());
            for (size_t i = 0; i < points.size(); ++i) {
                inputVertices[i] = glm::vec3(
                    static_cast<float>(points[i].x),
                    static_cast<float>(points[i].y),
                    static_cast<float>(points[i].z)
                );
            }

            // Try GPU transformation
            if (GpuKernels::transformVertices(inputVertices, transform, gpuResult)) {
                // Validate GPU results
                if (GpuValidation::validateTransformVertices(gpuResult, inputVertices, transform)) {
                    outPoints = std::move(gpuResult);
                    gpuSuccess = true;
                    MIDDLEWARE_LOG_INFO("GPU vertex transformation successful: %zu vertices", points.size());
                } else {
                    MIDDLEWARE_LOG_WARNING("GPU vertex validation failed, falling back to CPU");
                }
            } else {
                MIDDLEWARE_LOG_DEBUG("GPU vertex transformation not available, using CPU");
            }
        }
 #endif

        // CPU fallback if GPU not used or failed
        if (!gpuSuccess) {
            const float* m = &transform[0][0];
            
            auto transformVertex = [m](const tinyusdz::value::point3f& pt) -> glm::vec3 {
                const float x = static_cast<float>(pt.x);
                const float y = static_cast<float>(pt.y);
                const float z = static_cast<float>(pt.z);
                
                // Fast NaN/Inf check using integer representation
                const int32_t* ix = reinterpret_cast<const int32_t*>(&x);
                const int32_t* iy = reinterpret_cast<const int32_t*>(&y);
                const int32_t* iz = reinterpret_cast<const int32_t*>(&z);
                
                // Check if any component is NaN or Inf (exponent bits all 1s)
                if ((*ix & 0x7F800000) == 0x7F800000 ||
                    (*iy & 0x7F800000) == 0x7F800000 ||
                    (*iz & 0x7F800000) == 0x7F800000) {
                    return glm::vec3(0.0f, 0.0f, 0.0f);
                }

                // Manual matrix multiplication - optimized
                const float tx = m[0] * x + m[4] * y + m[8] * z + m[12];
                const float ty = m[1] * x + m[5] * y + m[9] * z + m[13];
                const float tz = m[2] * x + m[6] * y + m[10] * z + m[14];
                
                // Fast validation of transformed vertex
                const int32_t* itx = reinterpret_cast<const int32_t*>(&tx);
                const int32_t* ity = reinterpret_cast<const int32_t*>(&ty);
                const int32_t* itz = reinterpret_cast<const int32_t*>(&tz);
                
                if ((*itx & 0x7F800000) == 0x7F800000 ||
                    (*ity & 0x7F800000) == 0x7F800000 ||
                    (*itz & 0x7F800000) == 0x7F800000) {
                    return glm::vec3(x, y, z);
                }
                
                return glm::vec3(tx, ty, tz);
            };
            
            // PARALLEL vertex processing using std::transform
            std::transform(PAR_POLICY
                          points.begin(), points.end(),
                          outPoints.begin(),
                          transformVertex);
            
            MIDDLEWARE_LOG_DEBUG("CPU vertex transformation complete: %zu vertices", points.size());
        }
    }

    /**
     * Transform normals with GPU acceleration and CPU fallback
     * @param normals Input normals from TinyUSDZ
     * @param normalMatrix 3x3 normal transformation matrix
     * @param outNormals Output transformed normals
     */
    void transformNormalsWithGpu(
        const std::vector<tinyusdz::value::normal3f>& normals,
        const glm::mat3& normalMatrix,
        std::vector<glm::vec3>& outNormals)
    {
        if (normals.empty()) {
            return;
        }

        outNormals.resize(normals.size());
        bool gpuSuccess = false;
        std::vector<glm::vec3> gpuResult;

 #ifdef ENABLE_CUDA_ACCELERATION
        if (GpuContext::isAvailable() && normals.size() >= 10000) {
            MIDDLEWARE_LOG_DEBUG("Attempting GPU normal transformation for %zu normals", normals.size());
            
            // Convert input
            std::vector<glm::vec3> inputNormals(normals.size());
            for (size_t i = 0; i < normals.size(); ++i) {
                inputNormals[i] = glm::vec3(
                    static_cast<float>(normals[i].x),
                    static_cast<float>(normals[i].y),
                    static_cast<float>(normals[i].z)
                );
            }

            // Try GPU transformation
            if (GpuKernels::transformNormals(inputNormals, normalMatrix, gpuResult)) {
                if (GpuValidation::validateTransformNormals(gpuResult, inputNormals, normalMatrix)) {
                    outNormals = std::move(gpuResult);
                    gpuSuccess = true;
                    MIDDLEWARE_LOG_INFO("GPU normal transformation successful: %zu normals", normals.size());
                } else {
                    MIDDLEWARE_LOG_WARNING("GPU normal validation failed, falling back to CPU");
                }
            }
        }
 #endif

        // CPU fallback
        if (!gpuSuccess) {
            auto transformNormal = [normalMatrix](const tinyusdz::value::normal3f& nrm) -> glm::vec3 {
                // Fast NaN/Inf check
                const int32_t* ix = reinterpret_cast<const int32_t*>(&nrm.x);
                const int32_t* iy = reinterpret_cast<const int32_t*>(&nrm.y);
                const int32_t* iz = reinterpret_cast<const int32_t*>(&nrm.z);
                
                if ((*ix & 0x7F800000) == 0x7F800000 ||
                    (*iy & 0x7F800000) == 0x7F800000 ||
                    (*iz & 0x7F800000) == 0x7F800000) {
                    return glm::vec3(0.0f, 1.0f, 0.0f);  // Default up vector
                }

                glm::vec3 normalVec(static_cast<float>(nrm.x),
                                  static_cast<float>(nrm.y),
                                  static_cast<float>(nrm.z));
                glm::vec3 transformedNormal = normalMatrix * normalVec;

                // Fast length calculation and normalization
                float lengthSquared = glm::dot(transformedNormal, transformedNormal);
                if (lengthSquared > safety::EPSILON * safety::EPSILON) {
                    float invLength = 1.0f / std::sqrt(lengthSquared);
                    return transformedNormal * invLength;
                }
                
                return glm::vec3(0.0f, 1.0f, 0.0f);  // Default up vector
            };

            // PARALLEL normal processing
            std::transform(PAR_POLICY
                          normals.begin(), normals.end(),
                          outNormals.begin(),
                          transformNormal);
            
            MIDDLEWARE_LOG_DEBUG("CPU normal transformation complete: %zu normals", normals.size());
        }
    }

    /**
     * Process UVs with GPU acceleration and CPU fallback
     * @param uvs Input UV coordinates from TinyUSDZ
     * @param outUVs Output processed UVs
     */
    void processUVsWithGpu(
        const std::vector<tinyusdz::value::texcoord2f>& uvs,
        std::vector<glm::vec2>& outUVs)
    {
        if (uvs.empty()) {
            return;
        }

        outUVs.resize(uvs.size());
        bool gpuSuccess = false;
        std::vector<glm::vec2> gpuResult;

 #ifdef ENABLE_CUDA_ACCELERATION
        if (GpuContext::isAvailable() && uvs.size() >= 10000) {
            MIDDLEWARE_LOG_DEBUG("Attempting GPU UV processing for %zu UVs", uvs.size());
            
            // Convert input
            std::vector<glm::vec2> inputUVs(uvs.size());
            for (size_t i = 0; i < uvs.size(); ++i) {
                inputUVs[i] = glm::vec2(uvs[i].s, uvs[i].t);
            }

            // Try GPU processing
            if (GpuKernels::processUVs(inputUVs, gpuResult)) {
                if (GpuValidation::validateProcessUVs(gpuResult, inputUVs)) {
                    outUVs = std::move(gpuResult);
                    gpuSuccess = true;
                    MIDDLEWARE_LOG_INFO("GPU UV processing successful: %zu UVs", uvs.size());
                } else {
                    MIDDLEWARE_LOG_WARNING("GPU UV validation failed, falling back to CPU");
                }
            }
        }
 #endif

        // CPU fallback
        if (!gpuSuccess) {
            // PARALLEL UV extraction and processing
            std::transform(PAR_POLICY
                          uvs.begin(), uvs.end(),
                          outUVs.begin(),
                          [](const tinyusdz::value::texcoord2f& uv) {
                              return glm::vec2(uv.s, uv.t);
                          });

            // Normalize and validate UVs (parallel)
            std::for_each(PAR_POLICY outUVs.begin(), outUVs.end(), [](glm::vec2& uv) {
                // Fast NaN/Inf check using integer representation
                const int32_t* ix = reinterpret_cast<const int32_t*>(&uv.x);
                const int32_t* iy = reinterpret_cast<const int32_t*>(&uv.y);
                
                if ((*ix & 0x7F800000) == 0x7F800000 || (*iy & 0x7F800000) == 0x7F800000) {
                    uv = glm::vec2(0.0f, 0.0f);
                } else {
                    // Clamp UV coordinates to reasonable range
                    uv.x = std::clamp(uv.x, -10.0f, 10.0f);
                    uv.y = std::clamp(uv.y, -10.0f, 10.0f);
                }
            });
            
            MIDDLEWARE_LOG_DEBUG("CPU UV processing complete: %zu UVs", uvs.size());
        }
    }
 #endif

private:
    // Pre-compiled regex patterns for performance
    struct RegexPatterns {
        std::regex nonePattern;
        std::regex assetPattern;
        std::regex texCoordPattern;
        
        RegexPatterns() = default;
    };
    
    RegexPatterns regexPatterns;
    std::chrono::steady_clock::time_point processingStartTime;

public:
    std::atomic<size_t> memoryLimitBytes{std::numeric_limits<int64_t>::max()}; // unlimited, synced by setMemoryLimit
    std::vector<std::string> extractClipsFromString(std::string_view content);
};

// Enhanced MeshData validation methods
std::pair<glm::vec3, glm::vec3> UsdProcessor::MeshData::getBounds() const {
    if (points.empty()) {
        return {glm::vec3(0.0f), glm::vec3(0.0f)};
    }

    glm::vec3 minBounds = points[0];
    glm::vec3 maxBounds = points[0];

    for (const auto& point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
            MIDDLEWARE_LOG_WARNING("Non-finite vertex detected in bounds calculation");
            continue;
        }
        minBounds = glm::min(minBounds, point);
        maxBounds = glm::max(maxBounds, point);
    }

    return {minBounds, maxBounds};
}

bool UsdProcessor::MeshData::validateGeometry() const {
    if (points.empty()) return false;
    if (!indices.empty()) {
        if (indices.size() % 3 != 0) return false;
        for (uint32_t index : indices) {
            if (index >= points.size()) return false;
        }
    }
    if (!normals.empty()) {
        if (normals.size() != points.size()) return false;
    }
    if (!uvs.empty()) {
        if (uvs.size() != points.size()) return false;
    }
    for (const auto& point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) return false;
    }
    for (const auto& normal : normals) {
        if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z)) return false;
    }
    for (const auto& uv : uvs) {
        if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) return false;
    }
    return true;
}

// Constructor and destructor with enhanced safety
UsdProcessor::UsdProcessor() : pImpl(std::make_unique<UsdProcessorImpl>()) {
    MIDDLEWARE_LOG_INFO("UsdProcessor created with enhanced safety features");
    stats.reset();
    
    // Initialize GPU context on startup
#ifdef ENABLE_CUDA_ACCELERATION
    MIDDLEWARE_LOG_INFO("🔥 Initializing GPU acceleration support...");
    if (GpuContext::getInstance().initialize()) {
        MIDDLEWARE_LOG_INFO("✅ GPU acceleration initialized successfully - Device: %s",
                           GpuContext::getDeviceInfo().deviceName.c_str());
    } else {
        MIDDLEWARE_LOG_WARNING("⚠️ GPU acceleration not available - using CPU fallback");
    }
#else
    MIDDLEWARE_LOG_INFO("ℹ️ GPU acceleration not compiled in - using CPU only");
#endif
}

UsdProcessor::~UsdProcessor() {
    MIDDLEWARE_LOG_INFO("UsdProcessor destroyed");
    shutdownRequested.store(true);

    // Wait for any ongoing processing to complete
    std::unique_lock<std::shared_mutex> lock(processingMutex);
    MIDDLEWARE_LOG_INFO("UsdProcessor shutdown complete");
}

// Enhanced texture creation with comprehensive validation
UsdProcessor::TextureData UsdProcessor::CreateTextureFromBuffer(const std::vector<uint8_t>& buffer,
                                                               const std::string& expectedFormat) {
    std::shared_lock<std::shared_mutex> lock(processingMutex);

    MIDDLEWARE_LOG_INFO("Creating texture from buffer of size %zu", buffer.size());

    TextureData textureData;

    // MEMORY SAFETY: Validate input buffer
    if (buffer.empty()) {
        MIDDLEWARE_LOG_ERROR("CreateTextureFromBuffer: Empty buffer");
        stats.processingErrors.fetch_add(1);
        return textureData;
    }

    if (buffer.size() > safety::MAX_BUFFER_SIZE) {
        MIDDLEWARE_LOG_ERROR("Buffer too large for texture creation: %zu bytes (max: %zu)",
                            buffer.size(), safety::MAX_BUFFER_SIZE);
        stats.processingErrors.fetch_add(1);
        return textureData;
    }

    // MEMORY SAFETY: Check buffer data pointer validity
    if (!buffer.data()) {
        MIDDLEWARE_LOG_ERROR("Buffer has null data pointer despite non-zero size: %zu", buffer.size());
        stats.processingErrors.fetch_add(1);
        return textureData;
    }

    try {
        int width, height, channels;

        // Use STB image with enhanced error handling
        stbi_set_flip_vertically_on_load(0); // Don't flip for consistency

        // MEMORY SAFETY: Validate buffer size for STB processing
        if (buffer.size() > INT_MAX) {
            MIDDLEWARE_LOG_ERROR("Buffer too large for STB image processing: %zu bytes (max: %d)",
                                buffer.size(), INT_MAX);
            stats.processingErrors.fetch_add(1);
            return textureData;
        }

        // MEMORY SAFETY: Double-check buffer pointer before passing to STB
        const unsigned char* bufferPtr = buffer.data();
        if (!bufferPtr) {
            MIDDLEWARE_LOG_ERROR("Buffer data pointer is null before STB processing");
            stats.processingErrors.fetch_add(1);
            return textureData;
        }

        unsigned char* imageData = stbi_load_from_memory(
            bufferPtr,
            static_cast<int>(buffer.size()),
            &width, &height, &channels,
            4 // Force RGBA
        );

        if (!imageData) {
            const char* reason = stbi_failure_reason();
            MIDDLEWARE_LOG_ERROR("Failed to decode image data: %s", reason ? reason : "Unknown error");
            stats.processingErrors.fetch_add(1);
            return textureData;
        }

        // MEMORY SAFETY: Validate dimensions returned by STB
        if (width <= 0 || height <= 0) {
            MIDDLEWARE_LOG_ERROR("Invalid image dimensions from STB: %dx%d", width, height);
            stbi_image_free(imageData);
            stats.processingErrors.fetch_add(1);
            return textureData;
        }

        // MEMORY SAFETY: Check for potential overflow in size calculation
        if (width > 0 && height > 0) {
            // Check if multiplication would overflow
            size_t maxAllowedWidth = safety::MAX_BUFFER_SIZE / static_cast<size_t>(height) / 4;
            if (static_cast<size_t>(width) > maxAllowedWidth) {
                MIDDLEWARE_LOG_ERROR("Image dimensions would cause overflow: %dx%d", width, height);
                stbi_image_free(imageData);
                stats.processingErrors.fetch_add(1);
                return textureData;
            }
        }

        // Calculate expected data size with overflow protection
        size_t expectedDataSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        if (expectedDataSize > safety::MAX_BUFFER_SIZE) {
            MIDDLEWARE_LOG_ERROR("Decoded image too large: %zu bytes (max: %zu)",
                                expectedDataSize, safety::MAX_BUFFER_SIZE);
            stbi_image_free(imageData);
            stats.processingErrors.fetch_add(1);
            return textureData;
        }

        // MEMORY SAFETY: Validate imageData pointer after STB processing
        if (!imageData) {
            MIDDLEWARE_LOG_ERROR("STB returned null image data pointer");
            stats.processingErrors.fetch_add(1);
            return textureData;
        }

        // Handle gradient line extraction (height == 2)
        if (height == 2) {
            MIDDLEWARE_LOG_INFO("Detected gradient image, extracting top row");

            size_t rowSize = static_cast<size_t>(width) * 4;

            // MEMORY SAFETY: Validate row size before allocation
            if (rowSize == 0 || rowSize > safety::MAX_BUFFER_SIZE) {
                MIDDLEWARE_LOG_ERROR("Invalid gradient row size: %zu", rowSize);
                stbi_image_free(imageData);
                stats.processingErrors.fetch_add(1);
                return textureData;
            }

            try {
                textureData.data.clear();
                textureData.data.reserve(rowSize);
                textureData.data.resize(rowSize);

                // MEMORY SAFETY: Safe copy with bounds checking
                std::memcpy(textureData.data.data(), imageData, rowSize);

                // Verify the copy worked correctly
                if (textureData.data.size() != rowSize) {
                    MIDDLEWARE_LOG_ERROR("Gradient data copy size mismatch: expected %zu, got %zu",
                                        rowSize, textureData.data.size());
                    stbi_image_free(imageData);
                    textureData.clear();
                    stats.processingErrors.fetch_add(1);
                    return textureData;
                }

            } catch (const std::bad_alloc& e) {
                MIDDLEWARE_LOG_ERROR("Memory allocation failed for gradient data: %s", e.what());
                stbi_image_free(imageData);
                textureData.clear();
                stats.processingErrors.fetch_add(1);
                return textureData;
            }

            textureData.width = width;
            textureData.height = 1;
            textureData.channels = 4;

            stbi_image_free(imageData);

            MIDDLEWARE_LOG_INFO("Gradient line extracted: %dx%d, %d channels",
                              textureData.width, textureData.height, textureData.channels);

            stats.texturesProcessed.fetch_add(1);
            return textureData;
        }

        // Standard image processing
        try {
            textureData.data.clear();
            textureData.data.reserve(expectedDataSize);
            textureData.data.resize(expectedDataSize);

            // MEMORY SAFETY: Safe copy with validation
            std::memcpy(textureData.data.data(), imageData, expectedDataSize);

            // Verify the copy worked correctly
            if (textureData.data.size() != expectedDataSize) {
                MIDDLEWARE_LOG_ERROR("Image data copy size mismatch: expected %zu, got %zu",
                                    expectedDataSize, textureData.data.size());
                stbi_image_free(imageData);
                textureData.clear();
                stats.processingErrors.fetch_add(1);
                return textureData;
            }

        } catch (const std::bad_alloc& e) {
            MIDDLEWARE_LOG_ERROR("Memory allocation failed for image data: %s", e.what());
            stbi_image_free(imageData);
            textureData.clear();
            stats.processingErrors.fetch_add(1);
            return textureData;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception during image data copy: %s", e.what());
            stbi_image_free(imageData);
            textureData.clear();
            stats.processingErrors.fetch_add(1);
            return textureData;
        }

        textureData.width = width;
        textureData.height = height;
        textureData.channels = 4;

        stbi_image_free(imageData);

        // Validate final texture data
        if (!textureData.isValid()) {
            MIDDLEWARE_LOG_ERROR("Created texture data failed validation");
            textureData.clear();
            stats.processingErrors.fetch_add(1);
            return textureData;
        }

        MIDDLEWARE_LOG_INFO("Texture created successfully: %dx%d, %d channels",
                          textureData.width, textureData.height, textureData.channels);

        stats.texturesProcessed.fetch_add(1);
        return textureData;

    } catch (const std::bad_alloc& e) {
        MIDDLEWARE_LOG_ERROR("Memory allocation error in CreateTextureFromBuffer: %s", e.what());
        stats.processingErrors.fetch_add(1);
        return textureData;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in CreateTextureFromBuffer: %s", e.what());
        stats.processingErrors.fetch_add(1);
        return textureData;

    } catch (...) {
        MIDDLEWARE_LOG_ERROR("Unknown exception in CreateTextureFromBuffer");
        stats.processingErrors.fetch_add(1);
        return textureData;
    }
}

// Thin wrapper: delegates to pointer-based overload
bool UsdProcessor::LoadUSDBuffer(const std::vector<uint8_t>& buffer,
                                 const std::string& fileName,
                                 std::vector<MeshData>& outMeshData,
                                 std::vector<PointCloudData>* outPointCloudData,
                                 ProgressCallback progressCallback) {
    return LoadUSDBufferFromRaw(buffer.data(), buffer.size(), fileName, outMeshData, outPointCloudData, progressCallback);
}

// Pointer-based overload: TRUE zero-copy when no preprocessing is required.
// Callers pass raw buffer + size directly (e.g., from TArray<uint8> or external memory).
// A single working copy is only made for the "0: None"/"asset:images/"/"texCoord2f" fix.
bool UsdProcessor::LoadUSDBufferFromRaw(const uint8_t* buffer, size_t buffer_size,
                                        const std::string& fileName,
                                        std::vector<MeshData>& outMeshData,
                                        std::vector<PointCloudData>* outPointCloudData,
                                        ProgressCallback progressCallback) {
    std::unique_lock<std::shared_mutex> lock(processingMutex);
    if (shutdownRequested.load()) {
        MIDDLEWARE_LOG_WARNING("USD loading aborted: shutdown requested");
        return false;
    }

    MIDDLEWARE_LOG_INFO("Loading USD from buffer, size: %zu, filename: %s", buffer_size, fileName.c_str());

    // Clear output data first
    outMeshData.clear();
    if (outPointCloudData) {
        outPointCloudData->clear();
    }

    // Validate inputs
    if (buffer_size == 0) {
        MIDDLEWARE_LOG_ERROR("Cannot load USD from empty buffer");
        stats.processingErrors.fetch_add(1);
        return false;
    }

    if (buffer_size > safety::MAX_BUFFER_SIZE) {
        MIDDLEWARE_LOG_ERROR("USD buffer too large: %zu bytes (max: %zu)",
                            buffer_size, safety::MAX_BUFFER_SIZE);
        stats.processingErrors.fetch_add(1);
        return false;
    }

    if (fileName.empty()) {
        MIDDLEWARE_LOG_ERROR("Filename cannot be empty");
        stats.processingErrors.fetch_add(1);
        return false;
    }

    try {
        if (progressCallback) {
            progressCallback(0.1f, "Preprocessing USD content");
        }

        // ---- Self-describing wire format: zstd framing -------------------------
        // The broker zstd-compresses USD payloads at store time. The frame is
        // detected by the zstd magic (0xFD2FB528) in the first 4 bytes, so no
        // wire-protocol change is involved: uncompressed payloads pass through
        // untouched, and compressed ones are transparently decompressed here
        // (the single C API choke point every parse path funnels through).
        const uint8_t* wirePtr = buffer;
        size_t wireSize = buffer_size;
        std::vector<uint8_t> decompressed;  // allocated only for zstd payloads
#if defined(JUSYNC_HAS_ZSTD)
        // Explicit byte compare (not an endianness-dependent uint32_t compare):
        // zstd frames begin with 28 B5 2F FD on the wire.
        if (wireSize >= 4 &&
            wirePtr[0] == 0x28 && wirePtr[1] == 0xB5 &&
            wirePtr[2] == 0x2F && wirePtr[3] == 0xFD) {
            unsigned long long contentSize = ZSTD_getFrameContentSize(wirePtr, wireSize);
            if (contentSize == ZSTD_CONTENTSIZE_ERROR) {
                MIDDLEWARE_LOG_ERROR("Invalid zstd frame in USD payload for '%s'", fileName.c_str());
                stats.processingErrors.fetch_add(1);
                return false;
            }

            if (contentSize == 0) {
                wirePtr = decompressed.data();
                wireSize = 0;
            } else {
                size_t dstCapacity;
                if (contentSize != ZSTD_CONTENTSIZE_UNKNOWN) {
                    if (contentSize > static_cast<unsigned long long>(safety::MAX_BUFFER_SIZE)) {
                        MIDDLEWARE_LOG_ERROR("zstd USD payload too large after decompression: %llu bytes for '%s'",
                                             contentSize, fileName.c_str());
                        stats.processingErrors.fetch_add(1);
                        return false;
                    }
                    dstCapacity = static_cast<size_t>(contentSize);
                } else {
                    // Unknown content size: start small and grow if the static API
                    // reports dstSize_tooSmall, rather than guessing a fixed ratio.
                    const size_t initialGuess = std::max<size_t>(
                        1u << 20,
                        wireSize < (size_t(1) << 60) ? wireSize * 4 : size_t(1) << 60);
                    dstCapacity = std::min(initialGuess, static_cast<size_t>(safety::MAX_BUFFER_SIZE));
                }

                decompressed.resize(dstCapacity);
                bool success = false;
                for (;;) {
                    size_t actual = ZSTD_decompress(decompressed.data(), decompressed.size(), wirePtr, wireSize);
                    if (!ZSTD_isError(actual)) {
                        if (actual > static_cast<size_t>(safety::MAX_BUFFER_SIZE)) {
                            MIDDLEWARE_LOG_ERROR("zstd USD payload too large after decompression: %zu bytes for '%s'",
                                                 actual, fileName.c_str());
                            stats.processingErrors.fetch_add(1);
                            return false;
                        }
                        decompressed.resize(actual);
                        success = true;
                        break;
                    }
                    if (ZSTD_getErrorCode(actual) != ZSTD_error_dstSize_tooSmall) {
                        MIDDLEWARE_LOG_ERROR("zstd decompression FAILED for '%s': %s",
                                             fileName.c_str(), ZSTD_getErrorName(actual));
                        stats.processingErrors.fetch_add(1);
                        return false;
                    }
                    if (decompressed.size() >= static_cast<size_t>(safety::MAX_BUFFER_SIZE)) {
                        MIDDLEWARE_LOG_ERROR("zstd USD payload too large after decompression for '%s'",
                                             fileName.c_str());
                        stats.processingErrors.fetch_add(1);
                        return false;
                    }
                    size_t nextCapacity = std::min(
                        decompressed.size() * 2,
                        static_cast<size_t>(safety::MAX_BUFFER_SIZE));
                    decompressed.resize(nextCapacity);
                }

                wirePtr = decompressed.data();
                wireSize = decompressed.size();
                MIDDLEWARE_LOG_INFO("Decompressed zstd USD payload for '%s': %zu -> %zu bytes",
                                    fileName.c_str(), buffer_size, wireSize);
            }
        }
#endif // JUSYNC_HAS_ZSTD

        // Binary USDC detection (checked AFTER zstd, since the broker compresses
        // binary USD). The ASCII text transforms below must NOT run on binary
        // content: a byte replacement inside a USDC string table would corrupt
        // its length-prefixed records.
        const bool isBinaryUsdc = wireSize >= 8 && std::memcmp(wirePtr, "PXR-USDC", 8) == 0;

        // TRUE ZERO-COPY fast path: scan the raw buffer for any byte that would
        // require a transform. Binary (.usdc) files never need one. For ASCII
        // .usda, note that both empty and real timeSampled arrays ("0: None",
        // "0: [(x,y,z)...]") are handled natively by TinyUSDZ (verified
        // empirically), so they do NOT force a copy.
        //
        // Only the genuinely length-changing text substitutions below
        // (which cannot be done in the read-only caller buffer) remain.
        // When none are present we hand the caller's pointer straight to
        // TinyUSDZ with ZERO copies.
        const std::string assetPatternStr = "asset:images/";
        const std::string assetReplacement = "@./images/";
        const std::string texCoordPatternStr = "texCoord2f";
        const std::string texCoordReplacement = "texCoord2f[]";

        // Zero-copy scan for any transform-forcing token. A buffered string_view
        // find uses block-wise comparison instead of the old per-byte loop (up to
        // 2 memcmps per byte across the entire buffer). Skipped entirely for
        // binary USDC (see isBinaryUsdc).
        bool needsPreprocess = false;
        if (!isBinaryUsdc && wireSize > 0) {
            const std::string_view raw(reinterpret_cast<const char*>(wirePtr), wireSize);
            if (raw.find(assetPatternStr) != std::string_view::npos ||
                raw.find(texCoordPatternStr) != std::string_view::npos) {
                needsPreprocess = true;
            }
        }

        // Holds the single transformed copy when preprocessing is required.
        // Kept at function scope so it stays alive for the whole parse + reference
        // resolution pass; when not needed it is empty and parsePtr points at the
        // caller's original buffer (the true zero-copy path).
        std::string content;
        std::string fixedContent; // holds fixed content for clip extraction

        const uint8_t* parsePtr = wirePtr;
        size_t parseSize = wireSize;

        if (needsPreprocess) {
            // Copy exactly once, then transform in place. This replaces the old
            // two-copy chain (std::string content -> std::vector processedBuffer).
            content.assign(reinterpret_cast<const char*>(wirePtr), wireSize);

            size_t pos = 0;
            while ((pos = content.find(assetPatternStr, pos)) != std::string::npos) {
                content.replace(pos, assetPatternStr.length(), assetReplacement);
                pos += assetReplacement.length();
            }
            pos = 0;
            while ((pos = content.find(texCoordPatternStr, pos)) != std::string::npos) {
                content.replace(pos, texCoordPatternStr.length(), texCoordReplacement);
                pos += texCoordReplacement.length();
            }

            // Check if this contains large geometry arrays
            if (content.find("int[] faceVertexIndices") != std::string::npos ||
                content.find("point3f[] points") != std::string::npos ||
                content.find("float3[] points") != std::string::npos) {
                MIDDLEWARE_LOG_INFO("Large geometry detected - using working buffer for Unreal RealtimeMesh");
                fixedContent = content; // keep for clip extraction below
            }

            parsePtr = reinterpret_cast<const uint8_t*>(content.data());
            parseSize = content.size();
            MIDDLEWARE_LOG_INFO("USD preprocessed (single copy): %zu -> %zu bytes", buffer_size, parseSize);
        } else {
            MIDDLEWARE_LOG_INFO("USD content requires no rewrite - parsing caller buffer directly (true zero-copy)");
        }

        // LIMITED DEBUG: Only show first 200 characters for debugging
        if (parseSize > 200) {
            std::string preview(reinterpret_cast<const char*>(parsePtr), 200);
            preview += "... [truncated for debug]";
            MIDDLEWARE_LOG_DEBUG("USD content preview: %s", preview.c_str());
        } else {
            std::string fullContent(reinterpret_cast<const char*>(parsePtr), parseSize);
            MIDDLEWARE_LOG_DEBUG("USD content: %s", fullContent.c_str());
        }

        if (progressCallback) {
            progressCallback(0.2f, "Detecting file format");
        }

        // Detect file format
        bool isUSDZ = (fileName.find(".usdz") != std::string::npos);
        if (isUSDZ) {
            MIDDLEWARE_LOG_INFO("Detected USDZ format file");
        }

        if (progressCallback) {
            progressCallback(0.3f, "Loading USD stage");
        }

        // Load USD stage with enhanced options
        tinyusdz::Stage stage;
        std::string warnings, errors;
        tinyusdz::USDLoadOptions options;
        options.load_payloads = true;
        options.load_references = true;
        options.load_sublayers = true;
        options.max_memory_limit_in_mb = static_cast<int>(memoryLimitMB.load());

        bool loadResult = tinyusdz::LoadUSDFromMemory(
            parsePtr,
            parseSize,
            fileName.c_str(),
            &stage,
            &warnings,
            &errors,
            options
        );

        if (!loadResult) {
            MIDDLEWARE_LOG_ERROR("TinyUSDZ_load_FAILED: file='%s' size=%zu errors='%s' warnings='%s'",
                fileName.c_str(), parseSize, errors.c_str(), warnings.c_str());
            // Log first 300 chars of buffer to diagnose if it's actually USD content
            size_t previewLen = std::min(parseSize, static_cast<size_t>(300));
            std::string preview(reinterpret_cast<const char*>(parsePtr), previewLen);
            MIDDLEWARE_LOG_ERROR("TinyUSDZ_buffer_preview: '...%s'...", preview.c_str());
            stats.processingErrors.fetch_add(1);
            return false;
        }

        if (!warnings.empty()) {
            MIDDLEWARE_LOG_WARNING("TinyUSDZ load warnings for '%s': %s", fileName.c_str(), warnings.c_str());
        }

        MIDDLEWARE_LOG_INFO("USD stage loaded successfully. Root prims: %zu", stage.root_prims().size());

        if (progressCallback) {
            progressCallback(0.5f, "Processing primitives");
        }

        // Process main stage
        glm::mat4 identity(1.0f);
        size_t initialMeshCount = outMeshData.size();

        for (const auto& rootPrim : stage.root_prims()) {
            if (shutdownRequested.load()) {
                MIDDLEWARE_LOG_WARNING("USD processing aborted: shutdown requested");
                return false;
            }

            if (!ProcessPrim(const_cast<tinyusdz::Prim*>(&rootPrim), outMeshData, outPointCloudData, identity, 0)) {
                MIDDLEWARE_LOG_WARNING("Failed to process root prim: %s", rootPrim.element_name().c_str());
            }
        }

        size_t meshesFromMain = outMeshData.size() - initialMeshCount;
        MIDDLEWARE_LOG_INFO("Extracted %zu meshes from main stage", meshesFromMain);

        if (outPointCloudData) {
            MIDDLEWARE_LOG_INFO("Extracted %zu point clouds from main stage", outPointCloudData->size());
        }

        if (progressCallback) {
            progressCallback(0.7f, "Resolving references");
        }

        // Enhanced reference resolution
        if (referenceResolutionEnabled.load() && (outMeshData.empty() || hasEmptyGeometry(outMeshData))) {
            MIDDLEWARE_LOG_INFO("Attempting reference resolution for missing geometry");

            // resolveReferences needs a std::vector<uint8_t>; build a view only
            // on this rare reference-resolution path (which already re-reads from disk).
            std::vector<uint8_t> refBuffer(parsePtr, parsePtr + parseSize);
            if (!resolveReferences(stage, refBuffer, fileName, outMeshData, progressCallback, &fixedContent)) {
                MIDDLEWARE_LOG_WARNING("Reference resolution completed with some failures");
            }
        }

        if (progressCallback) {
            progressCallback(0.9f, "Validating mesh data");
        }

        // Validate all mesh data
        auto it = outMeshData.begin();
        while (it != outMeshData.end()) {
            if (!it->isValid()) {
                MIDDLEWARE_LOG_WARNING("Removing invalid mesh: %s", it->elementName.c_str());
                it = outMeshData.erase(it);
            } else {
                ++it;
            }
        }

        if (progressCallback) {
            progressCallback(1.0f, "Processing complete");
        }

        // Update statistics
        stats.filesProcessed.fetch_add(1);
        stats.meshesExtracted.fetch_add(outMeshData.size());
        stats.totalBytesProcessed.fetch_add(buffer_size);

        // LIMITED DEBUG: Only show mesh statistics for RealtimeMesh, not full data
        MIDDLEWARE_LOG_INFO("USD processing complete: %zu valid meshes extracted for RealtimeMesh", outMeshData.size());

        for (size_t i = 0; i < outMeshData.size(); ++i) {
            const auto& mesh = outMeshData[i];
            MIDDLEWARE_LOG_INFO("Mesh %zu '%s': %zu vertices, %zu triangles, %zu normals, %zu UVs",
                i, mesh.elementName.c_str(),
                mesh.points.size(),          // already vec3
                mesh.indices.size() / 3,     // indices to triangles
                mesh.normals.size(),         // already vec3
                mesh.uvs.size());            // already vec2

        }

        return true;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in LoadUSDBuffer: %s", e.what());
        stats.processingErrors.fetch_add(1);
        return false;
    }
}


// Enhanced disk loading with file validation
bool UsdProcessor::LoadUSDFromDisk(const std::string& filePath,
                                  std::vector<MeshData>& outMeshData,
                                  ProgressCallback progressCallback) {
    MIDDLEWARE_LOG_INFO("Loading USD from disk: %s", filePath.c_str());

    // Validate file path
    if (!validateFilePath(filePath, true)) {
        MIDDLEWARE_LOG_ERROR("Invalid file path: %s", filePath.c_str());
        return false;
    }

    try {
        if (progressCallback) {
            progressCallback(0.1f, "Reading file from disk");
        }

        // Read file with size validation
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            MIDDLEWARE_LOG_ERROR("Failed to open file: %s", filePath.c_str());
            return false;
        }

        auto fileSize = file.tellg();
        if (fileSize <= 0 || static_cast<size_t>(fileSize) > safety::MAX_BUFFER_SIZE) {
            MIDDLEWARE_LOG_ERROR("Invalid file size: %lld bytes", static_cast<long long>(fileSize));
            return false;
        }

        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> buffer;
        buffer.resize(static_cast<size_t>(fileSize));

        if (!file.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
            MIDDLEWARE_LOG_ERROR("Failed to read file contents: %s", filePath.c_str());
            return false;
        }

        if (progressCallback) {
            progressCallback(0.2f, "File loaded, processing USD");
        }

        // Use existing buffer processing
        return LoadUSDBuffer(buffer, filePath, outMeshData, nullptr, progressCallback);

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in LoadUSDFromDisk: %s - %s", filePath.c_str(), e.what());
        stats.processingErrors.fetch_add(1);
        return false;
    }
}

// Configuration methods with validation
void UsdProcessor::setMaxRecursionDepth(int32_t maxDepth) {
    if (maxDepth >= 1 && maxDepth <= 1000) {
        maxRecursionDepth.store(maxDepth);
        MIDDLEWARE_LOG_INFO("Max recursion depth set to %d", maxDepth);
    } else {
        MIDDLEWARE_LOG_ERROR("Invalid recursion depth: %d (must be 1-1000)", maxDepth);
    }
}

int32_t UsdProcessor::getMaxRecursionDepth() const {
    return maxRecursionDepth.load();
}

void UsdProcessor::setMemoryLimit(size_t limitMB) {
    if (limitMB >= 1) {
        memoryLimitMB.store(limitMB);
        pImpl->memoryLimitBytes.store(limitMB * 1024 * 1024); // sync internal gate (bytes)
        MIDDLEWARE_LOG_INFO("Memory limit set to %zu MB (%.2f GB)", limitMB, limitMB / 1024.0);
    } else {
        MIDDLEWARE_LOG_ERROR("Invalid memory limit: %zu MB (must be >= 1)", limitMB);
    }
}

size_t UsdProcessor::getMemoryLimit() const {
    return memoryLimitMB.load();
}

void UsdProcessor::setReferenceResolutionEnabled(bool enable) {
    referenceResolutionEnabled.store(enable);
    MIDDLEWARE_LOG_INFO("Reference resolution %s", enable ? "enabled" : "disabled");
}

bool UsdProcessor::isReferenceResolutionEnabled() const {
    return referenceResolutionEnabled.load();
}

UsdProcessor::ProcessingStats::Snapshot UsdProcessor::getProcessingStats() const {
    return stats.getSnapshot(); // Return copyable snapshot
}

void UsdProcessor::resetProcessingStats() {
    stats.reset();
    MIDDLEWARE_LOG_INFO("Processing statistics reset");
}

// Enhanced format validation
bool UsdProcessor::validateUSDFormat(const std::vector<uint8_t>& buffer, const std::string& fileName) {
    if (buffer.empty() || fileName.empty()) {
        return false;
    }

    // Check file extension
    std::string extension = std::filesystem::path(fileName).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    if (!isSupportedExtension(extension)) {
        return false;
    }

    // Basic content validation
    if (buffer.size() < 10) { // Minimum reasonable size
        return false;
    }

    // Check for USD magic bytes or text patterns — use string_view, no copy
    size_t checkLen = std::min(buffer.size(), static_cast<size_t>(1000));
    std::string_view content(reinterpret_cast<const char*>(buffer.data()), checkLen);

    return (content.find("#usda") != std::string::npos ||
            content.find("PXR-USDC") != std::string::npos ||
            content.find("def ") != std::string::npos ||
            content.find("over ") != std::string::npos);
}

// Static utility methods
std::vector<std::string> UsdProcessor::getSupportedExtensions() {
    return {".usd", ".usda", ".usdc", ".usdz"};
}

bool UsdProcessor::isSupportedExtension(const std::string& extension) {
    std::string ext = extension;
    if (!ext.empty() && ext[0] != '.') {
        ext = "." + ext;
    }
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    auto supported = getSupportedExtensions();
    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

// Private helper methods implementation

bool UsdProcessor::ProcessPrim(void* prim,
                               std::vector<MeshData>& meshDataArray,
                               std::vector<PointCloudData>* outPointCloudData,
                               const glm::mat4& parentTransform,
                               int32_t depth) {
    MIDDLEWARE_VALIDATE_POINTER(prim, "ProcessPrim");

    // Check recursion depth limit
    if (depth >= maxRecursionDepth.load()) {
        MIDDLEWARE_LOG_WARNING("Maximum recursion depth reached: %d", depth);
        return false;
    }

    if (shutdownRequested.load()) {
        MIDDLEWARE_LOG_DEBUG("Processing aborted: shutdown requested");
        return false;
    }

    try {
        const tinyusdz::Prim& usdPrim = *static_cast<const tinyusdz::Prim*>(prim);

        MIDDLEWARE_LOG_DEBUG("Processing prim: %s (type: %s, depth: %d)",
                           usdPrim.element_name().c_str(),
                           usdPrim.prim_type_name().c_str(),
                           depth);

        // Get and validate local transform
        glm::mat4 localTransform = GetLocalTransform(prim);
        if (!validateTransform(localTransform)) {
            MIDDLEWARE_LOG_WARNING("Invalid transform for prim: %s, using identity",
                                 usdPrim.element_name().c_str());
            localTransform = glm::mat4(1.0f);
        }

        // Compute world transform
        glm::mat4 worldTransform = parentTransform * localTransform;
        if (!validateTransform(worldTransform)) {
            MIDDLEWARE_LOG_ERROR("Invalid world transform computed for prim: %s",
                                usdPrim.element_name().c_str());
            return false;
        }

        // Check for point cloud primitive FIRST (def Points)
        const tinyusdz::GeomPoints* pts = usdPrim.as<tinyusdz::GeomPoints>();
        if (pts) {
            MIDDLEWARE_LOG_INFO("Found point cloud primitive: %s", usdPrim.element_name().c_str());

            PointCloudData pointData;
            pointData.elementName = usdPrim.element_name();
            pointData.typeName = usdPrim.prim_type_name();

            if (ExtractPointCloudData(const_cast<tinyusdz::GeomPoints*>(pts), pointData, worldTransform)) {
                if (pointData.isValid()) {
                    MIDDLEWARE_LOG_INFO("Extracted point cloud: %s (%zu positions, %zu colors)",
                        pointData.elementName.c_str(), pointData.positions.size(), pointData.vertex_colors.size());
                    if (outPointCloudData) {
                        outPointCloudData->push_back(std::move(pointData));
                    }
                }
            }
        }

        // Check if this is a mesh primitive (skip if already handled as point cloud)
        if (!pts) {
            const tinyusdz::GeomMesh* mesh = usdPrim.as<tinyusdz::GeomMesh>();
            if (mesh) {
                MIDDLEWARE_LOG_DEBUG("Found mesh primitive: %s", usdPrim.element_name().c_str());

                // Check memory limits before processing
                if (!checkMemoryLimit(sizeof(MeshData) + 1000000)) { // Estimate 1MB per mesh
                    MIDDLEWARE_LOG_ERROR("Memory limit would be exceeded processing mesh: %s",
                                        usdPrim.element_name().c_str());
                    return false;
                }

                MeshData meshData;
                meshData.elementName = usdPrim.element_name();
                meshData.typeName = usdPrim.prim_type_name();

                if (ExtractMeshData(const_cast<tinyusdz::GeomMesh*>(mesh), meshData, worldTransform)) {
                    if (meshData.isValid()) {
                        meshDataArray.push_back(std::move(meshData));
                        stats.meshesExtracted.fetch_add(1);
                        MIDDLEWARE_LOG_DEBUG("Successfully extracted mesh: %s (%zu vertices, %zu triangles)",
                                           meshData.elementName.c_str(),
                                           meshData.getVertexCount(),
                                           meshData.getTriangleCount());
                    } else {
                        MIDDLEWARE_LOG_WARNING("Extracted mesh data is invalid: %s",
                                             usdPrim.element_name().c_str());
                    }
                } else {
                    MIDDLEWARE_LOG_WARNING("Failed to extract mesh data: %s",
                                         usdPrim.element_name().c_str());
                }
            }
        }

        // Process children recursively - optimized parallel processing
        const auto& children = usdPrim.children();
        const size_t childCount = children.size();
        
        if (childCount > 1 && depth < 3) { // Limit parallelism depth to avoid overhead
            // Use parallel processing only for significant workloads
            // Threshold based on expected processing time
            const size_t PARALLEL_THRESHOLD = 4; // Process at least 4 children in parallel
            
            if (childCount >= PARALLEL_THRESHOLD) {
                // Use parallel processing with reduced thread creation overhead
                std::vector<std::future<bool>> futures;
                futures.reserve(childCount);
                std::vector<std::vector<MeshData>> childResults(childCount);
                std::vector<std::vector<PointCloudData>> childPCResults(childCount);
                
                for (size_t i = 0; i < childCount; ++i) {
                    futures.push_back(std::async(std::launch::async, [&, i]() {
                        std::vector<MeshData> localMeshData;
                        std::vector<PointCloudData> localPCData;
                        bool result = ProcessPrim(const_cast<tinyusdz::Prim*>(&children[i]),
                                                 localMeshData, &localPCData, worldTransform, depth + 1);
                        if (!result) {
                            MIDDLEWARE_LOG_WARNING("Failed to process child prim: %s",
                                                 children[i].element_name().c_str());
                        }
                        childResults[i] = std::move(localMeshData);
                        childPCResults[i] = std::move(localPCData);
                        return result;
                    }));
                }
                
                // Collect results efficiently
                for (size_t i = 0; i < futures.size(); ++i) {
                    try {
                        futures[i].get(); // Wait for completion
                        // Merge mesh results
                        auto& result = childResults[i];
                        if (!result.empty()) {
                            meshDataArray.reserve(meshDataArray.size() + result.size());
                            meshDataArray.insert(meshDataArray.end(),
                                                std::make_move_iterator(result.begin()),
                                                std::make_move_iterator(result.end()));
                        }
                        // Merge point cloud results
                        auto& pcResult = childPCResults[i];
                        if (!pcResult.empty() && outPointCloudData) {
                            outPointCloudData->reserve(outPointCloudData->size() + pcResult.size());
                            outPointCloudData->insert(outPointCloudData->end(),
                                                      std::make_move_iterator(pcResult.begin()),
                                                      std::make_move_iterator(pcResult.end()));
                        }
                    } catch (const std::exception& e) {
                        MIDDLEWARE_LOG_ERROR("Exception processing child %zu: %s", i, e.what());
                    }
                }
            } else {
                // Sequential processing for small numbers
                for (const auto& child : children) {
                    if (!ProcessPrim(const_cast<tinyusdz::Prim*>(&child),
                                    meshDataArray, outPointCloudData, worldTransform, depth + 1)) {
                        MIDDLEWARE_LOG_WARNING("Failed to process child prim: %s",
                                             child.element_name().c_str());
                    }
                }
            }
        } else {
            // Sequential processing for small numbers or deep recursion
            for (const auto& child : children) {
                if (!ProcessPrim(const_cast<tinyusdz::Prim*>(&child),
                                meshDataArray, outPointCloudData, worldTransform, depth + 1)) {
                    MIDDLEWARE_LOG_WARNING("Failed to process child prim: %s",
                                         child.element_name().c_str());
                }
            }
        }

        return true;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in ProcessPrim at depth %d: %s", depth, e.what());
        stats.processingErrors.fetch_add(1);
        return false;
    }
}

bool UsdProcessor::ExtractMeshData(void* mesh,
                                  MeshData& outMeshData,
                                  const glm::mat4& worldTransform) {
    MIDDLEWARE_VALIDATE_POINTER(mesh, "ExtractMeshData");
    if (!validateTransform(worldTransform)) {
        MIDDLEWARE_LOG_ERROR("Invalid world transform in ExtractMeshData");
        return false;
    }

    try {
        tinyusdz::GeomMesh* geomMesh = static_cast<tinyusdz::GeomMesh*>(mesh);
                // NEW: Extract subdivision scheme
        // NEW: Extract subdivision scheme
        std::string subdivScheme = "none";
        try {
            // TinyUSDZ returns the value directly, not via pointer
            auto subdivValue = geomMesh->subdivisionScheme.get_value();
            // Convert SubdivisionScheme enum to string
            if (subdivValue == tinyusdz::GeomMesh::SubdivisionScheme::CatmullClark) {
                subdivScheme = "catmullClark";
            } else if (subdivValue == tinyusdz::GeomMesh::SubdivisionScheme::Loop) {
                subdivScheme = "loop";
            } else if (subdivValue == tinyusdz::GeomMesh::SubdivisionScheme::Bilinear) {
                subdivScheme = "bilinear";
            } else {
                subdivScheme = "none";
            }
            MIDDLEWARE_LOG_DEBUG("Mesh '%s' has subdivision scheme: %s",
                                outMeshData.elementName.c_str(), subdivScheme.c_str());
        } catch (...) {
            subdivScheme = "none";
        }
        outMeshData.subdivisionScheme = subdivScheme;

        // NEW: Extract doubleSided attribute
        try {
            bool doubleSidedValue = geomMesh->doubleSided.get_value();
            outMeshData.doubleSided = doubleSidedValue;
            MIDDLEWARE_LOG_DEBUG("Mesh '%s' doubleSided: %d",
                                outMeshData.elementName.c_str(), outMeshData.doubleSided);
        } catch (...) {
            outMeshData.doubleSided = false;
        }


        // Extract points with validation
        auto points = geomMesh->get_points();
        if (points.empty()) {
            MIDDLEWARE_LOG_WARNING("Mesh has no points: %s", outMeshData.elementName.c_str());
            return false;
        }

        if (points.size() > safety::MAX_MESH_VERTICES) {
            MIDDLEWARE_LOG_ERROR("Mesh has too many vertices: %zu (max: %zu)",
                                points.size(), safety::MAX_MESH_VERTICES);
            return false;
        }

        // Performance optimization: Only log debug info for large meshes or in debug mode
        if (points.size() > 10000) {
            MIDDLEWARE_LOG_DEBUG("Extracting large mesh with %zu points", points.size());
        }

        // Start timing for performance measurement
        auto vertexStartTime = std::chrono::high_resolution_clock::now();

        // Transform and validate points with GPU acceleration and CPU fallback
#ifdef ENABLE_CUDA_ACCELERATION
        pImpl->transformVerticesWithGpu(points, worldTransform, outMeshData.points);
#else
        outMeshData.points.clear();
        outMeshData.points.resize(points.size());  // Pre-allocate exact size
        
        // Extract matrix components for faster access
        const float* m = &worldTransform[0][0];
        
        // Helper lambda for vertex transformation
        auto transformVertex = [m](const tinyusdz::value::point3f& pt) -> glm::vec3 {
            const float x = static_cast<float>(pt.x);
            const float y = static_cast<float>(pt.y);
            const float z = static_cast<float>(pt.z);
            
            // Fast NaN/Inf check using integer representation
            const int32_t* ix = reinterpret_cast<const int32_t*>(&x);
            const int32_t* iy = reinterpret_cast<const int32_t*>(&y);
            const int32_t* iz = reinterpret_cast<const int32_t*>(&z);
            
            // Check if any component is NaN or Inf (exponent bits all 1s)
            if ((*ix & 0x7F800000) == 0x7F800000 ||
                (*iy & 0x7F800000) == 0x7F800000 ||
                (*iz & 0x7F800000) == 0x7F800000) {
                return glm::vec3(0.0f, 0.0f, 0.0f);
            }

            // Manual matrix multiplication - optimized
            const float tx = m[0] * x + m[4] * y + m[8] * z + m[12];
            const float ty = m[1] * x + m[5] * y + m[9] * z + m[13];
            const float tz = m[2] * x + m[6] * y + m[10] * z + m[14];
            
            // Fast validation of transformed vertex
            const int32_t* itx = reinterpret_cast<const int32_t*>(&tx);
            const int32_t* ity = reinterpret_cast<const int32_t*>(&ty);
            const int32_t* itz = reinterpret_cast<const int32_t*>(&tz);
            
            if ((*itx & 0x7F800000) == 0x7F800000 ||
                (*ity & 0x7F800000) == 0x7F800000 ||
                (*itz & 0x7F800000) == 0x7F800000) {
                return glm::vec3(x, y, z);
            }
            
            return glm::vec3(tx, ty, tz);
        };
        
        // PARALLEL vertex processing using std::transform
        std::transform(PAR_POLICY
                      points.begin(), points.end(),
                      outMeshData.points.begin(),
                      transformVertex);
#endif
        
        // Count non-finite vertices for logging
        size_t nonFiniteCount = std::count_if(PAR_POLICY
                                            outMeshData.points.begin(),
                                            outMeshData.points.end(),
                                            [](const glm::vec3& v) {
                                                const int32_t* ix = reinterpret_cast<const int32_t*>(&v.x);
                                                const int32_t* iy = reinterpret_cast<const int32_t*>(&v.y);
                                                const int32_t* iz = reinterpret_cast<const int32_t*>(&v.z);
                                                return (*ix & 0x7F800000) == 0x7F800000 ||
                                                       (*iy & 0x7F800000) == 0x7F800000 ||
                                                       (*iz & 0x7F800000) == 0x7F800000;
                                            });
        
        if (nonFiniteCount > 0) {
            MIDDLEWARE_LOG_WARNING("%zu non-finite vertices detected and zeroed", nonFiniteCount);
        }
        
        // Log performance metrics
        auto vertexEndTime = std::chrono::high_resolution_clock::now();
        auto vertexDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            vertexEndTime - vertexStartTime).count();
        
        if (points.size() > 1000) {
            MIDDLEWARE_LOG_DEBUG("Parallel vertex processing: %zu vertices in %lld µs (%.2f µs/vertex)",
                               points.size(), vertexDuration,
                               static_cast<float>(vertexDuration) / points.size());
        }

        if (outMeshData.points.empty()) {
            MIDDLEWARE_LOG_ERROR("No valid points after transformation");
            return false;
        }

        // Extract and triangulate faces
        auto faceVertexCounts = geomMesh->get_faceVertexCounts();
        auto faceVertexIndices = geomMesh->get_faceVertexIndices();

        outMeshData.faceVertexCounts.clear();
        outMeshData.faceVertexCounts.reserve(faceVertexCounts.size());
        for (const auto& count : faceVertexCounts) {
            outMeshData.faceVertexCounts.push_back(static_cast<uint32_t>(count));
        }

        bool bHasFaces = !faceVertexCounts.empty() && !faceVertexIndices.empty();
        if (!bHasFaces) {
            MIDDLEWARE_LOG_INFO("No face data for %s — treating as point cloud (%zu points)",
                outMeshData.elementName.c_str(), outMeshData.points.size());
        }

        std::vector<uint32_t> finalIndices;
        if (bHasFaces) {
            if (subdivScheme == "none") {
                MIDDLEWARE_LOG_DEBUG("Triangulating mesh (subdivScheme=none)");
                size_t totalTriangles = 0;
                for (int32_t count : faceVertexCounts) {
                    if (count >= 3 && count <= 100) totalTriangles += (count - 2);
                }
                finalIndices.reserve(totalTriangles * 3);
                size_t indexOffset = 0;
                const size_t vertexCount = outMeshData.points.size();
                for (size_t faceIdx = 0; faceIdx < faceVertexCounts.size(); ++faceIdx) {
                    int32_t numVertsInFace = faceVertexCounts[faceIdx];
                    if (numVertsInFace < 3 || numVertsInFace > 100) { indexOffset += numVertsInFace; continue; }
                    if (indexOffset + numVertsInFace > faceVertexIndices.size()) break;
                    uint32_t baseIdx = static_cast<uint32_t>(faceVertexIndices[indexOffset]);
                    if (baseIdx >= vertexCount) { indexOffset += numVertsInFace; continue; }
                    for (int32_t triIdx = 0; triIdx < numVertsInFace - 2; ++triIdx) {
                        uint32_t idx1 = static_cast<uint32_t>(faceVertexIndices[indexOffset + triIdx + 1]);
                        uint32_t idx2 = static_cast<uint32_t>(faceVertexIndices[indexOffset + triIdx + 2]);
                        if (idx1 >= vertexCount || idx2 >= vertexCount) continue;
                        finalIndices.push_back(baseIdx);
                        finalIndices.push_back(idx1);
                        finalIndices.push_back(idx2);
                    }
                    indexOffset += numVertsInFace;
                }
            } else {
                MIDDLEWARE_LOG_DEBUG("Preserving original topology for subdivision (scheme=%s)", subdivScheme.c_str());
                finalIndices.reserve(faceVertexIndices.size());
                const size_t vertexCount = outMeshData.points.size();
                for (const auto& idx : faceVertexIndices) {
                    if (static_cast<size_t>(idx) >= vertexCount) continue;
                    finalIndices.push_back(static_cast<uint32_t>(idx));
                }
            }

            if (finalIndices.empty()) { MIDDLEWARE_LOG_WARNING("No valid indices generated"); return false; }
            outMeshData.indices = std::move(finalIndices);
        }

        // Extract normals
        auto normals = geomMesh->get_normals();
        if (!normals.empty()) {
            if (normals.size() != points.size()) {
                MIDDLEWARE_LOG_WARNING("Normal count (%zu) doesn't match vertex count (%zu)",
                                     normals.size(), points.size());
            } else {
                auto normalStartTime = std::chrono::high_resolution_clock::now();
                
                outMeshData.normals.clear();
                outMeshData.normals.resize(normals.size());  // Pre-allocate exact size
                glm::mat3 normalMatrix = glm::mat3(worldTransform);

                // Helper lambda for normal transformation
                auto transformNormal = [normalMatrix](const tinyusdz::value::normal3f& nrm) -> glm::vec3 {
                    // Fast NaN/Inf check
                    const int32_t* ix = reinterpret_cast<const int32_t*>(&nrm.x);
                    const int32_t* iy = reinterpret_cast<const int32_t*>(&nrm.y);
                    const int32_t* iz = reinterpret_cast<const int32_t*>(&nrm.z);
                    
                    if ((*ix & 0x7F800000) == 0x7F800000 ||
                        (*iy & 0x7F800000) == 0x7F800000 ||
                        (*iz & 0x7F800000) == 0x7F800000) {
                        return glm::vec3(0.0f, 1.0f, 0.0f);  // Default up vector
                    }

                    glm::vec3 normalVec(static_cast<float>(nrm.x),
                                      static_cast<float>(nrm.y),
                                      static_cast<float>(nrm.z));
                    glm::vec3 transformedNormal = normalMatrix * normalVec;

                    // Fast length calculation and normalization
                    float lengthSquared = glm::dot(transformedNormal, transformedNormal);
                    if (lengthSquared > safety::EPSILON * safety::EPSILON) {
                        float invLength = 1.0f / std::sqrt(lengthSquared);
                        return transformedNormal * invLength;
                    }
                    
                    return glm::vec3(0.0f, 1.0f, 0.0f);  // Default up vector
                };

                // PARALLEL normal processing
                std::transform(PAR_POLICY
                             normals.begin(), normals.end(),
                             outMeshData.normals.begin(),
                             transformNormal);

                // Count invalid normals for logging
                size_t invalidNormalCount = std::count_if(PAR_POLICY
                                                        outMeshData.normals.begin(),
                                                        outMeshData.normals.end(),
                                                        [](const glm::vec3& n) {
                                                            return n == glm::vec3(0.0f, 1.0f, 0.0f);
                                                        });
                
                if (invalidNormalCount > 0) {
                    MIDDLEWARE_LOG_DEBUG("%zu invalid normals replaced with default up vector", invalidNormalCount);
                }
                
                auto normalEndTime = std::chrono::high_resolution_clock::now();
                auto normalDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                    normalEndTime - normalStartTime).count();
                
                if (normals.size() > 1000) {
                    MIDDLEWARE_LOG_DEBUG("Parallel normal processing: %zu normals in %lld µs",
                                       normals.size(), normalDuration);
                }
            }
        }

        // If no normals provided, calculate them
        if (outMeshData.normals.empty()) {
            if (!calculateMeshNormals(outMeshData.points, outMeshData.indices, outMeshData.normals)) {
                MIDDLEWARE_LOG_WARNING("Failed to calculate normals for mesh: %s",
                                     outMeshData.elementName.c_str());
            }
        }

        // Extract UV coordinates
        extractUVCoordinates(geomMesh, outMeshData);

        // ✅ NEW: Extract vertex colors from primvars:color.timeSamples
        extractVertexColors(geomMesh, outMeshData);

        MIDDLEWARE_LOG_DEBUG("Successfully extracted mesh: %zu vertices, %zu triangles, %zu normals, %zu UVs, %zu colors",
                           outMeshData.points.size(),
                           outMeshData.indices.size() / 3,
                           outMeshData.normals.size(),
                           outMeshData.uvs.size(),
                           outMeshData.vertex_colors.size() / 4);

        return true;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in ExtractMeshData: %s", e.what());
        stats.processingErrors.fetch_add(1);
        return false;
    }
}

    void UsdProcessor::extractVertexColors(tinyusdz::GeomMesh* mesh, MeshData& meshData) {
    if (!mesh) return;

    try {
        // Clear any existing vertex colors
        meshData.vertex_colors.clear();

        // Try to get vertex colors from primvars:color
        tinyusdz::GeomPrimvar colorPrimvar;
        std::string primvarErr;

        // Try different color attribute names
        const std::vector<std::string> colorNames = {
            "primvars:color", "color", "primvars:displayColor", "displayColor",
            "primvars:Cd", "Cd"  // Common in Houdini/Maya
        };

        bool foundColors = false;
        for (const auto& name : colorNames) {
            if (mesh->get_primvar(name, &colorPrimvar, &primvarErr)) {
                // Try to get color values as different types
                std::vector<tinyusdz::value::color3f> color3fValues;
                std::vector<tinyusdz::value::color4f> color4fValues;
                std::vector<tinyusdz::value::float3> float3Values;
                std::vector<tinyusdz::value::float4> float4Values;

                // Try color4f first (RGBA)
                if (colorPrimvar.get_value(&color4fValues)) {
                    MIDDLEWARE_LOG_DEBUG("Found %zu RGBA vertex colors in primvar: %s",
                                       color4fValues.size(), name.c_str());

                    meshData.vertex_colors.reserve(color4fValues.size());
                    for (const auto& color : color4fValues) {
                        // Validate color values
                        if (std::isfinite(color.r) && std::isfinite(color.g) &&
                            std::isfinite(color.b) && std::isfinite(color.a)) {
                            meshData.vertex_colors.push_back(glm::vec4(
                                static_cast<float>(color.r),
                                static_cast<float>(color.g),
                                static_cast<float>(color.b),
                                static_cast<float>(color.a)
                            ));
                        } else {
                            // Default white color for invalid values
                            meshData.vertex_colors.push_back(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                        }
                    }
                    foundColors = true;
                    break;
                }
                // Try color3f (RGB, add alpha)
                else if (colorPrimvar.get_value(&color3fValues)) {
                    MIDDLEWARE_LOG_DEBUG("Found %zu RGB vertex colors in primvar: %s",
                                       color3fValues.size(), name.c_str());

                    meshData.vertex_colors.reserve(color3fValues.size());
                    for (const auto& color : color3fValues) {
                        // Validate color values
                        if (std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b)) {
                            meshData.vertex_colors.push_back(glm::vec4(
                                static_cast<float>(color.r),
                                static_cast<float>(color.g),
                                static_cast<float>(color.b),
                                1.0f  // Default alpha
                            ));
                        } else {
                            // Default white color for invalid values
                            meshData.vertex_colors.push_back(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                        }
                    }
                    foundColors = true;
                    break;
                }
                // Try float4 (generic RGBA)
                else if (colorPrimvar.get_value(&float4Values)) {
                    MIDDLEWARE_LOG_DEBUG("Found %zu float4 vertex colors in primvar: %s",
                                       float4Values.size(), name.c_str());

                    meshData.vertex_colors.reserve(float4Values.size());
                    for (const auto& color : float4Values) {
                        // Validate color values
                        if (std::isfinite(color[0]) && std::isfinite(color[1]) &&
                            std::isfinite(color[2]) && std::isfinite(color[3])) {
                            meshData.vertex_colors.push_back(glm::vec4(
                                static_cast<float>(color[0]),
                                static_cast<float>(color[1]),
                                static_cast<float>(color[2]),
                                static_cast<float>(color[3])
                            ));
                        } else {
                            // Default white color for invalid values
                            meshData.vertex_colors.push_back(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                        }
                    }
                    foundColors = true;
                    break;
                }
                // Try float3 (generic RGB)
                else if (colorPrimvar.get_value(&float3Values)) {
                    MIDDLEWARE_LOG_DEBUG("Found %zu float3 vertex colors in primvar: %s",
                                       float3Values.size(), name.c_str());

                    meshData.vertex_colors.reserve(float3Values.size());
                    for (const auto& color : float3Values) {
                        // Validate color values
                        if (std::isfinite(color[0]) && std::isfinite(color[1]) && std::isfinite(color[2])) {
                            meshData.vertex_colors.push_back(glm::vec4(
                                static_cast<float>(color[0]),
                                static_cast<float>(color[1]),
                                static_cast<float>(color[2]),
                                1.0f  // Default alpha
                            ));
                        } else {
                            // Default white color for invalid values
                            meshData.vertex_colors.push_back(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                        }
                    }
                    foundColors = true;
                    break;
                }
            }
        }

        if (foundColors) {
            // Validate that color count matches vertex count
            if (meshData.vertex_colors.size() != meshData.points.size()) {
                MIDDLEWARE_LOG_WARNING("Vertex color count (%zu) doesn't match vertex count (%zu) for mesh: %s",
                                     meshData.vertex_colors.size(), meshData.points.size(), meshData.elementName.c_str());

                // Resize to match vertex count
                if (meshData.vertex_colors.size() > meshData.points.size()) {
                    meshData.vertex_colors.resize(meshData.points.size());
                } else {
                    // Fill missing colors with white
                    while (meshData.vertex_colors.size() < meshData.points.size()) {
                        meshData.vertex_colors.push_back(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                    }
                }
            }

            MIDDLEWARE_LOG_INFO("Successfully extracted %zu vertex colors for mesh: %s",
                              meshData.vertex_colors.size(), meshData.elementName.c_str());
        } else {
            MIDDLEWARE_LOG_DEBUG("No vertex colors found for mesh: %s", meshData.elementName.c_str());
        }

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception extracting vertex colors: %s", e.what());
        meshData.vertex_colors.clear(); // Clear on error
    }
}


glm::mat4 UsdProcessor::GetLocalTransform(void* prim) {
    MIDDLEWARE_VALIDATE_POINTER(prim, "GetLocalTransform");

    try {
        const tinyusdz::Prim& usdPrim = *static_cast<const tinyusdz::Prim*>(prim);
        glm::mat4 localTransform(1.0f); // Identity matrix

        const tinyusdz::Xformable* xformable = nullptr;
        if (!tinyusdz::CastToXformable(usdPrim, &xformable) || !xformable) {
            return localTransform; // Return identity if not transformable
        }

        MIDDLEWARE_LOG_DEBUG("Processing %zu transform operations for prim: %s",
                           xformable->xformOps.size(), usdPrim.element_name().c_str());

        for (const auto& op : xformable->xformOps) {
            try {
                switch (op.op_type) {
                    case tinyusdz::XformOp::OpType::Translate: {
                        tinyusdz::value::double3 trans;
                        if (op.get_interpolated_value(&trans)) {
                            // Validate translation values
                            if (std::isfinite(trans[0]) && std::isfinite(trans[1]) && std::isfinite(trans[2])) {
                                glm::vec3 translation(static_cast<float>(trans[0]),
                                                    static_cast<float>(trans[1]),
                                                    static_cast<float>(trans[2]));
                                localTransform = glm::translate(localTransform, translation);
                                MIDDLEWARE_LOG_DEBUG("Applied translation: (%f, %f, %f)",
                                                   translation.x, translation.y, translation.z);
                            } else {
                                MIDDLEWARE_LOG_WARNING("Non-finite translation values detected, skipping");
                            }
                        }
                        break;
                    }

                    case tinyusdz::XformOp::OpType::Scale: {
                        tinyusdz::value::double3 scale;
                        if (op.get_interpolated_value(&scale)) {
                            if (std::isfinite(scale[0]) && std::isfinite(scale[1]) && std::isfinite(scale[2]) &&
                                scale[0] > safety::EPSILON && scale[1] > safety::EPSILON && scale[2] > safety::EPSILON) {
                                glm::vec3 scaleVec(static_cast<float>(scale[0]),
                                                 static_cast<float>(scale[1]),
                                                 static_cast<float>(scale[2]));
                                localTransform = glm::scale(localTransform, scaleVec);
                                MIDDLEWARE_LOG_DEBUG("Applied scale: (%f, %f, %f)",
                                                   scaleVec.x, scaleVec.y, scaleVec.z);
                            } else {
                                MIDDLEWARE_LOG_WARNING("Invalid scale values detected, skipping");
                            }
                        }
                        break;
                    }

                    case tinyusdz::XformOp::OpType::RotateXYZ: {
                        tinyusdz::value::double3 rot;
                        if (op.get_interpolated_value(&rot)) {
                            if (std::isfinite(rot[0]) && std::isfinite(rot[1]) && std::isfinite(rot[2])) {
                                // Apply rotations in XYZ order
                                localTransform = glm::rotate(localTransform,
                                    glm::radians(static_cast<float>(rot[0])), glm::vec3(1.0f, 0.0f, 0.0f));
                                localTransform = glm::rotate(localTransform,
                                    glm::radians(static_cast<float>(rot[1])), glm::vec3(0.0f, 1.0f, 0.0f));
                                localTransform = glm::rotate(localTransform,
                                    glm::radians(static_cast<float>(rot[2])), glm::vec3(0.0f, 0.0f, 1.0f));
                                MIDDLEWARE_LOG_DEBUG("Applied rotation XYZ: (%f, %f, %f)",
                                                   rot[0], rot[1], rot[2]);
                            } else {
                                MIDDLEWARE_LOG_WARNING("Non-finite rotation values detected, skipping");
                            }
                        }
                        break;
                    }

                    default:
                        MIDDLEWARE_LOG_DEBUG("Unsupported transform operation type: %d",
                                           static_cast<int>(op.op_type));
                        break;
                }

            } catch (const std::exception& e) {
                MIDDLEWARE_LOG_ERROR("Error processing transform operation: %s", e.what());
                continue; // Skip this operation, continue with others
            }
        }

        // Validate final transformation matrix
        if (!validateTransform(localTransform)) {
            MIDDLEWARE_LOG_ERROR("Final transformation matrix is invalid, returning identity");
            return glm::mat4(1.0f);
        }

        return localTransform;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in GetLocalTransform: %s", e.what());
        return glm::mat4(1.0f);
    }
}

// MISSING METHOD IMPLEMENTATIONS - These were causing linker errors

void UsdProcessor::ExtractReferencePaths(const tinyusdz::Stage& stage,
                                        std::vector<std::string>& outReferencePaths) {
    MIDDLEWARE_LOG_INFO("Extracting reference paths from stage");

    // Process each root prim
    for (const auto& rootPrim : stage.root_prims()) {
        ExtractReferencePathsFromPrim(rootPrim, outReferencePaths);
    }
}

std::vector<std::string> UsdProcessor::ExtractClipsFromRawContent(const std::vector<uint8_t>& buffer) {
    // Pass the buffer straight through (no full-buffer std::string copy).
    return pImpl->extractClipsFromString(
        std::string_view(reinterpret_cast<const char*>(buffer.data()), buffer.size()));
}

std::vector<std::string> UsdProcessor::UsdProcessorImpl::extractClipsFromString(std::string_view content) {
    std::vector<std::string> clipPaths;

    // Look for clips patterns in the USD content (regex compiled once, not per call).
    static const std::regex clipsPattern(R"(asset\[\]\s+assetPaths\s*=\s*\[@([^@]+)@\])");

    // std::regex_iterator<const char*> matches std::string_view (const char* base);
    // std::sregex_iterator is only bound to std::string iterators.
    std::regex_iterator<const char*> it(content.begin(), content.end(), clipsPattern);
    const std::regex_iterator<const char*> end;

    while (it != end) {
        std::string clipPath = (*it)[1].str();
        MIDDLEWARE_LOG_DEBUG("Found clip asset path: %s", clipPath.c_str());
        clipPaths.push_back(std::move(clipPath));
        ++it;
    }

    return clipPaths;
}

void UsdProcessor::ExtractReferencePathsFromPrim(const tinyusdz::Prim& prim,
                                                std::vector<std::string>& outReferencePaths) {
    // Check for references in this prim
    if (prim.metas().references.has_value()) {
        const auto& refs = prim.metas().references.value();

        // Access the references vector (second element of the pair)
        const auto& references = refs.second;

        // For each reference in the vector
        for (const auto& ref : references) {
            std::string assetPath = ref.asset_path.GetAssetPath();
            if (!assetPath.empty()) {
                MIDDLEWARE_LOG_INFO("Found reference: %s", assetPath.c_str());
                outReferencePaths.push_back(assetPath);
            }
        }
    }

    // Check for payloads in this prim
    if (prim.metas().payload.has_value()) {
        const auto& pl = prim.metas().payload.value();

        // Access the payloads vector (second element of the pair)
        const auto& payloads = pl.second;

        // For each payload in the vector
        for (const auto& payload : payloads) {
            std::string assetPath = payload.asset_path.GetAssetPath();
            if (!assetPath.empty()) {
                MIDDLEWARE_LOG_INFO("Found payload: %s", assetPath.c_str());
                outReferencePaths.push_back(assetPath);
            }
        }
    }

    // Recursively check child prims
    for (const auto& child : prim.children()) {
        ExtractReferencePathsFromPrim(child, outReferencePaths);
    }
}

void UsdProcessor::ListPrimHierarchy(const tinyusdz::Prim& prim, int depth) {
    std::string indent(depth * 2, ' ');
    MIDDLEWARE_LOG_INFO("%s- %s (%s)", indent.c_str(),
                      prim.element_name().c_str(),
                      prim.prim_type_name().c_str());

    for (const auto& child : prim.children()) {
        ListPrimHierarchy(child, depth + 1);
    }
}

// Additional private helper methods
    bool UsdProcessor::validateTransform(const glm::mat4& transform) const {
    // Check for finite values (you already do this)
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (!std::isfinite(transform[i][j])) {
                return false;
            }
        }
    }

    // ADD THIS: Check determinant to avoid singular matrices
    float det = glm::determinant(transform);
    if (std::abs(det) < safety::EPSILON) {
        MIDDLEWARE_LOG_WARNING("Matrix is singular (determinant near zero)");
        return false;
    }

    return true;
}

bool UsdProcessor::validateFilePath(const std::string& filePath, bool checkExistence) const {
    if (filePath.empty()) {
        MIDDLEWARE_LOG_ERROR("File path is empty");
        return false;
    }

    if (filePath.size() > 1000) { // Reasonable path length limit
        MIDDLEWARE_LOG_ERROR("File path too long: %zu characters", filePath.size());
        return false;
    }

    // Check for dangerous path patterns
    if (filePath.find("..") != std::string::npos) {
        MIDDLEWARE_LOG_ERROR("Path traversal detected in file path: %s", filePath.c_str());
        return false;
    }

    if (checkExistence) {
        try {
            if (!std::filesystem::exists(filePath)) {
                MIDDLEWARE_LOG_ERROR("File does not exist: %s", filePath.c_str());
                return false;
            }
        } catch (const std::filesystem::filesystem_error& e) {
            MIDDLEWARE_LOG_ERROR("Filesystem error checking file: %s - %s", filePath.c_str(), e.what());
            return false;
        }
    }

    return true;
}

std::vector<uint8_t> UsdProcessor::preprocessUsdContent(const std::vector<uint8_t>& buffer) {
    return pImpl->preprocessUsdContent(buffer);
}

bool UsdProcessor::checkMemoryLimit(size_t additionalBytes) const {
    return pImpl->checkMemoryUsage(additionalBytes);
}

void UsdProcessor::normalizeUVCoordinates(std::vector<glm::vec2>& uvs) {
    for (auto& uv : uvs) {
        // Clamp UV coordinates to reasonable range
        uv.x = std::clamp(uv.x, -10.0f, 10.0f);
        uv.y = std::clamp(uv.y, -10.0f, 10.0f);

        // Validate finite values
        if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
            MIDDLEWARE_LOG_WARNING("Non-finite UV coordinate detected, setting to (0,0)");
            uv = glm::vec2(0.0f, 0.0f);
        }
    }
}

void UsdProcessor::normalizeUVCoordinatesParallel(std::vector<glm::vec2>& uvs) {
    if (uvs.size() < 1000) {
        // For small UV sets, sequential is faster due to parallel overhead
        normalizeUVCoordinates(uvs);
        return;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // PARALLEL UV normalization
    std::for_each(PAR_POLICY uvs.begin(), uvs.end(), [](glm::vec2& uv) {
        // Fast NaN/Inf check using integer representation
        const int32_t* ix = reinterpret_cast<const int32_t*>(&uv.x);
        const int32_t* iy = reinterpret_cast<const int32_t*>(&uv.y);
        
        if ((*ix & 0x7F800000) == 0x7F800000 || (*iy & 0x7F800000) == 0x7F800000) {
            uv = glm::vec2(0.0f, 0.0f);
        } else {
            // Clamp UV coordinates to reasonable range
            uv.x = std::clamp(uv.x, -10.0f, 10.0f);
            uv.y = std::clamp(uv.y, -10.0f, 10.0f);
        }
    });
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();
    
    if (uvs.size() > 10000) {
        MIDDLEWARE_LOG_DEBUG("Parallel UV normalization: %zu UVs in %lld µs", uvs.size(), duration);
    }
}

bool UsdProcessor::validateMeshIndices(std::vector<uint32_t>& indices, size_t vertexCount) {
    if (indices.empty()) {
        return true; // Empty indices are valid
    }

    if (indices.size() % 3 != 0) {
        MIDDLEWARE_LOG_ERROR("Index count is not a multiple of 3: %zu", indices.size());
        return false;
    }

    // Validate all indices are within bounds
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= vertexCount) {
            MIDDLEWARE_LOG_ERROR("Index %u at position %zu exceeds vertex count %zu",
                                indices[i], i, vertexCount);
            return false;
        }
    }

    return true;
}

bool UsdProcessor::calculateMeshNormals(const std::vector<glm::vec3>& points,
                                       const std::vector<uint32_t>& indices,
                                       std::vector<glm::vec3>& outNormals) {
    if (points.empty() || indices.empty() || indices.size() % 3 != 0) {
        return false;
    }

    try {
        outNormals.clear();
        outNormals.resize(points.size(), glm::vec3(0.0f));

        // Calculate face normals and accumulate vertex normals
        for (size_t i = 0; i < indices.size(); i += 3) {
            uint32_t idx0 = indices[i];
            uint32_t idx1 = indices[i + 1];
            uint32_t idx2 = indices[i + 2];

            // Validate indices
            if (idx0 >= points.size() || idx1 >= points.size() || idx2 >= points.size()) {
                continue;
            }

            const glm::vec3& v0 = points[idx0];
            const glm::vec3& v1 = points[idx1];
            const glm::vec3& v2 = points[idx2];

            // Calculate face normal
            glm::vec3 edge1 = v1 - v0;
            glm::vec3 edge2 = v2 - v0;
            glm::vec3 faceNormal = glm::cross(edge1, edge2);

            // Validate face normal
            float length = glm::length(faceNormal);
            if (length > safety::EPSILON) {
                faceNormal = faceNormal / length;

                // Accumulate to vertex normals
                outNormals[idx0] += faceNormal;
                outNormals[idx1] += faceNormal;
                outNormals[idx2] += faceNormal;
            }
        }

        // Normalize vertex normals
        for (auto& normal : outNormals) {
            float length = glm::length(normal);
            if (length > safety::EPSILON) {
                normal = normal / length;
            } else {
                normal = glm::vec3(0.0f, 1.0f, 0.0f); // Default up vector
            }
        }

        return true;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in calculateMeshNormals: %s", e.what());
        return false;
    }
}

void UsdProcessor::extractUVCoordinates(tinyusdz::GeomMesh* mesh, MeshData& meshData) {
    if (!mesh) return;

    try {
        auto uvStartTime = std::chrono::high_resolution_clock::now();
        
        // ✅ MODIFIED: Support multiple UV sets
        const std::vector<std::string> uvNames = {
            "primvars:st", "st",
            "primvars:st1", "st1",
            "primvars:st2", "st2",
            "primvars:uv", "uv",
            "primvars:uv0", "uv0",
            "primvars:uv1", "uv1",
            "primvars:map1", "map1",
            "primvars:attribute0", "attribute0"
        };

        meshData.uvSets.clear();
        meshData.uvSetNames.clear();

        for (const auto& name : uvNames) {
            tinyusdz::GeomPrimvar primvar;
            std::string primvarErr;

            if (mesh->get_primvar(name, &primvar, &primvarErr)) {
                std::vector<tinyusdz::value::texcoord2f> uvs;

                if (primvar.get_value(&uvs)) {
                    std::vector<glm::vec2> uvChannel;
                    uvChannel.resize(uvs.size());  // Pre-allocate exact size

                    // PARALLEL UV extraction
                    std::transform(PAR_POLICY
                                 uvs.begin(), uvs.end(),
                                 uvChannel.begin(),
                                 [](const tinyusdz::value::texcoord2f& uv) {
                                     return glm::vec2(uv.s, uv.t);
                                 });

                    // Normalize and validate UVs (parallel)
                    normalizeUVCoordinatesParallel(uvChannel);

                    meshData.uvSets.push_back(std::move(uvChannel));  // Move to avoid copy
                    meshData.uvSetNames.push_back(name);

                    MIDDLEWARE_LOG_DEBUG("Found UV set '%s' with %zu coordinates", name.c_str(), uvs.size());
                }
            }
        }

        // Backward compatibility: copy first UV set to uvs
        if (!meshData.uvSets.empty()) {
            meshData.uvs = meshData.uvSets[0];
            
            auto uvEndTime = std::chrono::high_resolution_clock::now();
            auto uvDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                uvEndTime - uvStartTime).count();
            
            MIDDLEWARE_LOG_DEBUG("Mesh '%s' has %zu UV channels processed in %lld µs",
                               meshData.elementName.c_str(), meshData.uvSets.size(), uvDuration);

        } else {
            MIDDLEWARE_LOG_DEBUG("No UV coordinates found for mesh '%s'", meshData.elementName.c_str());
        }

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception extracting UV coordinates: %s", e.what());
        meshData.uvSets.clear();
        meshData.uvSetNames.clear();
    }
}


// Helper methods for reference resolution
bool UsdProcessor::hasEmptyGeometry(const std::vector<MeshData>& meshData) const {
    for (const auto& mesh : meshData) {
        if (mesh.points.empty()) {
            return true;
        }
    }
    return meshData.empty();
}

bool UsdProcessor::resolveReferences(const tinyusdz::Stage& stage,
                                     const std::vector<uint8_t>& buffer,
                                     const std::string& fileName,
                                     std::vector<MeshData>& outMeshData,
                                     ProgressCallback progressCallback,
                                     const std::string* preExistingContent) {
    try {
        if (progressCallback) {
            progressCallback(0.0f, "Extracting reference paths");
        }

        // Extract reference paths
        std::vector<std::string> referencePaths;
        ExtractReferencePaths(stage, referencePaths);

        // Extract clips from raw content — use pre-existing string if available (avoids copy)
        std::vector<std::string> clipPaths;
        if (preExistingContent) {
            clipPaths = pImpl->extractClipsFromString(*preExistingContent);
        } else {
            clipPaths = ExtractClipsFromRawContent(buffer);
        }
        referencePaths.insert(referencePaths.end(), clipPaths.begin(), clipPaths.end());

        if (referencePaths.empty()) {
            MIDDLEWARE_LOG_INFO("No references or clips found to resolve");
            return true;
        }

        MIDDLEWARE_LOG_INFO("Found %zu reference/clip paths to process", referencePaths.size());

        // Get base directory using TinyUSDZ's function
        std::string baseDir = tinyusdz::io::GetBaseDir(fileName);
        MIDDLEWARE_LOG_INFO("Base directory: %s", baseDir.c_str());
        if (baseDir.empty()) {
            // Extract directory from the full file path
            std::filesystem::path filePath(fileName);
            baseDir = filePath.parent_path().string();
            MIDDLEWARE_LOG_INFO("Using parent directory as base: %s", baseDir.c_str());

            if (baseDir.empty()) {
                // If still empty, use current working directory
                baseDir = std::filesystem::current_path().string();
                MIDDLEWARE_LOG_INFO("Using current directory as base: %s", baseDir.c_str());
            }
        }

        // Process each reference
        size_t processedCount = 0;
        for (const auto& refPath : referencePaths) {
            if (shutdownRequested.load()) {
                break;
            }

            std::string fullPath = baseDir + "/" + refPath;
            if (std::filesystem::exists(fullPath)) {
                if (loadReferencedFile(fullPath, outMeshData)) {
                    processedCount++;
                }
            }

            if (progressCallback) {
                float progress = static_cast<float>(processedCount) / static_cast<float>(referencePaths.size());
                progressCallback(progress * 0.8f + 0.2f, "Processing references");
            }
        }

        MIDDLEWARE_LOG_INFO("Successfully processed %zu/%zu references",
                          processedCount, referencePaths.size());

        return processedCount > 0;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in resolveReferences: %s", e.what());
        return false;
    }
}

bool UsdProcessor::loadReferencedFile(const std::string& filePath, std::vector<MeshData>& outMeshData) {
    try {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());

        size_t initialMeshCount = outMeshData.size();

        tinyusdz::Stage refStage;
        std::string warnings, errors;
        tinyusdz::USDLoadOptions options;
        options.load_payloads = true;
        options.load_references = true;
        options.max_memory_limit_in_mb = static_cast<int>(memoryLimitMB.load());

        bool result = tinyusdz::LoadUSDFromMemory(
            buffer.data(), buffer.size(), filePath.c_str(),
            &refStage, &warnings, &errors, options);

        if (result) {
            glm::mat4 identity(1.0f);
            for (const auto& rootPrim : refStage.root_prims()) {
                ProcessPrim(const_cast<tinyusdz::Prim*>(&rootPrim),
                            outMeshData, nullptr, identity, 0);
            }

            size_t meshesAdded = outMeshData.size() - initialMeshCount;
            if (meshesAdded > 0) {
                MIDDLEWARE_LOG_INFO("Extracted %zu meshes from %s", meshesAdded, filePath.c_str());
                stats.referencesResolved.fetch_add(1);
                return true;
            }
        } else {
            MIDDLEWARE_LOG_WARNING("Failed to load referenced file: %s - %s",
                                 filePath.c_str(), errors.c_str());
        }

        return false;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception loading referenced file %s: %s", filePath.c_str(), e.what());
        return false;
    }
}

bool UsdProcessor::ExtractPointCloudData(tinyusdz::GeomPoints* geomPoints,
                                         PointCloudData& outData,
                                         const glm::mat4& worldTransform) {
    if (!geomPoints) {
        MIDDLEWARE_LOG_ERROR("ExtractPointCloudData: null GeomPoints pointer");
        return false;
    }

    try {
        auto startTime = std::chrono::high_resolution_clock::now();

        // ── Extract point positions via TypedAttribute<Animatable<std::vector<value::point3f>>> ──
        auto pointsAnim = geomPoints->points.get_value();
        if (pointsAnim && pointsAnim->is_timesamples()) {
            std::vector<tinyusdz::value::point3f> rawPoints;
            if (pointsAnim->get(0.0, &rawPoints) && !rawPoints.empty()) {
                outData.positions.resize(rawPoints.size());
                size_t i = 0;
                for (const auto& pt : rawPoints) {
                    glm::vec4 homogeneous = glm::vec4(static_cast<float>(pt.x),
                                                      static_cast<float>(pt.y),
                                                      static_cast<float>(pt.z), 1.0f);
                    glm::vec4 transformed = worldTransform * homogeneous;
                    outData.positions[i] = glm::vec3(transformed.x / transformed.w,
                                                     transformed.y / transformed.w,
                                                     transformed.z / transformed.w);
                    i++;
                }
            } else {
                MIDDLEWARE_LOG_WARNING("Empty point positions for %s", outData.elementName.c_str());
                return false;
            }
        } else if (pointsAnim && pointsAnim->has_value()) {
            std::vector<tinyusdz::value::point3f> rawPoints;
            pointsAnim->get_default(&rawPoints);
            outData.positions.resize(rawPoints.size());
            size_t i = 0;
            for (const auto& pt : rawPoints) {
                glm::vec4 homogeneous = glm::vec4(static_cast<float>(pt.x),
                                                  static_cast<float>(pt.y),
                                                  static_cast<float>(pt.z), 1.0f);
                glm::vec4 transformed = worldTransform * homogeneous;
                outData.positions[i] = glm::vec3(transformed.x / transformed.w,
                                                 transformed.y / transformed.w,
                                                 transformed.z / transformed.w);
                i++;
            }
        } else {
            MIDDLEWARE_LOG_WARNING("No point positions found for %s", outData.elementName.c_str());
            return false;
        }
        MIDDLEWARE_LOG_INFO("Extracted %zu point positions from %s",
            outData.positions.size(), outData.elementName.c_str());

        // ── Extract normals ──
        auto normalsAnim = geomPoints->normals.get_value();
        if (normalsAnim) {
            std::vector<tinyusdz::value::normal3f> normalData;
            if (normalsAnim->is_timesamples()) {
                normalsAnim->get(0.0, &normalData);
            } else if (normalsAnim->has_value()) {
                normalsAnim->get_default(&normalData);
            }
            if (!normalData.empty()) {
                outData.normals.resize(normalData.size());
                for (size_t i = 0; i < normalData.size(); i++) {
                    outData.normals[i] = glm::normalize(glm::vec3(
                        static_cast<float>(normalData[i].x),
                        static_cast<float>(normalData[i].y),
                        static_cast<float>(normalData[i].z)));
                }
                MIDDLEWARE_LOG_INFO("Extracted %zu normals from %s", outData.normals.size(), outData.elementName.c_str());
            }
        }

        // ── Extract widths ──
        auto widthsAnim = geomPoints->widths.get_value();
        if (widthsAnim) {
            std::vector<float> widthData;
            if (widthsAnim->is_timesamples()) {
                widthsAnim->get(0.0, &widthData);
            } else if (widthsAnim->has_value()) {
                widthsAnim->get_default(&widthData);
            }
            if (!widthData.empty()) {
                outData.widths = std::move(widthData);
                MIDDLEWARE_LOG_INFO("Extracted %zu point widths from %s", outData.widths.size(), outData.elementName.c_str());
            }
        }

        // ── Extract attribute0 via primvar (GeomPrimvar interface) ──
        outData.scalarAttributes.clear();
        const std::vector<std::string> attrNames = {
            "primvars:attribute0", "attribute0",
            "primvars:st", "st",
            "primvars:map1", "map1"
        };
        for (const auto& name : attrNames) {
            tinyusdz::GeomPrimvar primvar;
            std::string err;
            if (geomPoints->get_primvar(name, &primvar, &err)) {
                std::vector<tinyusdz::value::texcoord2f> uvData;
                if (primvar.get_value(&uvData)) {
                    outData.scalarAttributes.resize(uvData.size());
                    for (size_t i = 0; i < uvData.size(); i++) {
                        outData.scalarAttributes[i] = glm::vec2(uvData[i].s, uvData[i].t);
                    }
                    outData.uvSetNames.push_back(name);
                    MIDDLEWARE_LOG_INFO("Extracted %zu %s attribute values from %s",
                        outData.scalarAttributes.size(), name.c_str(), outData.elementName.c_str());
                    break;
                }
            }
        }

        // ── Extract direct colors via primvar (GeomPrimvar interface) ──
        tinyusdz::GeomPrimvar colorPrimvar;
        std::string colorErr;
        if (geomPoints->get_primvar("primvars:color", &colorPrimvar, &colorErr)) {
            std::vector<tinyusdz::value::color4f> rawColors;
            if (colorPrimvar.get_value(&rawColors) && !rawColors.empty() &&
                rawColors.size() == outData.positions.size()) {
                outData.vertex_colors.resize(rawColors.size());
                for (size_t i = 0; i < rawColors.size(); i++) {
                    outData.vertex_colors[i] = glm::vec4(
                        static_cast<float>(rawColors[i].r),
                        static_cast<float>(rawColors[i].g),
                        static_cast<float>(rawColors[i].b),
                        static_cast<float>(rawColors[i].a));
                }
                MIDDLEWARE_LOG_INFO("Extracted %zu direct colors from %s",
                    rawColors.size(), outData.elementName.c_str());
            }
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        MIDDLEWARE_LOG_DEBUG("ExtractPointCloudData for '%s' (%zu points) took %lld ms",
            outData.elementName.c_str(), outData.positions.size(), duration);

        return true;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in ExtractPointCloudData: %s", e.what());
        return false;
    }
}

bool UsdProcessor::BakeColorsFromGradient(PointCloudData& pointCloud,
                                          const uint8_t* gradientRGBA,
                                          int texWidth) {
    if (!gradientRGBA || texWidth <= 0 || pointCloud.scalarAttributes.empty()) {
        MIDDLEWARE_LOG_WARNING("BakeColorsFromGradient: invalid input (gradient=%p, width=%d, attrs=%zu)",
            gradientRGBA, texWidth, pointCloud.scalarAttributes.size());
        return false;
    }

    size_t pointCount = pointCloud.scalarAttributes.size();
    pointCloud.vertex_colors.resize(pointCount);

    // Parallel bake using TBB or std::async
    size_t batchSize = 100000; // Process in batches for millions of points
    for (size_t start = 0; start < pointCount; start += batchSize) {
        size_t end = std::min(start + batchSize, pointCount);
        for (size_t i = start; i < end; i++) {
            // attribute0.x is the scalar colormap value in [0, 1]
            float scalar = pointCloud.scalarAttributes[i].x;
            int idx = static_cast<int>(scalar * (texWidth - 1));
            idx = std::clamp(idx, 0, texWidth - 1);
            int px = idx * 4; // RGBA offset

            pointCloud.vertex_colors[i] = glm::vec4(
                gradientRGBA[px] / 255.0f,
                gradientRGBA[px + 1] / 255.0f,
                gradientRGBA[px + 2] / 255.0f,
                gradientRGBA[px + 3] / 255.0f
            );
        }
    }

    MIDDLEWARE_LOG_INFO("Baked %zu colors from %dx%d gradient texture (%d channels)",
        pointCount, texWidth, 1, 4);
    return true;
}

} // namespace anari_usd_middleware

