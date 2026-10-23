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
#include <sstream>
#include <map>
#include <unordered_map>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace anari_usd_middleware {


class AnariUsdMiddleware::Impl {
public:
    ZmqConnector zmqConnector;
    std::shared_ptr<AnariUsdClient> anariUsdClient;  // ✅ FIX: shared_ptr for safe capture in async threads
    std::unique_ptr<UsdProcessor> usdProcessor;
    std::unique_ptr<CollisionProcessor> collisionProcessor;  // ✅ NEW: Collision processor

    std::map<int, FileUpdateCallback> updateCallbacks;
    std::map<int, MessageCallback> messageCallbacks;
    std::mutex callbackMutex;
    std::atomic<int> nextCallbackId{1};

    std::thread receiverThread;
    std::atomic<bool> running{false};
    std::atomic<bool> shutdownRequested{false};
    std::vector<std::thread> asyncThreads;
    std::mutex asyncThreadsMutex;

    std::mutex initMutex;
    std::atomic<bool> initialized{false};
    std::chrono::steady_clock::time_point initializationTime;

    std::map<std::string, std::vector<uint8_t>> cachedTextures;
    std::mutex textureCacheMutex;

    std::atomic<ECollisionComplexity> defaultCollisionComplexity{ECollisionComplexity::Complex};
    std::mutex collisionConfigMutex;

public:
    Impl() : nextCallbackId(1), running(false), shutdownRequested(false) {
        MIDDLEWARE_LOG_INFO("AnariUsdMiddleware::Impl created with collision support");
        initializationTime = std::chrono::steady_clock::now();
    }

    ~Impl() {
        MIDDLEWARE_LOG_INFO("AnariUsdMiddleware::Impl destroyed");
        shutdownRequested.store(true);
        stopReceiving();
        zmqConnector.disconnect();
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
            usdProcessor = std::make_unique<UsdProcessor>();
            if (!usdProcessor) {
                MIDDLEWARE_LOG_ERROR("Failed to create USD processor");
                return false;
            }

            usdProcessor->setMaxRecursionDepth(50);
            usdProcessor->setMemoryLimit(1024);
            usdProcessor->setReferenceResolutionEnabled(true);
            MIDDLEWARE_LOG_INFO("USD processor initialized successfully");

            collisionProcessor = std::make_unique<CollisionProcessor>();
            if (!collisionProcessor) {
                MIDDLEWARE_LOG_ERROR("Failed to create collision processor");
                usdProcessor.reset();
                return false;
            }
            MIDDLEWARE_LOG_INFO("Collision processor initialized successfully");

            bool zmqResult = zmqConnector.initialize(endpoint, 5000);
            if (!zmqResult) {
                MIDDLEWARE_LOG_ERROR("Failed to initialize ZMQ connector");
                cleanup();
                return false;
            }

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

            if (collisionProcessor) {
                collisionProcessor.reset();
                MIDDLEWARE_LOG_INFO("Collision processor cleaned up");
            }

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
        return initialized.load() && !shutdownRequested.load();
    }

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
        if (running.load()) {
            MIDDLEWARE_LOG_INFO("Receiver thread already marked as running");
            return true;
        }

        if (!isConnected()) {
            MIDDLEWARE_LOG_ERROR("Cannot start receiver: middleware not initialized");
            return false;
        }

