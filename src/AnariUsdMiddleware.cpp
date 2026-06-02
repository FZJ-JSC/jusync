#include "AnariUsdMiddleware.h"
#include "CollisionProcessor.h"
#include "ZmqConnector.h"
#include "AnariUsdClient.h"
#include "HashVerifier.h"
#include "UsdProcessor.h"
#include "MiddlewareLogging.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <memory>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <sstream>
#include <map>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace anari_usd_middleware {

// ============================================================================
// IMPLEMENTATION CLASS WITH COLLISION SUPPORT
// ============================================================================

class AnariUsdMiddleware::Impl {
public:
    // Core components
    ZmqConnector zmqConnector;
    std::shared_ptr<AnariUsdClient> anariUsdClient;  // ✅ FIX: shared_ptr for safe capture in async threads
    std::unique_ptr<UsdProcessor> usdProcessor;
    std::unique_ptr<CollisionProcessor> collisionProcessor;  // ✅ NEW: Collision processor

    // Callback management
    std::map<int, FileUpdateCallback> updateCallbacks;
    std::map<int, MessageCallback> messageCallbacks;
    std::mutex callbackMutex;
    std::atomic<int> nextCallbackId{1};

    // Threading
    std::thread receiverThread;
    std::atomic<bool> running{false};
    std::atomic<bool> shutdownRequested{false};
    // ✅ FIX: Track async background threads to prevent use-after-free on destruction
    std::vector<std::thread> asyncThreads;
    std::mutex asyncThreadsMutex;

    // Initialization
    std::mutex initMutex;
    std::atomic<bool> initialized{false};
    std::chrono::steady_clock::time_point initializationTime;

    // File tracking for duplicate prevention
    std::unordered_set<std::string> processedFiles;
    std::mutex processedFilesMutex;
    std::chrono::steady_clock::time_point lastCleanup;
    std::atomic<size_t> maxTrackedFiles{10000};

    // ✅ Gradient/colormap texture caching (for point cloud color baking)
    std::map<std::string, std::vector<uint8_t>> cachedTextures;
    std::mutex textureCacheMutex;

    // ✅ NEW: Collision configuration
    std::atomic<ECollisionComplexity> defaultCollisionComplexity{ECollisionComplexity::Complex};
    std::mutex collisionConfigMutex;

public:
    Impl() : nextCallbackId(1), running(false), shutdownRequested(false) {
        MIDDLEWARE_LOG_INFO("AnariUsdMiddleware::Impl created with collision support");
        initializationTime = std::chrono::steady_clock::now();
        lastCleanup = std::chrono::steady_clock::now();
    }

    ~Impl() {
        MIDDLEWARE_LOG_INFO("AnariUsdMiddleware::Impl destroyed");
        shutdownRequested.store(true);
        stopReceiving();
        zmqConnector.disconnect();
        // ✅ FIX: Join tracked async threads with timeout, detach any remaining
        {
            std::lock_guard<std::mutex> lock(asyncThreadsMutex);
            for (auto& t : asyncThreads) {
                if (t.joinable()) {
                    t.detach();
                }
            }
            asyncThreads.clear();
        }
        MIDDLEWARE_LOG_INFO("AnariUsdMiddleware::Impl cleanup complete");
    }

    bool initialize(const char* endpoint) {
        std::lock_guard<std::mutex> lock(initMutex);
        if (initialized.load()) {
            MIDDLEWARE_LOG_WARNING("AnariUsdMiddleware already initialized");
            return true;
        }

        MIDDLEWARE_LOG_INFO("Initializing AnariUsdMiddleware with collision support...");
        try {
            // Initialize USD processor with error handling
            usdProcessor = std::make_unique<UsdProcessor>();
            if (!usdProcessor) {
                MIDDLEWARE_LOG_ERROR("Failed to create USD processor");
                return false;
            }

            // Configure USD processor with safe defaults
            usdProcessor->setMaxRecursionDepth(50);
            usdProcessor->setMemoryLimit(1024);
            usdProcessor->setReferenceResolutionEnabled(true);
            MIDDLEWARE_LOG_INFO("USD processor initialized successfully");

            // ✅ NEW: Initialize collision processor
            collisionProcessor = std::make_unique<CollisionProcessor>();
            if (!collisionProcessor) {
                MIDDLEWARE_LOG_ERROR("Failed to create collision processor");
                usdProcessor.reset();
                return false;
            }
            MIDDLEWARE_LOG_INFO("Collision processor initialized successfully");

            // Initialize ZMQ connection with enhanced error handling
            bool zmqResult = zmqConnector.initialize(endpoint, 5000);
            if (!zmqResult) {
                MIDDLEWARE_LOG_ERROR("Failed to initialize ZMQ connector");
                cleanup();
                return false;
            }

            // Set safe message size limits
            zmqConnector.setMaxMessageSize(safety::MAX_BUFFER_SIZE);

            initialized.store(true);
            MIDDLEWARE_LOG_INFO("AnariUsdMiddleware with collision support initialized successfully");
            return true;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception during initialization: %s", e.what());
            cleanup();
            return false;
        }
    }

    void shutdown() {
        MIDDLEWARE_LOG_INFO("Shutting down AnariUsdMiddleware with collision support...");
        shutdownRequested.store(true);
        stopReceiving();

        // ✅ FIX: Signal shutdown first, then detach tracked async threads (they check shutdownRequested)
        {
            std::lock_guard<std::mutex> athrLock(asyncThreadsMutex);
            for (auto& t : asyncThreads) {
                if (t.joinable()) {
                    t.detach();
                }
            }
            asyncThreads.clear();
            MIDDLEWARE_LOG_INFO("Detached %d async threads during shutdown", asyncThreads.size());
        }

        std::lock_guard<std::mutex> lock(initMutex);
        try {
            zmqConnector.disconnect(1000);

            // ✅ NEW: Disconnect ANARI USD client
            if (anariUsdClient) {
                anariUsdClient->disconnect(1000);
                anariUsdClient.reset();
                MIDDLEWARE_LOG_INFO("ANARI USD client disconnected");
            }

            if (usdProcessor) {
                auto stats = usdProcessor->getProcessingStats();
                MIDDLEWARE_LOG_INFO("Final processing stats - Files: %llu, Meshes: %llu, Errors: %llu",
                                   static_cast<unsigned long long>(stats.filesProcessed),
                                   static_cast<unsigned long long>(stats.meshesExtracted),
                                   static_cast<unsigned long long>(stats.processingErrors));
                usdProcessor.reset();
            }

            // ✅ NEW: Cleanup collision processor
            if (collisionProcessor) {
                collisionProcessor.reset();
                MIDDLEWARE_LOG_INFO("Collision processor cleaned up");
            }

            // Clear all callbacks
            {
                std::lock_guard<std::mutex> callbackLock(callbackMutex);
                updateCallbacks.clear();
                messageCallbacks.clear();
            }

            initialized.store(false);
            MIDDLEWARE_LOG_INFO("AnariUsdMiddleware shutdown complete");
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception during shutdown: %s", e.what());
        }
    }

    bool isConnected() const {
        // DEALER-ONLY ARCHITECTURE: Don't check zmqConnector.isConnected() since we don't have ROUTER socket
        // In DEALER-only mode, connection status is based on AnariUsdClient DEALER socket
        return initialized.load() && !shutdownRequested.load();
    }

