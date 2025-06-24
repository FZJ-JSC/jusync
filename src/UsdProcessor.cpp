#include "UsdProcessor.h"
#include "MiddlewareLogging.h"

// Standard library includes with enhanced safety
#include <algorithm>
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
    }

    ~UsdProcessorImpl() {
        MIDDLEWARE_LOG_INFO("UsdProcessorImpl destroyed");
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

        try {
            // Convert buffer to string with size validation
            std::string fileContent;
            fileContent.assign(reinterpret_cast<const char*>(buffer.data()), buffer.size());

            if (fileContent.find("int[] faceVertexIndices") != std::string::npos ||
                fileContent.find("point3f[] points") != std::string::npos ||
                fileContent.find("float3[] points") != std::string::npos) {

                MIDDLEWARE_LOG_INFO("Large geometry detected - preserving original USD data for Unreal RealtimeMesh");
                return buffer;  // Return original content without any modifications
                }

            // Apply regex replacements with exception handling
            try {
                // Fix common USD format issues
                fileContent = std::regex_replace(fileContent, std::regex("0: None"), "0: []");
                fileContent = std::regex_replace(fileContent, std::regex("asset:images/"), "@./images/");
                fileContent = std::regex_replace(fileContent, std::regex("texCoord2f"), "texCoord2f[]");

                MIDDLEWARE_LOG_DEBUG("Applied regex replacements successfully");

            } catch (const std::regex_error& e) {
                MIDDLEWARE_LOG_ERROR("Regex error during preprocessing: %s", e.what());
                return buffer; // Return original on regex failure
            }

            // Safe line processing with bounds checking
            std::vector<std::string> lines;
            std::istringstream iss(fileContent);
            std::string line;

            // Limit number of lines to prevent memory exhaustion
            const size_t MAX_LINES = 1000000;
            lines.reserve(std::min(MAX_LINES, static_cast<size_t>(fileContent.size() / 50))); // Estimate

            while (std::getline(iss, line) && lines.size() < MAX_LINES) {
                // Validate line length
                lines.push_back(std::move(line));
            }

            if (lines.size() >= MAX_LINES) {
                MIDDLEWARE_LOG_WARNING("File has too many lines, truncated at %zu", MAX_LINES);
            }

            // Safe line modification with bounds checking
            if (lines.size() > 33) {
                std::string& line34 = lines[33];
                MIDDLEWARE_LOG_DEBUG("Processing line 34: %s", line34.c_str());

                if ((line34.find("texture") != std::string::npos ||
                     line34.find("albedoTex") != std::string::npos) &&
                    line34.find("uniform") == std::string::npos) {

                    std::string newLine = "uniform token info:id = \"UsdPreviewSurface\";" + line34;
                    if (newLine.size() < 1000) { // Reasonable line length check
                        lines[33] = std::move(newLine);
                        MIDDLEWARE_LOG_DEBUG("Modified line 34 successfully");
                    } else {
                        MIDDLEWARE_LOG_WARNING("Modified line would be too long, skipping");
                    }
                }
            }

            // Rebuild content with size monitoring
            fileContent.clear();
            size_t estimatedSize = 0;
            const size_t MAX_GROWTH_FACTOR = 2;

            for (const auto& l : lines) {
                estimatedSize += l.size() + 1; // +1 for newline

                // Prevent excessive memory growth
                if (estimatedSize > buffer.size() * MAX_GROWTH_FACTOR) {
                    MIDDLEWARE_LOG_WARNING("Preprocessed content growing too large, truncating at %zu bytes",
                                         estimatedSize);
                    break;
                }

                fileContent += l + "\n";
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

private:
    std::chrono::steady_clock::time_point processingStartTime;
    std::atomic<size_t> memoryLimitBytes{1024 * 1024 * 1024}; // 1GB default
};

// Enhanced MeshData validation methods
std::pair<glm::vec3, glm::vec3> UsdProcessor::MeshData::getBounds() const {
    if (points.empty()) {
        return {glm::vec3(0.0f), glm::vec3(0.0f)};
    }

    glm::vec3 minBounds = points[0];
    glm::vec3 maxBounds = points[0];

    for (const auto& point : points) {
        // Validate finite values
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
    // Validate points
    if (points.empty()) {
        return false;
    }

    // Check for finite values in points
    for (const auto& point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
            return false;
        }
    }

    // Validate indices
    if (!indices.empty()) {
        if (indices.size() % 3 != 0) {
            return false; // Must be triangles
        }

        // Check index bounds
        for (uint32_t index : indices) {
            if (index >= points.size()) {
                return false;
            }
        }
    }

    // Validate normals if present
    if (!normals.empty()) {
        if (normals.size() != points.size()) {
            return false; // Must match vertex count
        }

        for (const auto& normal : normals) {
            if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z)) {
                return false;
            }
        }
    }

    // Validate UVs if present
    if (!uvs.empty()) {
        if (uvs.size() != points.size()) {
            return false; // Must match vertex count
        }

        for (const auto& uv : uvs) {
            if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
                return false;
            }
        }
    }

    return true;
}