        MIDDLEWARE_LOG_INFO("DEALER-only mode: Receiver thread not needed (synchronous communication)");
        running.store(true); // Mark as "running" to satisfy API
        return true;
    }

    void stopReceiving() {
        if (!running.load()) {
            return;
        }

        MIDDLEWARE_LOG_INFO("Stopping receiver (DEALER-only mode)...");
        running.store(false);

        MIDDLEWARE_LOG_INFO("Receiver stopped (DEALER-only mode)");
    }

    bool LoadUSDBuffer(const std::vector<uint8_t>& buffer, const std::string& fileName,
                      std::vector<MeshData>& outMeshData) {
        return LoadUSDBufferWithCollision(buffer, fileName, ECollisionComplexity::None, outMeshData);
    }

    bool LoadUSDFromDisk(const std::string& filePath, std::vector<MeshData>& outMeshData) {
        return LoadUSDFromDiskWithCollision(filePath, ECollisionComplexity::None, outMeshData);
    }

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

            outMeshData.clear();
            outMeshData.reserve(processorMeshData.size());

            for (auto& processorMesh : processorMeshData) {
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
            if (!validateFilePath(filePath)) {
                return false;
            }

            std::vector<uint8_t> buffer;
            if (!readFileToBuffer(filePath, buffer)) {
                return false;
            }

            return LoadUSDBufferWithCollision(buffer, filePath, complexity, outMeshData);

        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in LoadUSDFromDiskWithCollision: %s - %s",
                               filePath.c_str(), e.what());
            return false;
        }
    }

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

            TextureData result;
            result.width = processorTexData.width;
            result.height = processorTexData.height;
            result.channels = processorTexData.channels;
            result.data = std::move(processorTexData.data);

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

            TextureData texData = CreateTextureFromBuffer(buffer);

            if (!texData.isValid() || texData.height != 1) {
                MIDDLEWARE_LOG_ERROR("Invalid gradient data for PNG writing: width=%d, height=%d",
                                   texData.width, texData.height);
                return false;
            }

            std::filesystem::path outputPath(outPath);
            if (outputPath.has_parent_path()) {
                std::filesystem::create_directories(outputPath.parent_path());
            }

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
            if (cachedTextures.empty()) {
                MIDDLEWARE_LOG_INFO("No cached gradient texture available");
                return false;
            }
            auto it = cachedTextures.end();
            --it;
            outData = it->second;
            TextureData texData = CreateTextureFromBuffer(outData);
            if (!texData.isValid()) {
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
    bool convertMeshDataWithCollision(UsdProcessor::MeshData& processorMeshData,
                                      ECollisionComplexity complexity,
                                      MeshData& publicMeshData) {
        try {
            publicMeshData.elementName = std::move(processorMeshData.elementName);
            publicMeshData.typeName = std::move(processorMeshData.typeName);

            size_t pointCount = processorMeshData.points.size();
            publicMeshData.points.clear();
            publicMeshData.points.resize(pointCount * 3);
            for (size_t i = 0; i < pointCount; ++i) {
                const auto& p = processorMeshData.points[i];
                float* out = publicMeshData.points.data() + i * 3;
                out[0] = p.x; out[1] = p.y; out[2] = p.z;
            }

            publicMeshData.indices = processorMeshData.indices;

            publicMeshData.normals.clear();
            {
                size_t normalCount = processorMeshData.normals.size();
                publicMeshData.normals.resize(normalCount * 3);
                for (size_t i = 0; i < normalCount; ++i) {
                    const auto& n = processorMeshData.normals[i];
                    float* out = publicMeshData.normals.data() + i * 3;
                    out[0] = n.x; out[1] = n.y; out[2] = n.z;
                }
            }

            publicMeshData.uvs.clear();
            {
                size_t uvCount = processorMeshData.uvs.size();
                publicMeshData.uvs.resize(uvCount * 2);
                for (size_t i = 0; i < uvCount; ++i) {
                    const auto& u = processorMeshData.uvs[i];
                    float* out = publicMeshData.uvs.data() + i * 2;
                    out[0] = u.x; out[1] = u.y;
                }
            }

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
                    publicMeshData.vertex_colors.resize(colorCount * 4);
                    for (size_t i = 0; i < colorCount; ++i) {
                        const auto& c = processorMeshData.vertex_colors[i];
                        float* out = publicMeshData.vertex_colors.data() + i * 4;
                        out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = c.a;
                    }
                } else if (isUniformInterp) {
                    std::vector<glm::vec4> vertexColors(pointCount, glm::vec4(1.0f));
                    for (size_t faceIdx = 0; faceIdx < faceCount; ++faceIdx) {
                        if (faceIdx >= colorCount) break;
                        const auto& faceColor = processorMeshData.vertex_colors[faceIdx];
                        size_t i0 = processorMeshData.indices[faceIdx * 3 + 0];
                        size_t i1 = processorMeshData.indices[faceIdx * 3 + 1];
                        size_t i2 = processorMeshData.indices[faceIdx * 3 + 2];
                        if (i0 < pointCount) vertexColors[i0] = faceColor;
                        if (i1 < pointCount) vertexColors[i1] = faceColor;
                        if (i2 < pointCount) vertexColors[i2] = faceColor;
                    }
                    publicMeshData.vertex_colors.resize(pointCount * 4);
                    for (size_t i = 0; i < pointCount; ++i) {
                        const auto& c = vertexColors[i];
                        float* out = publicMeshData.vertex_colors.data() + i * 4;
                        out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = c.a;
                    }
                } else {
                    MIDDLEWARE_LOG_WARNING("Color count mismatch - using fallback vertex mapping");
                    publicMeshData.vertex_colors.resize(pointCount * 4);
                    float* outData = publicMeshData.vertex_colors.data();
                    for (size_t i = 0; i < pointCount; ++i) {
                        float* out = outData + i * 4;
                        if (i < colorCount) {
                            const auto& c = processorMeshData.vertex_colors[i];
                            out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = c.a;
                        } else {
                            out[0] = 1.0f; out[1] = 1.0f; out[2] = 1.0f; out[3] = 1.0f;
                        }
                    }
                }
            }

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
                publicMeshData.collision.clear();
            }

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

    static void writeToVector(void* context, void* data, int size) {
        auto* vec = reinterpret_cast<std::vector<uint8_t>*>(context);
        uint8_t* bytes = reinterpret_cast<uint8_t*>(data);
        vec->insert(vec->end(), bytes, bytes + size);
    }
};


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