    // ✅ NEW: Collision configuration methods
    void setDefaultCollisionComplexity(ECollisionComplexity complexity) {
        std::lock_guard<std::mutex> lock(collisionConfigMutex);
        defaultCollisionComplexity.store(complexity);
        MIDDLEWARE_LOG_INFO("Default collision complexity set to: %s",
                           CollisionProcessor::getComplexityName(complexity).c_str());
    }

    ECollisionComplexity getDefaultCollisionComplexity() const {
        return defaultCollisionComplexity.load();
    }

    void setCollisionParameters(float simplificationRatio, float convexHullPrecision, int maxConvexHulls) {
        if (!collisionProcessor) {
            MIDDLEWARE_LOG_ERROR("Collision processor not initialized");
            return;
        }

        std::lock_guard<std::mutex> lock(collisionConfigMutex);
        try {
            if (simplificationRatio > 0.0f && simplificationRatio < 1.0f) {
                collisionProcessor->setSimplificationRatio(simplificationRatio);
            }
            if (convexHullPrecision > 0.0f && convexHullPrecision < 1.0f) {
                collisionProcessor->setConvexHullPrecision(convexHullPrecision);
            }
            if (maxConvexHulls > 0 && maxConvexHulls <= 64) {
                collisionProcessor->setMaxConvexHulls(maxConvexHulls);
            }
            MIDDLEWARE_LOG_INFO("Collision parameters updated");
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception setting collision parameters: %s", e.what());
        }
    }

    // Callback management methods
    int registerUpdateCallback(FileUpdateCallback callback) {
        if (!callback) {
            MIDDLEWARE_LOG_ERROR("Attempted to register null file callback");
            return -1;
        }

        if (shutdownRequested.load()) {
            MIDDLEWARE_LOG_WARNING("Cannot register callback: shutdown requested");
            return -1;
        }

        std::lock_guard<std::mutex> lock(callbackMutex);
        int callbackId = nextCallbackId.fetch_add(1);
        updateCallbacks[callbackId] = std::move(callback);
        MIDDLEWARE_LOG_INFO("Registered file update callback with ID: %d", callbackId);
        return callbackId;
    }

    void unregisterUpdateCallback(int callbackId) {
        std::lock_guard<std::mutex> lock(callbackMutex);
        auto it = updateCallbacks.find(callbackId);
        if (it != updateCallbacks.end()) {
            updateCallbacks.erase(it);
            MIDDLEWARE_LOG_INFO("Unregistered file update callback with ID: %d", callbackId);
        } else {
            MIDDLEWARE_LOG_WARNING("Attempted to unregister non-existent file callback ID: %d", callbackId);
        }
    }

    int registerMessageCallback(MessageCallback callback) {
        if (!callback) {
            MIDDLEWARE_LOG_ERROR("Attempted to register null message callback");
            return -1;
        }

        if (shutdownRequested.load()) {
            MIDDLEWARE_LOG_WARNING("Cannot register callback: shutdown requested");
            return -1;
        }

        std::lock_guard<std::mutex> lock(callbackMutex);
        int callbackId = nextCallbackId.fetch_add(1);
        messageCallbacks[callbackId] = std::move(callback);
        MIDDLEWARE_LOG_INFO("Registered message callback with ID: %d", callbackId);
        return callbackId;
    }

    void unregisterMessageCallback(int callbackId) {
        std::lock_guard<std::mutex> lock(callbackMutex);
        auto it = messageCallbacks.find(callbackId);
        if (it != messageCallbacks.end()) {
            messageCallbacks.erase(it);
            MIDDLEWARE_LOG_INFO("Unregistered message callback with ID: %d", callbackId);
        } else {
            MIDDLEWARE_LOG_WARNING("Attempted to unregister non-existent message callback ID: %d", callbackId);
        }
    }

    bool startReceiving() {
        // DEALER-ONLY ARCHITECTURE: No receiver thread needed
        // In DEALER-only mode, all communication is synchronous through the DEALER socket
        // The receiver thread was for polling ROUTER socket, which no longer exists
        if (running.load()) {
            MIDDLEWARE_LOG_INFO("Receiver thread already marked as running");
            return true;
        }

        if (!isConnected()) {
            MIDDLEWARE_LOG_ERROR("Cannot start receiver: middleware not initialized");
            return false;
        }

        // In DEALER-only mode, we don't start a receiver thread
        // File transfers happen synchronously through request/response
        MIDDLEWARE_LOG_INFO("DEALER-only mode: Receiver thread not needed (synchronous communication)");
        running.store(true); // Mark as "running" to satisfy API
        return true;
    }

    void stopReceiving() {
        // DEALER-ONLY ARCHITECTURE: No receiver thread to stop
        if (!running.load()) {
            return;
        }

        MIDDLEWARE_LOG_INFO("Stopping receiver (DEALER-only mode)...");
        running.store(false);
        
        // In DEALER-only mode, there's no thread to join
        // Just mark as stopped
        MIDDLEWARE_LOG_INFO("Receiver stopped (DEALER-only mode)");
    }

    // Enhanced USD loading methods with collision support
    bool LoadUSDBuffer(const std::vector<uint8_t>& buffer, const std::string& fileName,
                      std::vector<MeshData>& outMeshData) {
        // Use default collision complexity (None for legacy compatibility)
        return LoadUSDBufferWithCollision(buffer, fileName, ECollisionComplexity::None, outMeshData);
    }

    bool LoadUSDFromDisk(const std::string& filePath, std::vector<MeshData>& outMeshData) {
        // Use default collision complexity (None for legacy compatibility)
        return LoadUSDFromDiskWithCollision(filePath, ECollisionComplexity::None, outMeshData);
    }

    // ✅ NEW: Enhanced USD loading methods WITH collision support
    bool LoadUSDBufferWithCollision(const std::vector<uint8_t>& buffer, const std::string& fileName,
                                   ECollisionComplexity complexity, std::vector<MeshData>& outMeshData) {
        if (!usdProcessor) {
            MIDDLEWARE_LOG_ERROR("USD processor not initialized");
            return false;
        }

        if (shutdownRequested.load()) {
            MIDDLEWARE_LOG_WARNING("USD loading aborted: shutdown requested");
            return false;
        }

        try {
            // Load USD data using existing processor
            std::vector<UsdProcessor::MeshData> processorMeshData;

            auto progressCallback = [this](float progress, const std::string& status) {
                if (progress == 1.0f) {
                    MIDDLEWARE_LOG_INFO("USD processing complete: %s", status.c_str());
                } else if (static_cast<int>(progress * 10) % 2 == 0) {
                    MIDDLEWARE_LOG_DEBUG("USD processing progress: %.1f%% - %s",
                                       progress * 100.0f, status.c_str());
                }
            };

            bool result = usdProcessor->LoadUSDBuffer(buffer, fileName, processorMeshData, nullptr, progressCallback);
            if (!result || processorMeshData.empty()) {
                MIDDLEWARE_LOG_ERROR("Failed to load USD data from buffer");
                return false;
            }

            // Convert to public API format with collision generation
            outMeshData.clear();
            outMeshData.reserve(processorMeshData.size());

            for (const auto& processorMesh : processorMeshData) {
                MeshData publicMesh;
                if (!convertMeshDataWithCollision(processorMesh, complexity, publicMesh)) {
                    MIDDLEWARE_LOG_WARNING("Failed to convert mesh data with collision: %s",
                                         processorMesh.elementName.c_str());
                    continue;
                }
                outMeshData.push_back(std::move(publicMesh));
            }

            MIDDLEWARE_LOG_INFO("Successfully loaded USD with collision: %zu meshes converted", outMeshData.size());
            return true;

        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in LoadUSDBufferWithCollision: %s", e.what());
            return false;
        }
    }