// Constructor and destructor with enhanced safety
UsdProcessor::UsdProcessor() : pImpl(std::make_unique<UsdProcessorImpl>()) {
    MIDDLEWARE_LOG_INFO("UsdProcessor created with enhanced safety features");
    stats.reset();
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
        if (width <= 0 || height <= 0 || width > 32768 || height > 32768) {
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

// Enhanced USD buffer loading with comprehensive safety
bool UsdProcessor::LoadUSDBuffer(const std::vector<uint8_t>& buffer,
                                const std::string& fileName,
                                std::vector<MeshData>& outMeshData,
                                ProgressCallback progressCallback) {
    std::unique_lock<std::shared_mutex> lock(processingMutex);
    if (shutdownRequested.load()) {
        MIDDLEWARE_LOG_WARNING("USD loading aborted: shutdown requested");
        return false;
    }

    MIDDLEWARE_LOG_INFO("Loading USD from buffer, size: %zu, filename: %s", buffer.size(), fileName.c_str());

    // Clear output data first
    outMeshData.clear();

    // Validate inputs
    if (buffer.empty()) {
        MIDDLEWARE_LOG_ERROR("Cannot load USD from empty buffer");
        stats.processingErrors.fetch_add(1);
        return false;
    }

    if (buffer.size() > safety::MAX_BUFFER_SIZE) {
        MIDDLEWARE_LOG_ERROR("USD buffer too large: %zu bytes (max: %zu)",
                            buffer.size(), safety::MAX_BUFFER_SIZE);
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

        // CRITICAL FIX: Preserve full data for Unreal RealtimeMesh processing
        std::vector<uint8_t> processedBuffer;

        // Convert buffer to string for geometry detection
        std::string content(reinterpret_cast<const char*>(buffer.data()), buffer.size());

        // Check if this contains large geometry arrays
        if (content.find("int[] faceVertexIndices") != std::string::npos ||
            content.find("point3f[] points") != std::string::npos ||
            content.find("float3[] points") != std::string::npos) {

            MIDDLEWARE_LOG_INFO("Large geometry detected - preserving original USD data for Unreal RealtimeMesh");
            processedBuffer = buffer;  // Use original buffer without preprocessing
        } else {
            // Apply minimal preprocessing for non-geometry files
            processedBuffer = pImpl->preprocessUsdContent(buffer);
        }

        // LIMITED DEBUG: Only show first 200 characters for debugging
        if (processedBuffer.size() > 200) {
            std::string preview(reinterpret_cast<const char*>(processedBuffer.data()), 200);
            preview += "... [truncated for debug]";
            MIDDLEWARE_LOG_DEBUG("USD content preview: %s", preview.c_str());
        } else {
            std::string fullContent(reinterpret_cast<const char*>(processedBuffer.data()), processedBuffer.size());
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
            processedBuffer.data(),
            processedBuffer.size(),
            fileName.c_str(),
            &stage,
            &warnings,
            &errors,
            options
        );

        if (!loadResult) {
            MIDDLEWARE_LOG_ERROR("TinyUSDZ load error: %s", errors.c_str());
            stats.processingErrors.fetch_add(1);
            return false;
        }

        if (!warnings.empty()) {
            MIDDLEWARE_LOG_WARNING("TinyUSDZ load warnings: %s", warnings.c_str());
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

            if (!ProcessPrim(const_cast<tinyusdz::Prim*>(&rootPrim), outMeshData, identity, 0)) {
                MIDDLEWARE_LOG_WARNING("Failed to process root prim: %s", rootPrim.element_name().c_str());
            }
        }

        size_t meshesFromMain = outMeshData.size() - initialMeshCount;
        MIDDLEWARE_LOG_INFO("Extracted %zu meshes from main stage", meshesFromMain);

        if (progressCallback) {
            progressCallback(0.7f, "Resolving references");
        }

        // Enhanced reference resolution
        if (referenceResolutionEnabled.load() && (outMeshData.empty() || hasEmptyGeometry(outMeshData))) {
            MIDDLEWARE_LOG_INFO("Attempting reference resolution for missing geometry");

            if (!resolveReferences(stage, processedBuffer, fileName, outMeshData, progressCallback)) {
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
        stats.totalBytesProcessed.fetch_add(buffer.size());

        // LIMITED DEBUG: Only show mesh statistics for RealtimeMesh, not full data
        MIDDLEWARE_LOG_INFO("USD processing complete: %zu valid meshes extracted for RealtimeMesh", outMeshData.size());

        for (size_t i = 0; i < outMeshData.size(); ++i) {
            const auto& mesh = outMeshData[i];
            MIDDLEWARE_LOG_INFO("Mesh %zu '%s': %zu vertices, %zu triangles, %zu normals, %zu UVs",
                               i, mesh.elementName.c_str(),
                               mesh.points.size() / 3,
                               mesh.indices.size() / 3,
                               mesh.normals.size() / 3,
                               mesh.uvs.size() / 2);
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
        return LoadUSDBuffer(buffer, filePath, outMeshData, progressCallback);

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
    if (limitMB >= 1 && limitMB <= 4096) {
        memoryLimitMB.store(limitMB);
        MIDDLEWARE_LOG_INFO("Memory limit set to %zu MB", limitMB);
    } else {
        MIDDLEWARE_LOG_ERROR("Invalid memory limit: %zu MB (must be 1-4096)", limitMB);
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

    // Check for USD magic bytes or text patterns
    std::string content(reinterpret_cast<const char*>(buffer.data()),
                      std::min(buffer.size(), static_cast<size_t>(1000)));

    // Look for USD-specific patterns
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

        // Check if this is a mesh primitive
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

        // Process children recursively
        for (const auto& child : usdPrim.children()) {
            if (!ProcessPrim(const_cast<tinyusdz::Prim*>(&child),
                            meshDataArray, worldTransform, depth + 1)) {
                MIDDLEWARE_LOG_WARNING("Failed to process child prim: %s",
                                     child.element_name().c_str());
                // Continue processing other children
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

        MIDDLEWARE_LOG_DEBUG("Extracting mesh with %zu points", points.size());

        // Transform and validate points
        outMeshData.points.clear();
        outMeshData.points.reserve(points.size());

        for (const auto& pt : points) {
            // Validate input point
            if (!std::isfinite(pt.x) || !std::isfinite(pt.y) || !std::isfinite(pt.z)) {
                MIDDLEWARE_LOG_WARNING("Non-finite vertex detected, skipping");
                continue;
            }

            glm::vec3 vertex(static_cast<float>(pt.x),
                           static_cast<float>(pt.y),
                           static_cast<float>(pt.z));

            glm::vec4 transformedVertex = worldTransform * glm::vec4(vertex, 1.0f);

            // Validate transformed vertex
            if (!std::isfinite(transformedVertex.x) ||
                !std::isfinite(transformedVertex.y) ||
                !std::isfinite(transformedVertex.z)) {
                MIDDLEWARE_LOG_WARNING("Transform produced non-finite vertex, using original");
                outMeshData.points.push_back(vertex);
            } else {
                outMeshData.points.push_back(glm::vec3(transformedVertex));
            }
        }

        if (outMeshData.points.empty()) {
            MIDDLEWARE_LOG_ERROR("No valid points after transformation");
            return false;
        }

        // Extract and triangulate faces
        auto faceVertexCounts = geomMesh->get_faceVertexCounts();
        auto faceVertexIndices = geomMesh->get_faceVertexIndices();

        if (faceVertexCounts.empty() || faceVertexIndices.empty()) {
            MIDDLEWARE_LOG_WARNING("Mesh has no face data: %s", outMeshData.elementName.c_str());
            return false;
        }

        // Triangulate with validation
        std::vector<uint32_t> triangulatedIndices;
        size_t indexOffset = 0;

        for (size_t faceIdx = 0; faceIdx < faceVertexCounts.size(); faceIdx++) {
            int32_t numVertsInFace = faceVertexCounts[faceIdx];

            if (numVertsInFace < 3) {
                MIDDLEWARE_LOG_WARNING("Face %zu has less than 3 vertices, skipping", faceIdx);
                indexOffset += numVertsInFace;
                continue;
            }

            if (numVertsInFace > 100) { // Reasonable polygon limit
                MIDDLEWARE_LOG_WARNING("Face %zu has too many vertices (%d), skipping",
                                     faceIdx, numVertsInFace);
                indexOffset += numVertsInFace;
                continue;
            }

            // Check bounds for face indices
            if (indexOffset + numVertsInFace > faceVertexIndices.size()) {
                MIDDLEWARE_LOG_ERROR("Face vertex indices out of bounds");
                break;
            }

            // Triangulate this face
            for (int32_t triIdx = 0; triIdx < numVertsInFace - 2; triIdx++) {
                uint32_t idx0 = static_cast<uint32_t>(faceVertexIndices[indexOffset]);
                uint32_t idx1 = static_cast<uint32_t>(faceVertexIndices[indexOffset + triIdx + 1]);
                uint32_t idx2 = static_cast<uint32_t>(faceVertexIndices[indexOffset + triIdx + 2]);

                // Validate indices
                if (idx0 >= outMeshData.points.size() ||
                    idx1 >= outMeshData.points.size() ||
                    idx2 >= outMeshData.points.size()) {
                    MIDDLEWARE_LOG_WARNING("Invalid triangle indices, skipping triangle");
                    continue;
                }

                triangulatedIndices.push_back(idx0);
                triangulatedIndices.push_back(idx1);
                triangulatedIndices.push_back(idx2);
            }

            indexOffset += numVertsInFace;
        }

        if (triangulatedIndices.empty()) {
            MIDDLEWARE_LOG_WARNING("No valid triangles generated");
            return false;
        }

        if (triangulatedIndices.size() > safety::MAX_MESH_INDICES) {
            MIDDLEWARE_LOG_ERROR("Too many indices generated: %zu (max: %zu)",
                                triangulatedIndices.size(), safety::MAX_MESH_INDICES);
            return false;
        }

        outMeshData.indices = std::move(triangulatedIndices);

        // Extract normals with validation
        auto normals = geomMesh->get_normals();
        if (!normals.empty()) {
            if (normals.size() != points.size()) {
                MIDDLEWARE_LOG_WARNING("Normal count (%zu) doesn't match vertex count (%zu)",
                                     normals.size(), points.size());
            } else {
                outMeshData.normals.clear();
                outMeshData.normals.reserve(normals.size());

                glm::mat3 normalMatrix = glm::mat3(worldTransform);

                for (const auto& nrm : normals) {
                    if (!std::isfinite(nrm.x) || !std::isfinite(nrm.y) || !std::isfinite(nrm.z)) {
                        MIDDLEWARE_LOG_WARNING("Non-finite normal detected, using default");
                        outMeshData.normals.push_back(glm::vec3(0.0f, 1.0f, 0.0f));
                        continue;
                    }

                    glm::vec3 normalVec(static_cast<float>(nrm.x),
                                      static_cast<float>(nrm.y),
                                      static_cast<float>(nrm.z));

                    glm::vec3 transformedNormal = normalMatrix * normalVec;

                    // Validate and normalize
                    float length = glm::length(transformedNormal);
                    if (length > safety::EPSILON) {
                        transformedNormal = transformedNormal / length;
                    } else {
                        transformedNormal = glm::vec3(0.0f, 1.0f, 0.0f); // Default up vector
                    }

                    outMeshData.normals.push_back(transformedNormal);
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

        MIDDLEWARE_LOG_DEBUG("Successfully extracted mesh: %zu vertices, %zu triangles, %zu normals, %zu UVs",
                           outMeshData.points.size(),
                           outMeshData.indices.size() / 3,
                           outMeshData.normals.size(),
                           outMeshData.uvs.size());

        return true;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in ExtractMeshData: %s", e.what());
        stats.processingErrors.fetch_add(1);
        return false;
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
    std::vector<std::string> clipPaths;

    // Convert buffer to string for parsing
    std::string content(reinterpret_cast<const char*>(buffer.data()), buffer.size());

    // Look for clips patterns in the raw USD content
    std::regex clipsPattern(R"(asset\[\]\s+assetPaths\s*=\s*\[@([^@]+)@\])");

    std::sregex_iterator iter(content.begin(), content.end(), clipsPattern);
    std::sregex_iterator end;

    while (iter != end) {
        std::string clipPath = (*iter)[1].str();
        MIDDLEWARE_LOG_INFO("Found clip asset path: %s", clipPath.c_str());
        clipPaths.push_back(clipPath);
        ++iter;
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
        tinyusdz::GeomPrimvar primvar;
        std::string primvarErr;
        bool foundUVs = false;

        // Try different UV attribute names
        const std::vector<std::string> uvNames = {
            "primvars:st", "st", "primvars:uv", "uv",
            "primvars:attribute0", "attribute0"
        };

        for (const auto& name : uvNames) {
            if (mesh->get_primvar(name, &primvar, &primvarErr)) {
                std::vector<tinyusdz::value::texcoord2f> uvs;
                if (primvar.get_value(&uvs)) {
                    MIDDLEWARE_LOG_DEBUG("Found %zu UV coordinates in primvar: %s",
                                       uvs.size(), name.c_str());

                    meshData.uvs.clear();
                    meshData.uvs.reserve(uvs.size());

                    for (const auto& uv : uvs) {
                        meshData.uvs.push_back(glm::vec2(uv.s, uv.t));
                    }

                    // Normalize and validate UVs
                    normalizeUVCoordinates(meshData.uvs);
                    foundUVs = true;
                    break;
                }
            }
        }

        if (!foundUVs) {
            MIDDLEWARE_LOG_DEBUG("No UV coordinates found for mesh: %s", meshData.elementName.c_str());
        }

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception extracting UV coordinates: %s", e.what());
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
                                    ProgressCallback progressCallback) {
    try {
        if (progressCallback) {
            progressCallback(0.0f, "Extracting reference paths");
        }

        // Extract reference paths
        std::vector<std::string> referencePaths;
        ExtractReferencePaths(stage, referencePaths);

        // Extract clips from raw content
        std::vector<std::string> clipPaths = ExtractClipsFromRawContent(buffer);
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
                           outMeshData, identity, 0);
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

} // namespace anari_usd_middleware