void AnariUsdMiddleware::setDefaultCollisionComplexity(ECollisionComplexity complexity) {
    pImpl->setDefaultCollisionComplexity(complexity);
}

ECollisionComplexity AnariUsdMiddleware::getDefaultCollisionComplexity() const {
    return pImpl->getDefaultCollisionComplexity();
}

void AnariUsdMiddleware::setCollisionParameters(float simplificationRatio, float convexHullPrecision, int maxConvexHulls) {
    pImpl->setCollisionParameters(simplificationRatio, convexHullPrecision, maxConvexHulls);
}

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

bool AnariUsdMiddleware::LoadUSDBufferWithCollision(const std::vector<uint8_t>& buffer, const std::string& fileName,
                                                   ECollisionComplexity complexity, std::vector<MeshData>& outMeshData) {
    return pImpl->LoadUSDBufferWithCollision(buffer, fileName, complexity, outMeshData);
}

bool AnariUsdMiddleware::LoadUSDFromDiskWithCollision(const std::string& filePath, ECollisionComplexity complexity,
                                                     std::vector<MeshData>& outMeshData) {
    return pImpl->LoadUSDFromDiskWithCollision(filePath, complexity, outMeshData);
}

bool AnariUsdMiddleware::WriteGradientLineAsPNG(const std::vector<uint8_t>& buffer, const std::string& outPath) {
    return pImpl->WriteGradientLineAsPNG(buffer, outPath);
}

bool AnariUsdMiddleware::GetGradientLineAsPNGBuffer(const std::vector<uint8_t>& buffer, std::vector<uint8_t>& outPngBuffer) {
    return pImpl->GetGradientLineAsPNGBuffer(buffer, outPngBuffer);
}

bool AnariUsdMiddleware::GetCachedGradientTexture(std::vector<uint8_t>& outData, int& outWidth, int& outHeight) {
    return pImpl->GetCachedGradientTexture(outData, outWidth, outHeight);
}

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

namespace {
struct JusyncAbortRegistry {
    std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<std::atomic<bool>>> tokens;
    std::atomic<uint64_t> nextHandle{1};
};
JusyncAbortRegistry g_abortRegistry;
} // namespace