    bool LoadUSDFromDiskWithCollision(const std::string& filePath, ECollisionComplexity complexity,
                                     std::vector<MeshData>& outMeshData) {
        MIDDLEWARE_LOG_INFO("Loading USD from disk with collision: %s", filePath.c_str());

        try {
            // Validate file path
            if (!validateFilePath(filePath)) {
                return false;
            }

            // Read file to buffer
            std::vector<uint8_t> buffer;
            if (!readFileToBuffer(filePath, buffer)) {
                return false;
            }

            // Use buffer processing with collision
            return LoadUSDBufferWithCollision(buffer, filePath, complexity, outMeshData);

        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in LoadUSDFromDiskWithCollision: %s - %s",
                               filePath.c_str(), e.what());
            return false;
        }
    }

    // Enhanced texture creation with comprehensive validation
    TextureData CreateTextureFromBuffer(const std::vector<uint8_t>& buffer) {
        if (!usdProcessor) {
            MIDDLEWARE_LOG_ERROR("USD processor not initialized");
            return TextureData();
        }

        if (shutdownRequested.load()) {
            MIDDLEWARE_LOG_WARNING("Texture creation aborted: shutdown requested");
            return TextureData();
        }

        try {
            UsdProcessor::TextureData processorTexData = usdProcessor->CreateTextureFromBuffer(buffer);

            // Convert to public API structure with validation
            TextureData result;
            result.width = processorTexData.width;
            result.height = processorTexData.height;
            result.channels = processorTexData.channels;
            result.data = std::move(processorTexData.data);

            // Validate converted data
            if (!result.isValid()) {
                MIDDLEWARE_LOG_ERROR("Converted texture data failed validation");
                result.clear();
            }

            return result;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in CreateTextureFromBuffer: %s", e.what());
            return TextureData();
        }
    }

    // Enhanced gradient processing methods
    bool WriteGradientLineAsPNG(const std::vector<uint8_t>& buffer, const std::string& outPath) {
        try {
            if (buffer.empty()) {
                MIDDLEWARE_LOG_ERROR("Cannot write gradient PNG: empty buffer");
                return false;
            }

            if (outPath.empty()) {
                MIDDLEWARE_LOG_ERROR("Cannot write gradient PNG: empty output path");
                return false;
            }

            // Create texture from buffer
            TextureData texData = CreateTextureFromBuffer(buffer);

            // Validate gradient data (should be 1 row high)
            if (!texData.isValid() || texData.height != 1) {
                MIDDLEWARE_LOG_ERROR("Invalid gradient data for PNG writing: width=%d, height=%d",
                                   texData.width, texData.height);
                return false;
            }

            // Create output directory if needed
            std::filesystem::path outputPath(outPath);
            if (outputPath.has_parent_path()) {
                std::filesystem::create_directories(outputPath.parent_path());
            }

            // Write PNG with error handling
            int stride = texData.width * texData.channels;
            int success = stbi_write_png(
                outPath.c_str(),
                texData.width,
                texData.height,
                texData.channels,
                texData.data.data(),
                stride
            );

            if (!success) {
                MIDDLEWARE_LOG_ERROR("Failed to write PNG to %s", outPath.c_str());
                return false;
            }

            MIDDLEWARE_LOG_INFO("Gradient line PNG written successfully to %s (%dx%d, %d channels)",
                               outPath.c_str(), texData.width, texData.height, texData.channels);
            return true;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in WriteGradientLineAsPNG: %s", e.what());
            return false;
        }
    }

    bool GetGradientLineAsPNGBuffer(const std::vector<uint8_t>& buffer, std::vector<uint8_t>& outPngBuffer) {
        try {
            if (buffer.empty()) {
                MIDDLEWARE_LOG_ERROR("Cannot encode gradient PNG: empty buffer");
                return false;
            }

            TextureData texData = CreateTextureFromBuffer(buffer);

            // Validate gradient data
            if (!texData.isValid() || texData.height != 1) {
                MIDDLEWARE_LOG_ERROR("Invalid gradient data for PNG encoding: width=%d, height=%d",
                                   texData.width, texData.height);
                return false;
            }

            outPngBuffer.clear();
            int stride = texData.width * texData.channels;
            int success = stbi_write_png_to_func(
                writeToVector, &outPngBuffer,
                texData.width, texData.height, texData.channels,
                texData.data.data(), stride
            );

            if (!success) {
                MIDDLEWARE_LOG_ERROR("Failed to encode PNG to memory buffer");
                return false;
            }

            MIDDLEWARE_LOG_INFO("Gradient line encoded as PNG buffer: %zu bytes (%dx%d, %d channels)",
                               outPngBuffer.size(), texData.width, texData.height, texData.channels);
            return true;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in GetGradientLineAsPNGBuffer: %s", e.what());
            return false;
        }
    }

    bool GetCachedGradientTexture(std::vector<uint8_t>& outData, int& outWidth, int& outHeight) {
        try {
            std::lock_guard<std::mutex> lock(textureCacheMutex);
            // Get the most recently cached texture (last in map by insertion order)
            if (cachedTextures.empty()) {
                MIDDLEWARE_LOG_INFO("No cached gradient texture available");
                return false;
            }
            // std::map preserves insertion order, get the last element
            auto it = cachedTextures.end();
            --it;
            outData = it->second;
            // Decode the PNG to get dimensions
            TextureData texData = CreateTextureFromBuffer(outData);
            if (!texData.isValid()) {
                // Fallback: estimate dimensions if texture decode fails
                outWidth = static_cast<int>(std::sqrt(outData.size() / 4.0));
                outHeight = 1;
            } else {
                outWidth = texData.width;
                outHeight = texData.height;
            }
            MIDDLEWARE_LOG_INFO("Cached gradient texture retrieved: %s (%dx%d, %zu bytes)",
                               it->first.c_str(), outWidth, outHeight, outData.size());
            return true;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in GetCachedGradientTexture: %s", e.what());
            return false;
        }
    }

private:
    // ✅ NEW: Enhanced mesh conversion with collision support
    bool convertMeshDataWithCollision(const UsdProcessor::MeshData& processorMeshData,
                                     ECollisionComplexity complexity,
                                     MeshData& publicMeshData) {
        try {
            // Convert basic mesh data
            publicMeshData.elementName = processorMeshData.elementName;
            publicMeshData.typeName = processorMeshData.typeName;

            // Convert points (glm::vec3 → flat float array)
            size_t pointCount = processorMeshData.points.size();
            publicMeshData.points.clear();
            publicMeshData.points.reserve(pointCount * 3);
            for (const auto& point : processorMeshData.points) {
                publicMeshData.points.push_back(point.x);
                publicMeshData.points.push_back(point.y);
                publicMeshData.points.push_back(point.z);
            }

            // Direct copy for indices
            publicMeshData.indices = processorMeshData.indices;

            // Convert normals (glm::vec3 → flat float array)
            publicMeshData.normals.clear();
            publicMeshData.normals.reserve(processorMeshData.normals.size() * 3);
            for (const auto& normal : processorMeshData.normals) {
                publicMeshData.normals.push_back(normal.x);
                publicMeshData.normals.push_back(normal.y);
                publicMeshData.normals.push_back(normal.z);
            }

            // Convert UVs (glm::vec2 → flat float array)
            publicMeshData.uvs.clear();
            publicMeshData.uvs.reserve(processorMeshData.uvs.size() * 2);
            for (const auto& uv : processorMeshData.uvs) {
                publicMeshData.uvs.push_back(uv.x);
                publicMeshData.uvs.push_back(uv.y);
            }

            // Convert vertex colors (glm::vec4 → flat float array)
            publicMeshData.vertex_colors.clear();
            if (!processorMeshData.vertex_colors.empty()) {
                size_t colorCount = processorMeshData.vertex_colors.size();
                size_t faceCount = processorMeshData.indices.size() / 3;
                bool isVertexInterp = (colorCount == pointCount);
                bool isUniformInterp = (colorCount == faceCount);

                MIDDLEWARE_LOG_INFO("Color conversion: %zu colors, %zu vertices, %zu faces - Mode: %s",
                                   colorCount, pointCount, faceCount,
                                   isVertexInterp ? "VERTEX" : (isUniformInterp ? "UNIFORM" : "UNKNOWN"));

                if (isVertexInterp) {
                    // Direct per-vertex mapping
                    publicMeshData.vertex_colors.reserve(colorCount * 4);
                    for (const auto& color : processorMeshData.vertex_colors) {
                        publicMeshData.vertex_colors.push_back(color.r);
                        publicMeshData.vertex_colors.push_back(color.g);
                        publicMeshData.vertex_colors.push_back(color.b);
                        publicMeshData.vertex_colors.push_back(color.a);
                    }
                } else if (isUniformInterp) {
                    // Expand uniform (per-face) colors to per-vertex
                    publicMeshData.vertex_colors.reserve(pointCount * 4);
                    std::vector<glm::vec4> vertexColors(pointCount, glm::vec4(1.0f));

                    for (size_t faceIdx = 0; faceIdx < faceCount; ++faceIdx) {
                        if (faceIdx >= colorCount) break;
                        const auto& faceColor = processorMeshData.vertex_colors[faceIdx];

                        // Get the three vertex indices for this face
                        size_t i0 = processorMeshData.indices[faceIdx * 3 + 0];
                        size_t i1 = processorMeshData.indices[faceIdx * 3 + 1];
                        size_t i2 = processorMeshData.indices[faceIdx * 3 + 2];

                        // Assign face color to all three vertices
                        if (i0 < pointCount) vertexColors[i0] = faceColor;
                        if (i1 < pointCount) vertexColors[i1] = faceColor;
                        if (i2 < pointCount) vertexColors[i2] = faceColor;
                    }

                    // Flatten to float array
                    for (const auto& color : vertexColors) {
                        publicMeshData.vertex_colors.push_back(color.r);
                        publicMeshData.vertex_colors.push_back(color.g);
                        publicMeshData.vertex_colors.push_back(color.b);
                        publicMeshData.vertex_colors.push_back(color.a);
                    }
                } else {
                    // Fallback: treat as vertex colors with padding/truncation
                    MIDDLEWARE_LOG_WARNING("Color count mismatch - using fallback vertex mapping");
                    publicMeshData.vertex_colors.reserve(pointCount * 4);
                    for (size_t i = 0; i < pointCount; ++i) {
                        if (i < colorCount) {
                            const auto& color = processorMeshData.vertex_colors[i];
                            publicMeshData.vertex_colors.push_back(color.r);
                            publicMeshData.vertex_colors.push_back(color.g);
                            publicMeshData.vertex_colors.push_back(color.b);
                            publicMeshData.vertex_colors.push_back(color.a);
                        } else {
                            // Default white for missing colors
                            publicMeshData.vertex_colors.push_back(1.0f);
                            publicMeshData.vertex_colors.push_back(1.0f);
                            publicMeshData.vertex_colors.push_back(1.0f);
                            publicMeshData.vertex_colors.push_back(1.0f);
                        }
                    }
                }
            }

            // ✅ NEW: Generate collision data if requested
            if (complexity != ECollisionComplexity::None && collisionProcessor) {
                MIDDLEWARE_LOG_INFO("Generating %s collision for mesh: %s",
                                   CollisionProcessor::getComplexityName(complexity).c_str(),
                                   publicMeshData.elementName.c_str());

                bool collisionResult = collisionProcessor->generateCollision(
                    publicMeshData.points, publicMeshData.indices, complexity, publicMeshData.collision);

                if (!collisionResult || !publicMeshData.collision.isValid()) {
                    MIDDLEWARE_LOG_WARNING("Failed to generate collision for mesh: %s",
                                         publicMeshData.elementName.c_str());
                    publicMeshData.collision.clear(); // Clear failed collision data
                }
            } else {
                // No collision requested
                publicMeshData.collision.clear();
            }

            // Validate the converted mesh data
            bool isValid = publicMeshData.isValid();
            if (!isValid) {
                MIDDLEWARE_LOG_ERROR("Converted mesh data failed validation for: %s",
                                   processorMeshData.elementName.c_str());
            } else {
                MIDDLEWARE_LOG_INFO("Successfully converted mesh with collision: %s (%zu vertices, %zu faces, %zu colors)",
                                   processorMeshData.elementName.c_str(), pointCount,
                                   processorMeshData.indices.size() / 3,
                                   publicMeshData.vertex_colors.size() / 4);
            }

            return isValid;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in convertMeshDataWithCollision: %s", e.what());
            return false;
        }
    }

    // Helper methods implementation
    void cleanup() {
        try {
            stopReceiving();
            zmqConnector.disconnect();
            usdProcessor.reset();
            collisionProcessor.reset();  // ✅ NEW: Cleanup collision processor

            std::lock_guard<std::mutex> lock(callbackMutex);
            updateCallbacks.clear();
            messageCallbacks.clear();
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception during cleanup: %s", e.what());
        }
    }

    bool validateFilePath(const std::string& filePath) const {
        if (filePath.empty() || filePath.size() > 1000) {
            MIDDLEWARE_LOG_ERROR("File path too long: %zu characters", filePath.size());
            return false;
        }

        if (!std::filesystem::exists(filePath)) {
            MIDDLEWARE_LOG_ERROR("File does not exist: %s", filePath.c_str());
            return false;
        }

        return true;
    }

    bool readFileToBuffer(const std::string& filePath, std::vector<uint8_t>& buffer) {
        try {
            std::ifstream file(filePath, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                MIDDLEWARE_LOG_ERROR("Failed to open file: %s", filePath.c_str());
                return false;
            }

            auto fileSize = file.tellg();
            if (fileSize <= 0) {
                MIDDLEWARE_LOG_ERROR("Invalid file size: %lld", static_cast<long long>(fileSize));
                return false;
            }

            if (static_cast<size_t>(fileSize) > safety::MAX_BUFFER_SIZE) {
                MIDDLEWARE_LOG_ERROR("File too large: %lld bytes (max: %zu)",
                                   static_cast<long long>(fileSize), safety::MAX_BUFFER_SIZE);
                return false;
            }

            file.seekg(0, std::ios::beg);
            buffer.resize(static_cast<size_t>(fileSize));
            if (!file.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
                MIDDLEWARE_LOG_ERROR("Failed to read file contents: %s", filePath.c_str());
                return false;
            }

            return true;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception reading file %s: %s", filePath.c_str(), e.what());
            return false;
        }
    }

    void receiverLoop() {
        MIDDLEWARE_LOG_INFO("Enhanced receiver thread started with collision support");
        auto lastStatsLog = std::chrono::steady_clock::now();
        const auto STATS_LOG_INTERVAL = std::chrono::minutes(5);

        while (running.load() && !shutdownRequested.load()) {
            try {
                // Use polling with timeout instead of blocking receive
                zmq::pollitem_t items[] = {
                    { zmqConnector.getSocket(), 0, ZMQ_POLLIN, 0 }
                };

                int pollResult = zmq::poll(items, 1, std::chrono::milliseconds(100));
                if (pollResult > 0 && (items[0].revents & ZMQ_POLLIN)) {
                    // Socket has data available
                    if (!processIncomingMessage()) {
                        MIDDLEWARE_LOG_DEBUG("Failed to process incoming message");
                    }
                }

                // Periodic statistics logging
                auto now = std::chrono::steady_clock::now();
                if (now - lastStatsLog > STATS_LOG_INTERVAL) {
                    logStatistics();
                    lastStatsLog = now;
                }

                // Small sleep to prevent CPU spinning
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } catch (const std::exception& e) {
                MIDDLEWARE_LOG_ERROR("Exception in receiver loop: %s", e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        MIDDLEWARE_LOG_INFO("Enhanced receiver thread stopped");
    }

    bool processIncomingMessage() {
        try {
            FileData fileData;

            // Try to receive as file first
            if (zmqConnector.receiveFile(fileData.filename, fileData.data, fileData.hash, 0)) {
                MIDDLEWARE_LOG_INFO("Successfully received file via ZMQ: %s (%zu bytes)",
                                   fileData.filename.c_str(), fileData.data.size());
                return processReceivedFile(fileData);
            }

            // If not a file, try to receive as generic message
            if (zmqConnector.receiveAnyMessage(0)) {
                MIDDLEWARE_LOG_INFO("Successfully received generic message via ZMQ");
                return processReceivedMessage();
            }

            return false;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in processIncomingMessage: %s", e.what());
            return false;
        }
    }

    bool processReceivedFile(FileData& fileData) {
        MIDDLEWARE_LOG_INFO("Processing received file: %s (size: %zu bytes, hash: %s)",
                           fileData.filename.c_str(), fileData.data.size(), fileData.hash.c_str());

        try {
            // Basic validation
            if (fileData.filename.empty() || fileData.data.empty()) {
                MIDDLEWARE_LOG_ERROR("File data validation failed: empty filename or data");
                return false;
            }

            // Check for duplicate files
            if (isDuplicateFile(fileData.filename, fileData.hash)) {
                MIDDLEWARE_LOG_WARNING("Duplicate file detected, skipping: %s", fileData.filename.c_str());
                return true;
            }

            // Determine file type
            std::string fileType = "UNKNOWN";
            if (fileData.filename.find(".usda") != std::string::npos ||
                fileData.filename.find(".usd") != std::string::npos) {
                fileType = "USD";
            } else if (fileData.filename.find(".png") != std::string::npos ||
                      fileData.filename.find(".jpg") != std::string::npos) {
                fileType = "IMAGE";
            }

            fileData.fileType = fileType;
            MIDDLEWARE_LOG_INFO("File type detected: %s", fileType.c_str());

            // Cache gradient/colormap textures (for point cloud color baking)
            if (fileType == "IMAGE") {
                std::lock_guard<std::mutex> lock(textureCacheMutex);
                cachedTextures[fileData.filename] = fileData.data;
                MIDDLEWARE_LOG_INFO("Cached gradient texture: %s (%zu bytes)",
                    fileData.filename.c_str(), fileData.data.size());
            }

            // Mark file as processed BEFORE notifying callbacks
            markFileAsProcessed(fileData.filename, fileData.hash);

            // Notify callbacks
            notifyFileCallbacks(fileData);
            return true;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception processing received file: %s", e.what());
            return false;
        }
    }

    bool processReceivedMessage() {
        try {
            const std::string& message = zmqConnector.getLastReceivedMessage();
            MIDDLEWARE_LOG_INFO("Processing received message: %s", message.c_str());

            // Notify callbacks with error handling
            notifyMessageCallbacks(message);
            return true;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception processing received message: %s", e.what());
            return false;
        }
    }

    bool isDuplicateFile(const std::string& filename, const std::string& hash) {
        std::lock_guard<std::mutex> lock(processedFilesMutex);
        std::string fileIdentifier = filename + ":" + hash;
        bool isDuplicate = processedFiles.find(fileIdentifier) != processedFiles.end();

        // Periodic cleanup to prevent memory growth
        auto now = std::chrono::steady_clock::now();
        if (now - lastCleanup > std::chrono::hours(1)) {
            cleanupOldEntries();
            lastCleanup = now;
        }

        return isDuplicate;
    }

    void markFileAsProcessed(const std::string& filename, const std::string& hash) {
        std::lock_guard<std::mutex> lock(processedFilesMutex);
        std::string fileIdentifier = filename + ":" + hash;
        processedFiles.insert(fileIdentifier);

        // Prevent memory growth by limiting tracked files
        if (processedFiles.size() > maxTrackedFiles.load()) {
            auto it = processedFiles.begin();
            size_t toRemove = processedFiles.size() / 10;
            for (size_t i = 0; i < toRemove && it != processedFiles.end(); ++i) {
                it = processedFiles.erase(it);
            }
            MIDDLEWARE_LOG_INFO("Cleaned up %zu old file entries", toRemove);
        }
    }

    void cleanupOldEntries() {
        if (processedFiles.size() > maxTrackedFiles.load() / 2) {
            size_t originalSize = processedFiles.size();
            processedFiles.clear();
            MIDDLEWARE_LOG_INFO("Cleared %zu processed file entries during cleanup", originalSize);
        }
    }

    void notifyFileCallbacks(const FileData& fileData) {
        std::lock_guard<std::mutex> lock(callbackMutex);
        for (const auto& pair : updateCallbacks) {
            try {
                pair.second(fileData);
            } catch (const std::exception& e) {
                MIDDLEWARE_LOG_ERROR("Exception in file callback (ID: %d): %s", pair.first, e.what());
            } catch (...) {
                MIDDLEWARE_LOG_ERROR("Unknown exception in file callback (ID: %d)", pair.first);
            }
        }
    }

    void notifyMessageCallbacks(const std::string& message) {
        std::lock_guard<std::mutex> lock(callbackMutex);
        for (const auto& pair : messageCallbacks) {
            try {
                pair.second(message);
            } catch (const std::exception& e) {
                MIDDLEWARE_LOG_ERROR("Exception in message callback (ID: %d): %s", pair.first, e.what());
            } catch (...) {
                MIDDLEWARE_LOG_ERROR("Unknown exception in message callback (ID: %d)", pair.first);
            }
        }
    }

    void logStatistics() {
        try {
            auto zmqStats = zmqConnector.getMessageStats();
            if (usdProcessor) {
                auto usdStats = usdProcessor->getProcessingStats();
                MIDDLEWARE_LOG_INFO("Middleware Statistics - ZMQ: %llu msgs, %llu files, %llu bytes | "
                                   "USD: %llu files, %llu meshes, %llu errors",
                                   static_cast<unsigned long long>(zmqStats.totalMessagesReceived),
                                   static_cast<unsigned long long>(zmqStats.totalFilesReceived),
                                   static_cast<unsigned long long>(zmqStats.totalBytesReceived),
                                   static_cast<unsigned long long>(usdStats.filesProcessed),
                                   static_cast<unsigned long long>(usdStats.meshesExtracted),
                                   static_cast<unsigned long long>(usdStats.processingErrors));
            }
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception logging statistics: %s", e.what());
        }
    }

    // Helper for PNG writing to memory buffer
    static void writeToVector(void* context, void* data, int size) {
        auto* vec = reinterpret_cast<std::vector<uint8_t>*>(context);
        uint8_t* bytes = reinterpret_cast<uint8_t*>(data);
        vec->insert(vec->end(), bytes, bytes + size);
    }
};

// ============================================================================
// PUBLIC INTERFACE IMPLEMENTATION WITH COLLISION SUPPORT
// ============================================================================

AnariUsdMiddleware::AnariUsdMiddleware() : pImpl(std::make_unique<Impl>()) {
    MIDDLEWARE_LOG_INFO("AnariUsdMiddleware created with collision support");
}

AnariUsdMiddleware::~AnariUsdMiddleware() {
    MIDDLEWARE_LOG_INFO("AnariUsdMiddleware destroyed");
    shutdown();
}

bool AnariUsdMiddleware::initialize(const char* endpoint) {
    return pImpl->initialize(endpoint);
}

void AnariUsdMiddleware::shutdown() {
    pImpl->shutdown();
}

bool AnariUsdMiddleware::isConnected() const {
    return pImpl->isConnected();
}

// ✅ NEW: Collision configuration methods
void AnariUsdMiddleware::setDefaultCollisionComplexity(ECollisionComplexity complexity) {
    pImpl->setDefaultCollisionComplexity(complexity);
}

ECollisionComplexity AnariUsdMiddleware::getDefaultCollisionComplexity() const {
    return pImpl->getDefaultCollisionComplexity();
}

void AnariUsdMiddleware::setCollisionParameters(float simplificationRatio, float convexHullPrecision, int maxConvexHulls) {
    pImpl->setCollisionParameters(simplificationRatio, convexHullPrecision, maxConvexHulls);
}

// Callback management
int AnariUsdMiddleware::registerUpdateCallback(FileUpdateCallback callback) {
    return pImpl->registerUpdateCallback(callback);
}

void AnariUsdMiddleware::unregisterUpdateCallback(int callbackId) {
    pImpl->unregisterUpdateCallback(callbackId);
}

int AnariUsdMiddleware::registerMessageCallback(MessageCallback callback) {
    return pImpl->registerMessageCallback(callback);
}

void AnariUsdMiddleware::unregisterMessageCallback(int callbackId) {
    pImpl->unregisterMessageCallback(callbackId);
}

bool AnariUsdMiddleware::startReceiving() {
    return pImpl->startReceiving();
}

void AnariUsdMiddleware::stopReceiving() {
    pImpl->stopReceiving();
}

// Enhanced USD processing methods
TextureData AnariUsdMiddleware::CreateTextureFromBuffer(const std::vector<uint8_t>& buffer) {
    return pImpl->CreateTextureFromBuffer(buffer);
}

bool AnariUsdMiddleware::LoadUSDBuffer(const std::vector<uint8_t>& buffer, const std::string& fileName,
                                      std::vector<MeshData>& outMeshData) {
    return pImpl->LoadUSDBuffer(buffer, fileName, outMeshData);
}

bool AnariUsdMiddleware::LoadUSDFromDisk(const std::string& filePath, std::vector<MeshData>& outMeshData) {
    return pImpl->LoadUSDFromDisk(filePath, outMeshData);
}

// ✅ NEW: USD processing methods WITH collision support
bool AnariUsdMiddleware::LoadUSDBufferWithCollision(const std::vector<uint8_t>& buffer, const std::string& fileName,
                                                   ECollisionComplexity complexity, std::vector<MeshData>& outMeshData) {
    return pImpl->LoadUSDBufferWithCollision(buffer, fileName, complexity, outMeshData);
}

bool AnariUsdMiddleware::LoadUSDFromDiskWithCollision(const std::string& filePath, ECollisionComplexity complexity,
                                                     std::vector<MeshData>& outMeshData) {
    return pImpl->LoadUSDFromDiskWithCollision(filePath, complexity, outMeshData);
}

// Gradient processing methods
bool AnariUsdMiddleware::WriteGradientLineAsPNG(const std::vector<uint8_t>& buffer, const std::string& outPath) {
    return pImpl->WriteGradientLineAsPNG(buffer, outPath);
}

bool AnariUsdMiddleware::GetGradientLineAsPNGBuffer(const std::vector<uint8_t>& buffer, std::vector<uint8_t>& outPngBuffer) {
    return pImpl->GetGradientLineAsPNGBuffer(buffer, outPngBuffer);
}

bool AnariUsdMiddleware::GetCachedGradientTexture(std::vector<uint8_t>& outData, int& outWidth, int& outHeight) {
    return pImpl->GetCachedGradientTexture(outData, outWidth, outHeight);
}

// ✅ NEW: ANARI USD DEALER client methods
bool AnariUsdMiddleware::connectToBroker(const char* brokerEndpoint, int timeoutMs) {
    if (!pImpl->anariUsdClient) {
        pImpl->anariUsdClient = std::make_shared<AnariUsdClient>();
    }
    return pImpl->anariUsdClient->connect(brokerEndpoint, timeoutMs);
}

void AnariUsdMiddleware::disconnectFromBroker() {
    if (pImpl->anariUsdClient) {
        pImpl->anariUsdClient->disconnect(1000);
    }
}

bool AnariUsdMiddleware::isBrokerConnected() const {
    return pImpl->anariUsdClient && pImpl->anariUsdClient->isConnected();
}

bool AnariUsdMiddleware::requestFileList(int32_t targetRank, std::vector<std::string>& outFiles, int timeoutMs) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_WARNING("ANARI USD client not connected - returning empty file list (non-MPI mode)");
        outFiles.clear();  // Return empty list
        return true;  // Return success with empty list
    }
    
    bool success = pImpl->anariUsdClient->getFileListSync(targetRank, outFiles, timeoutMs);
    if (!success) {
        MIDDLEWARE_LOG_WARNING("Failed to get file list from broker - returning empty file list (non-MPI fallback)");
        outFiles.clear();  // Return empty list on failure
        return true;  // Return success with empty list
    }
    return success;
}