uint64_t AnariUsdMiddleware::createAbortToken() {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    const uint64_t handle = g_abortRegistry.nextHandle.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_abortRegistry.mutex);
    g_abortRegistry.tokens[handle] = std::move(flag);
    return handle;
}

bool AnariUsdMiddleware::triggerAbortToken(uint64_t handle) {
    std::shared_ptr<std::atomic<bool>> flag;
    {
        std::lock_guard<std::mutex> lock(g_abortRegistry.mutex);
        auto it = g_abortRegistry.tokens.find(handle);
        if (it == g_abortRegistry.tokens.end()) {
            return false; // released or never existed
        }
        flag = it->second;
    }
    flag->store(true, std::memory_order_release);
    if (pImpl->anariUsdClient) {
        pImpl->anariUsdClient->wakeResponseWaiters();
    }
    return true;
}

void AnariUsdMiddleware::releaseAbortToken(uint64_t handle) {
    std::lock_guard<std::mutex> lock(g_abortRegistry.mutex);
    g_abortRegistry.tokens.erase(handle);
}

bool AnariUsdMiddleware::requestFileIntoBuffer(const std::string& filename, int32_t targetRank,
                                                uint8_t* outBuffer, size_t outCapacity,
                                                size_t& outSize, int& outStatus, int timeoutMs,
                                                uint64_t abortHandle) {
    outSize = 0;
    outStatus = 0;
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_ERROR("ANARI USD client not connected");
        return false;
    }
    if (!outBuffer && outCapacity > 0) {
        MIDDLEWARE_LOG_ERROR("RequestFileIntoBuffer: null buffer with nonzero capacity");
        return false;
    }

    std::shared_ptr<std::atomic<bool>> abortFlag;
    if (abortHandle != 0) {
        std::lock_guard<std::mutex> lock(g_abortRegistry.mutex);
        auto it = g_abortRegistry.tokens.find(abortHandle);
        if (it != g_abortRegistry.tokens.end()) {
            abortFlag = it->second;
        } else {
            MIDDLEWARE_LOG_WARNING("RequestFileIntoBuffer: unknown abort handle %llu for '%s'",
                                   static_cast<unsigned long long>(abortHandle), filename.c_str());
        }
    }

    size_t written = 0;
    bool overflow = false;
    size_t totalSize = 0;

    auto chunkCallback = [&outBuffer, outCapacity, &written, &overflow](
                             const std::string& /*fname*/, const uint8_t* chunk,
                             size_t chunkSize, uint64_t offset, uint64_t /*totalSize*/) {
        if (overflow) return;
        if (offset >= outCapacity || chunkSize > outCapacity - offset) {
            overflow = true;
            return;
        }
        std::memcpy(outBuffer + offset, chunk, chunkSize);
        if (offset + chunkSize > written) written = offset + chunkSize;
    };

    auto completeCallback = [&outBuffer, outCapacity, &written, &totalSize, &overflow](
                                const std::string& /*fname*/, uint64_t size) {
        if (overflow) return;
        if (size > outCapacity) {
            overflow = true;
            return;
        }
        if (written < size) {
            std::memset(outBuffer + written, 0, size - written);
            written = size;
        }
        totalSize = size;
    };

    if (!pImpl->anariUsdClient->requestFile(filename, targetRank,
                                            chunkCallback, completeCallback,
                                            nullptr, timeoutMs, abortFlag.get())) {
        outSize = 0;
        outStatus = 0;
        return false;
    }

    if (overflow) {
        MIDDLEWARE_LOG_ERROR("File '%s' does not fit into %zu byte buffer", filename.c_str(), outCapacity);
        outSize = 0;
        outStatus = -1;
        return true;
    }

    outSize = totalSize;
    outStatus = 1;
    return true;
}