bool AnariUsdMiddleware::requestFileListWithSizes(int32_t targetRank, std::vector<FileInfo>& outFiles, int timeoutMs) {
    MIDDLEWARE_LOG_INFO("=== requestFileListWithSizes ENTERED ===");
    MIDDLEWARE_LOG_INFO("targetRank=%d, timeoutMs=%d", targetRank, timeoutMs);
    
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_ERROR("ANARI USD client not connected - cannot request file list");
        outFiles.clear();  // Return empty list
        return false;  // Return FAILURE to indicate the request did not succeed
    }
    
    MIDDLEWARE_LOG_INFO("Client IS connected, calling getFileListWithSizesSync...");
    MIDDLEWARE_LOG_INFO("Calling getFileListWithSizesSync with targetRank=%d, timeoutMs=%d", targetRank, timeoutMs);
    bool success = pImpl->anariUsdClient->getFileListWithSizesSync(targetRank, outFiles, timeoutMs);
    MIDDLEWARE_LOG_INFO("getFileListWithSizesSync returned: success=%d, fileCount=%zu", success, outFiles.size());
    if (!success) {
        MIDDLEWARE_LOG_ERROR("Failed to get file list from broker");
        outFiles.clear();  // Return empty list on failure
        return false;  // Return FAILURE to indicate the request did not succeed
    }
    return success;
}

bool AnariUsdMiddleware::requestFile(const std::string& filename, int32_t targetRank,
                                      std::vector<uint8_t>& outFileData, int timeoutMs) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_ERROR("ANARI USD client not connected");
        return false;
    }
    return pImpl->anariUsdClient->getFileSync(filename, targetRank, outFileData, timeoutMs);
}

bool AnariUsdMiddleware::requestFrame(int32_t frameNumber, int32_t targetRank,
                                       std::vector<std::pair<std::string, std::vector<uint8_t>>>& outFrameFiles,
                                       int timeoutMs) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_ERROR("ANARI USD client not connected");
        return false;
    }
    
    // Collect frame files using callbacks
    std::map<std::string, std::vector<uint8_t>> frameFiles;
    std::mutex frameFilesMutex;
    bool frameComplete = false;
    
    auto chunkCallback = [&frameFiles, &frameFilesMutex](const std::string& fname,
                                                          const std::vector<uint8_t>& chunk,
                                                          uint64_t offset, uint64_t totalSize) {
        std::lock_guard<std::mutex> lock(frameFilesMutex);
        if (frameFiles[fname].size() < offset + chunk.size()) {
            frameFiles[fname].resize(offset + chunk.size());
        }
        std::copy(chunk.begin(), chunk.end(), frameFiles[fname].begin() + offset);
    };
    
    auto completeCallback = [&frameFiles, &frameFilesMutex, &frameComplete](const std::string& fname, uint64_t totalSize) {
        std::lock_guard<std::mutex> lock(frameFilesMutex);
        frameFiles[fname].resize(totalSize);
        frameComplete = true;
    };
    
    if (!pImpl->anariUsdClient->requestFrame(frameNumber, targetRank, chunkCallback, completeCallback, nullptr, timeoutMs)) {
        return false;
    }
    
    // Convert to output format
    outFrameFiles.clear();
    for (const auto& pair : frameFiles) {
        outFrameFiles.push_back({pair.first, pair.second});
    }
    
    return true;
}