bool AnariUsdMiddleware::requestFrame(int32_t frameNumber, int32_t targetRank,
                                       std::vector<std::pair<std::string, std::vector<uint8_t>>>& outFrameFiles,
                                       int timeoutMs) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_ERROR("ANARI USD client not connected");
        return false;
    }

    std::map<std::string, std::vector<uint8_t>> frameFiles;
    std::mutex frameFilesMutex;
    bool frameComplete = false;

    auto chunkCallback = [&frameFiles, &frameFilesMutex](const std::string& fname,
                                                          const uint8_t* chunk, size_t chunkSize,
                                                          uint64_t offset, uint64_t totalSize) {
        std::lock_guard<std::mutex> lock(frameFilesMutex);
        if (frameFiles[fname].size() < offset + chunkSize) {
            frameFiles[fname].resize(offset + chunkSize);
        }
        std::memcpy(frameFiles[fname].data() + offset, chunk, chunkSize);
    };

    auto completeCallback = [&frameFiles, &frameFilesMutex, &frameComplete](const std::string& fname, uint64_t totalSize) {
        if (fname == "__frame_complete__") {
            std::lock_guard<std::mutex> lock(frameFilesMutex);
            frameComplete = true;
            return;
        }
        std::lock_guard<std::mutex> lock(frameFilesMutex);
        frameFiles[fname].resize(totalSize);
        frameComplete = true;
    };

    if (!pImpl->anariUsdClient->requestFrame(frameNumber, targetRank, chunkCallback, completeCallback, nullptr, timeoutMs)) {
        return false;
    }

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


void AnariUsdMiddleware::requestWorkerCountAsync(int timeoutMs, WorkerCountCallback callback, BrokerErrorCallback errorCallback) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_WARNING("ANARI USD client not connected - returning default count of 0 (no workers in non-MPI mode)");
        if (callback) {
            callback(0);  // Default: 0 workers (excluding rank 0)
        }
        return;
    }

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
    {
        std::lock_guard<std::mutex> lock(pImpl->asyncThreadsMutex);
        pImpl->asyncThreads.push_back(std::move(t));
    }
}

void AnariUsdMiddleware::requestTotalWorkerCountAsync(int timeoutMs, WorkerCountCallback callback, BrokerErrorCallback errorCallback) {
    if (!pImpl->anariUsdClient || !pImpl->anariUsdClient->isConnected()) {
        MIDDLEWARE_LOG_WARNING("ANARI USD client not connected - returning default count of 1 (rank 0 only for non-MPI mode)");
        if (callback) {
            callback(1);  // Default: 1 worker (rank 0)
        }
        return;
    }

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

    #ifdef _WIN32
    OutputDebugStringA("[ANARI] requestFilesParallelAsync: Entering function\n");
    #endif


    #ifdef _WIN32
    char debug_msg[256];
    snprintf(debug_msg, sizeof(debug_msg), "[ANARI] requestFilesParallelAsync: %zu files, timeout %d ms\n",
             filenames.size(), timeoutMs);
    OutputDebugStringA(debug_msg);
    #endif

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

void AnariUsdMiddleware::setSceneUpdateCallback(SceneUpdateCallback cb) {
    if (!pImpl || !pImpl->anariUsdClient) {
        MIDDLEWARE_LOG_ERROR("Cannot set scene update callback: middleware not initialized");
        return;
    }
    pImpl->anariUsdClient->setSceneUpdateCallback(std::move(cb));
}

void AnariUsdMiddleware::setProtocolDiagnosticsCallback(ProtocolDiagnosticsCallback cb) {
    if (!pImpl || !pImpl->anariUsdClient) {
        MIDDLEWARE_LOG_ERROR("Cannot set protocol diagnostics callback: middleware not initialized");
        return;
    }
    pImpl->anariUsdClient->setProtocolDiagnosticsCallback(std::move(cb));
}

uint32_t AnariUsdMiddleware::getProtocolVersion() const {
    if (!pImpl || !pImpl->anariUsdClient) {
        return ANARI_USD_PROTOCOL_VERSION;
    }
    return pImpl->anariUsdClient->getProtocolVersion();
}

} // namespace anari_usd_middleware