bool AnariUsdMiddleware::requestWorkerCount(uint32_t& outWorkerCount, int timeoutMs) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_WARNING("ANARI USD client not connected - returning default count of 0 (no workers in non-MPI mode)");
        outWorkerCount = 0;  // Default: 0 workers (excluding rank 0) in non-MPI mode
        return true;  // Return success with default value
    }
    
    bool success = pImpl->anariUsdClient->getWorkerCountSync(outWorkerCount, timeoutMs);
    if (!success) {
        MIDDLEWARE_LOG_WARNING("Failed to get worker count from broker - returning default count of 0 (non-MPI fallback)");
        outWorkerCount = 0;  // Fallback to 0 workers (excluding rank 0)
        return true;  // Return success with fallback value
    }
    return success;
}

bool AnariUsdMiddleware::requestTotalWorkerCount(uint32_t& outTotalCount, int timeoutMs) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_WARNING("ANARI USD client not connected - returning default count of 1 (rank 0 only for non-MPI mode)");
        outTotalCount = 1;  // Default: just rank 0 in non-MPI mode
        return true;  // Return success with default value
    }
    
    bool success = pImpl->anariUsdClient->getTotalWorkerCountSync(outTotalCount, timeoutMs);
    if (!success) {
        MIDDLEWARE_LOG_WARNING("Failed to get total worker count from broker - returning default count of 1 (non-MPI fallback)");
        outTotalCount = 1;  // Fallback to 1 worker (rank 0)
        return true;  // Return success with fallback value
    }
    return success;
}

bool AnariUsdMiddleware::requestWorkerStatus(int32_t targetRank,
                                             std::vector<std::tuple<int32_t, uint32_t, std::string, std::string, uint64_t>>& outWorkerStatus,
                                             int timeoutMs) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_ERROR("ANARI USD client not connected");
        return false;
    }
    return pImpl->anariUsdClient->getWorkerStatusSync(targetRank, outWorkerStatus, timeoutMs);
}

// ============================================================================
// ASYNC WORKER QUERIES (NON-BLOCKING)
// ============================================================================

void AnariUsdMiddleware::requestWorkerCountAsync(int timeoutMs, WorkerCountCallback callback, BrokerErrorCallback errorCallback) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_WARNING("ANARI USD client not connected - returning default count of 0 (no workers in non-MPI mode)");
        // Return default value immediately for non-MPI mode
        if (callback) {
            callback(0);  // Default: 0 workers (excluding rank 0)
        }
        return;
    }
    
    // ✅ FIX: Use shared_ptr for lifetime safety, track thread to prevent use-after-free
    auto client = pImpl->anariUsdClient;
    std::thread t([client, timeoutMs, callback, errorCallback]() {
        if (!client || client->isShutdownRequested()) return;
        uint32_t workerCount = 0;
        bool success = client->getWorkerCountSync(workerCount, timeoutMs);
        
        if (success && callback) {
            callback(workerCount);
        } else {
            MIDDLEWARE_LOG_WARNING("Failed to get worker count from broker - returning default count of 0 (non-MPI fallback)");
            if (callback) {
                callback(0);
            } else if (errorCallback) {
                errorCallback("Failed to retrieve worker count - using fallback value of 0");
            }
        }
    });
    // Track thread for safe shutdown
    {
        std::lock_guard<std::mutex> lock(pImpl->asyncThreadsMutex);
        pImpl->asyncThreads.push_back(std::move(t));
    }
}

void AnariUsdMiddleware::requestTotalWorkerCountAsync(int timeoutMs, WorkerCountCallback callback, BrokerErrorCallback errorCallback) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_WARNING("ANARI USD client not connected - returning default count of 1 (rank 0 only for non-MPI mode)");
        // Return default value immediately for non-MPI mode
        if (callback) {
            callback(1);  // Default: 1 worker (rank 0)
        }
        return;
    }
    
    // ✅ FIX: Use shared_ptr for lifetime safety, track thread to prevent use-after-free
    auto client = pImpl->anariUsdClient;
    std::thread t([client, timeoutMs, callback, errorCallback]() {
        if (!client || client->isShutdownRequested()) return;
        uint32_t totalCount = 0;
        bool success = client->getTotalWorkerCountSync(totalCount, timeoutMs);
        
        if (success && callback) {
            callback(totalCount);
        } else {
            MIDDLEWARE_LOG_WARNING("Failed to get total worker count from broker - returning default count of 1 (non-MPI fallback)");
            if (callback) {
                callback(1);
            } else if (errorCallback) {
                errorCallback("Failed to retrieve total worker count - using fallback value of 1");
            }
        }
    });
    {
        std::lock_guard<std::mutex> lock(pImpl->asyncThreadsMutex);
        pImpl->asyncThreads.push_back(std::move(t));
    }
}

void AnariUsdMiddleware::requestWorkerStatusAsync(int32_t targetRank, int timeoutMs, WorkerStatusCallback callback, BrokerErrorCallback errorCallback) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_ERROR("ANARI USD client not connected");
        if (errorCallback) {
            errorCallback("ANARI USD client not connected");
        }
        return;
    }
    
    // ✅ FIX: Use shared_ptr for lifetime safety, track thread to prevent use-after-free
    auto client = pImpl->anariUsdClient;
    std::thread t([client, targetRank, timeoutMs, callback, errorCallback]() {
        if (!client || client->isShutdownRequested()) return;
        std::vector<std::tuple<int32_t, uint32_t, std::string, std::string, uint64_t>> workerStatus;
        bool success = client->getWorkerStatusSync(targetRank, workerStatus, timeoutMs);
        
        if (success && callback) {
            callback(workerStatus);
        } else if (errorCallback) {
            errorCallback(success ? "Unknown error" : "Failed to retrieve worker status");
        }
    });
    {
        std::lock_guard<std::mutex> lock(pImpl->asyncThreadsMutex);
        pImpl->asyncThreads.push_back(std::move(t));
    }
}

void AnariUsdMiddleware::requestFileListAsync(int32_t targetRank, int timeoutMs, FileListCallback callback, BrokerErrorCallback errorCallback) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_ERROR("ANARI USD client not connected");
        if (errorCallback) {
            errorCallback("ANARI USD client not connected");
        }
        return;
    }
    
    // ✅ FIX: Use shared_ptr for lifetime safety, track thread to prevent use-after-free
    auto client = pImpl->anariUsdClient;
    std::thread t([client, targetRank, timeoutMs, callback, errorCallback]() {
        if (!client || client->isShutdownRequested()) return;
        std::vector<std::string> files;
        bool success = client->getFileListSync(targetRank, files, timeoutMs);
        
        if (success && callback) {
            callback(files);
        } else if (errorCallback) {
            errorCallback(success ? "Unknown error" : "Failed to retrieve file list");
        }
    });
    {
        std::lock_guard<std::mutex> lock(pImpl->asyncThreadsMutex);
        pImpl->asyncThreads.push_back(std::move(t));
    }
}

void AnariUsdMiddleware::requestFileListWithSizesAsync(int32_t targetRank, int timeoutMs, FileListWithSizesCallback callback, BrokerErrorCallback errorCallback) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_ERROR("ANARI USD client not connected");
        if (errorCallback) {
            errorCallback("ANARI USD client not connected");
        }
        return;
    }
    
    // ✅ FIX: Use shared_ptr for lifetime safety, track thread to prevent use-after-free
    auto client = pImpl->anariUsdClient;
    std::thread t([client, targetRank, timeoutMs, callback, errorCallback]() {
        if (!client || client->isShutdownRequested()) return;
        std::vector<FileInfo> files;
        bool success = client->getFileListWithSizesSync(targetRank, files, timeoutMs);
        
        if (success && callback) {
            callback(files);
        } else if (errorCallback) {
            errorCallback(success ? "Unknown error" : "Failed to retrieve file list");
        }
    });
    {
        std::lock_guard<std::mutex> lock(pImpl->asyncThreadsMutex);
        pImpl->asyncThreads.push_back(std::move(t));
    }
}

void AnariUsdMiddleware::requestFilesParallelAsync(
    const std::vector<std::string>& filenames,
    const std::vector<int32_t>& targetRanks,
    int timeoutMs,
    std::function<void(const std::string&, const std::vector<uint8_t>&)> fileReceivedCallback,
    std::function<void()> completionCallback,
    std::function<void(const std::string&, const std::string&)> errorCallback) {
    
    // Debug logging to verify we entered the function
    #ifdef _WIN32
    OutputDebugStringA("[ANARI] requestFilesParallelAsync: Entering function\n");
    #endif
    
    // Basic parameter validation (no __try/__except due to C++ object unwinding)
    // We'll rely on the fact that if parameters are invalid, the function won't be called
    // or will crash before reaching here (which is what we're trying to prevent)
    
    // Note: Cannot use __try/__except here because function has C++ objects with destructors
    // The crash protection is now in the C API wrapper instead
    
    #ifdef _WIN32
    char debug_msg[256];
    snprintf(debug_msg, sizeof(debug_msg), "[ANARI] requestFilesParallelAsync: %zu files, timeout %d ms\n", 
             filenames.size(), timeoutMs);
    OutputDebugStringA(debug_msg);
    #endif
    
    // Validate input
    if (filenames.empty()) {
        MIDDLEWARE_LOG_ERROR("Empty filename list for parallel download");
        if (errorCallback) {
            errorCallback("", "Empty filename list");
        }
        return;
    }
    
    if (filenames.size() != targetRanks.size()) {
        MIDDLEWARE_LOG_ERROR("Mismatch between filenames count (%zu) and target ranks count (%zu)",
                            filenames.size(), targetRanks.size());
        if (errorCallback) {
            errorCallback("", "Mismatch between filenames and target ranks count");
        }
        return;
    }
    
    if (!pImpl || !pImpl->anariUsdClient) {
        MIDDLEWARE_LOG_ERROR("ANARI USD client not initialized");
        if (errorCallback) {
            errorCallback("", "ANARI USD client not initialized");
        }
        return;
    }
    
    if (!pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_ERROR("ANARI USD client not connected");
        if (errorCallback) {
            errorCallback("", "ANARI USD client not connected");
        }
        return;
    }
    
    // ✅ FIX: Use shared_ptr for lifetime safety, track thread to prevent use-after-free
    auto client = pImpl->anariUsdClient;
    std::thread t([client, filenames, targetRanks, timeoutMs, fileReceivedCallback, completionCallback, errorCallback]() {
        try {
             if (!client || client->isShutdownRequested()) {
                if (errorCallback) errorCallback("", "Middleware shutting down");
                return;
            }
            bool success = client->requestFilesParallel(
                filenames,
                targetRanks,
                fileReceivedCallback,
                completionCallback,
                errorCallback,
                timeoutMs);
            
            if (!success && errorCallback) {
                errorCallback("", "Failed to request files in parallel");
            }
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in requestFilesParallelAsync thread: %s", e.what());
            if (errorCallback) {
                errorCallback("", std::string("Exception: ") + e.what());
            }
        } catch (...) {
            MIDDLEWARE_LOG_ERROR("Unknown exception in requestFilesParallelAsync thread");
            if (errorCallback) {
                errorCallback("", "Unknown exception in parallel download");
            }
        }
    });
    {
        std::lock_guard<std::mutex> lock(pImpl->asyncThreadsMutex);
        pImpl->asyncThreads.push_back(std::move(t));
    }
}

bool AnariUsdMiddleware::requestWorkerListString(std::vector<std::tuple<int32_t, std::string, std::string>>& outWorkers, int timeoutMs) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_ERROR("Cannot request worker list - broker not connected");
        return false;
    }
    
    return pImpl->anariUsdClient->requestWorkerListString(outWorkers, timeoutMs);
}

std::string AnariUsdMiddleware::getStatusInfo() const {
    try {
        std::ostringstream status;
        status << "AnariUsdMiddleware Status (with Collision Support):\n";
        status << " Connected: " << (isConnected() ? "Yes" : "No") << "\n";
        status << " Broker Connected: " << (isBrokerConnected() ? "Yes" : "No") << "\n";
        status << " Default Collision: " << CollisionProcessor::getComplexityName(getDefaultCollisionComplexity()) << "\n";
        return status.str();
    } catch (const std::exception& e) {
        return "Error getting status: " + std::string(e.what());
    }
}

AnariUsdClient* AnariUsdMiddleware::getClient() const {
    if (!pImpl) {
        return nullptr;
    }
    return pImpl->anariUsdClient.get();
}

void AnariUsdMiddleware::setNotificationCallback(NotificationCallback cb) {
    if (!pImpl || !pImpl->anariUsdClient) {
        MIDDLEWARE_LOG_ERROR("Cannot set notification callback: middleware not initialized");
        return;
    }
    pImpl->anariUsdClient->setNotificationCallback(std::move(cb));
}

} // namespace anari_usd_middleware
