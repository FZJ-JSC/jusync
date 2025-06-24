#include "AnariUsdMiddleware.h"
#include "ZmqConnector.h"
#include "HashVerifier.h"
#include "UsdProcessor.h"
#include "MiddlewareLogging.h"

#include <memory>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <unordered_set>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace anari_usd_middleware {

// Implementation class with all required methods
class AnariUsdMiddleware::Impl {
    // Core components
    ZmqConnector zmqConnector;
    std::unique_ptr<UsdProcessor> usdProcessor;

    // Callback management
    std::map<int, AnariUsdMiddleware::FileUpdateCallback> updateCallbacks;
    std::map<int, AnariUsdMiddleware::MessageCallback> messageCallbacks;
    std::mutex callbackMutex;
    std::atomic<int> nextCallbackId{1};

    // Threading
    std::thread receiverThread;
    std::atomic<bool> running{false};
    std::atomic<bool> shutdownRequested{false};

    // Initialization
    std::mutex initMutex;
    std::atomic<bool> initialized{false};
    std::chrono::steady_clock::time_point initializationTime;

    // File tracking for duplicate prevention
    std::unordered_set<std::string> processedFiles;
    std::mutex processedFilesMutex;
    std::chrono::steady_clock::time_point lastCleanup;
    std::atomic<size_t> maxTrackedFiles{10000};

public:
    Impl() : nextCallbackId(1), running(false), shutdownRequested(false) {
        MIDDLEWARE_LOG_INFO("AnariUsdMiddleware::Impl created with enhanced safety features");
        initializationTime = std::chrono::steady_clock::now();
        lastCleanup = std::chrono::steady_clock::now();
    }

    ~Impl() {
        MIDDLEWARE_LOG_INFO("AnariUsdMiddleware::Impl destroyed");
        stopReceiving();
        zmqConnector.disconnect();
    }

    bool initialize(const char* endpoint) {
        std::lock_guard<std::mutex> lock(initMutex);
        if (initialized.load()) {
            MIDDLEWARE_LOG_WARNING("AnariUsdMiddleware already initialized");
            return true;
        }

        MIDDLEWARE_LOG_INFO("Initializing AnariUsdMiddleware with enhanced safety...");
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

            // Initialize ZMQ connection with enhanced error handling
            bool zmqResult = zmqConnector.initialize(endpoint, 5000);
            if (!zmqResult) {
                MIDDLEWARE_LOG_ERROR("Failed to initialize ZMQ connector");
                usdProcessor.reset();
                return false;
            }

            // Set safe message size limits
            zmqConnector.setMaxMessageSize(safety::MAX_BUFFER_SIZE);
            initialized.store(true);
            MIDDLEWARE_LOG_INFO("AnariUsdMiddleware initialized successfully");
            return true;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception during initialization: %s", e.what());
            cleanup();
            return false;
        }
    }

    void shutdown() {
        MIDDLEWARE_LOG_INFO("Shutting down AnariUsdMiddleware...");
        shutdownRequested.store(true);
        stopReceiving();

        std::lock_guard<std::mutex> lock(initMutex);
        try {
            zmqConnector.disconnect(1000);

            if (usdProcessor) {
                // Get final statistics before shutdown
                auto stats = usdProcessor->getProcessingStats();
                MIDDLEWARE_LOG_INFO("Final processing stats - Files: %zu, Meshes: %zu, Errors: %zu",
                                    static_cast<size_t>(stats.filesProcessed),
                                    static_cast<size_t>(stats.meshesExtracted),
                                    static_cast<size_t>(stats.processingErrors));
                usdProcessor.reset();
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
        return initialized.load() && zmqConnector.isConnected() && !shutdownRequested.load();
    }

    int registerUpdateCallback(AnariUsdMiddleware::FileUpdateCallback callback) {
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

    int registerMessageCallback(AnariUsdMiddleware::MessageCallback callback) {
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
            MIDDLEWARE_LOG_INFO("Receiver thread already running");
            return true;
        }

        if (!isConnected()) {
            MIDDLEWARE_LOG_ERROR("Cannot start receiver thread: not connected");
            return false;
        }

        try {
            running.store(true);
            receiverThread = std::thread(&Impl::receiverLoop, this);
            MIDDLEWARE_LOG_INFO("Receiver thread started successfully");
            return true;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Failed to start receiver thread: %s", e.what());
            running.store(false);
            return false;
        }
    }

    void stopReceiving() {
        if (!running.load()) {
            return;
        }

        MIDDLEWARE_LOG_INFO("Stopping receiver thread...");
        running.store(false);

        // Give the thread time to observe the flag
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        if (receiverThread.joinable()) {
            try {
                receiverThread.join();
                MIDDLEWARE_LOG_INFO("Receiver thread joined successfully");
            } catch (const std::exception& e) {
                MIDDLEWARE_LOG_ERROR("Exception joining receiver thread: %s", e.what());
            }
        }
    }

    void receiverLoop() {
        MIDDLEWARE_LOG_INFO("Enhanced receiver thread started");
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
        MIDDLEWARE_LOG_DEBUG("=== PROCESSING INCOMING MESSAGE ===");
        try {
            AnariUsdMiddleware::FileData fileData;

            // Call receiveFile with 0 timeout since we know data is available
            if (zmqConnector.receiveFile(fileData.filename, fileData.data, fileData.hash, 0)) {
                MIDDLEWARE_LOG_INFO("Successfully received file via ZMQ: %s (%zu bytes)",
                                    fileData.filename.c_str(), fileData.data.size());
                return processReceivedFile(fileData);
            }

            MIDDLEWARE_LOG_DEBUG("Not a file message, trying as generic message");
            // If not a file, try to receive as a generic message with 0 timeout
            if (zmqConnector.receiveAnyMessage(0)) {
                MIDDLEWARE_LOG_INFO("Successfully received generic message via ZMQ");
                return processReceivedMessage();
            }

            MIDDLEWARE_LOG_WARNING("No valid message could be processed");
            return false;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in processIncomingMessage: %s", e.what());
            return false;
        }
    }

    bool processReceivedFile(AnariUsdMiddleware::FileData& fileData) {
        MIDDLEWARE_LOG_INFO("Processing received file: %s (size: %zu bytes, hash: %s)",
                            fileData.filename.c_str(), fileData.data.size(), fileData.hash.c_str());
        try {
            // Basic validation
            if (fileData.filename.empty() || fileData.data.empty()) {
                MIDDLEWARE_LOG_ERROR("File data validation failed: empty filename or data");
                return false;
            }

            // CRITICAL FIX: Check for duplicate files
            if (isDuplicateFile(fileData.filename, fileData.hash)) {
                MIDDLEWARE_LOG_WARNING("Duplicate file detected, skipping: %s", fileData.filename.c_str());
                return true; // Return true to indicate "successful" handling (just skipped)
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

            // Hash verification (non-fatal)
            if (HashVerifier::verifyHash(fileData.data, fileData.hash)) {
                MIDDLEWARE_LOG_INFO("Hash verification succeeded for file: %s", fileData.filename.c_str());
            } else {
                MIDDLEWARE_LOG_WARNING("Hash verification failed for file: %s (continuing anyway)", fileData.filename.c_str());
            }

            // CRITICAL: Mark file as processed BEFORE notifying callbacks
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

            // Enhanced message format detection
            std::string messageType = "PLAIN_TEXT";
            if (isJsonMessage(message)) {
                messageType = "JSON";
                MIDDLEWARE_LOG_DEBUG("Detected JSON message format");
            } else if (isXmlMessage(message)) {
                messageType = "XML";
                MIDDLEWARE_LOG_DEBUG("Detected XML message format");
            }

            // Notify callbacks with error handling
            notifyMessageCallbacks(message);
            return true;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception processing received message: %s", e.what());
            return false;
        }
    }

    // CRITICAL: Add duplicate detection methods
    bool isDuplicateFile(const std::string& filename, const std::string& hash) {
        std::lock_guard<std::mutex> lock(processedFilesMutex);

        // Create unique identifier combining filename and hash
        std::string fileIdentifier = filename + ":" + hash;

        // Check if already processed
        bool isDuplicate = processedFiles.find(fileIdentifier) != processedFiles.end();

        if (isDuplicate) {
            MIDDLEWARE_LOG_DEBUG("Duplicate detected: %s", filename.c_str());
        }

        // Periodic cleanup to prevent memory growth
        auto now = std::chrono::steady_clock::now();
        if (now - lastCleanup > std::chrono::hours(1)) { // Cleanup every hour
            cleanupOldEntries();
            lastCleanup = now;
        }

        return isDuplicate;
    }

    void markFileAsProcessed(const std::string& filename, const std::string& hash) {
        std::lock_guard<std::mutex> lock(processedFilesMutex);

        // Create unique identifier
        std::string fileIdentifier = filename + ":" + hash;

        // Add to processed set
        processedFiles.insert(fileIdentifier);

        MIDDLEWARE_LOG_DEBUG("Marked as processed: %s", filename.c_str());

        // Prevent memory growth by limiting tracked files
        if (processedFiles.size() > maxTrackedFiles.load()) {
            // Remove oldest entries (simple approach - remove 10% of entries)
            auto it = processedFiles.begin();
            size_t toRemove = processedFiles.size() / 10;
            for (size_t i = 0; i < toRemove && it != processedFiles.end(); ++i) {
                it = processedFiles.erase(it);
            }
            MIDDLEWARE_LOG_INFO("Cleaned up %zu old file entries", toRemove);
        }
    }

    void cleanupOldEntries() {
        // Simple cleanup - clear all after time period
        if (processedFiles.size() > maxTrackedFiles.load() / 2) {
            size_t originalSize = processedFiles.size();
            processedFiles.clear();
            MIDDLEWARE_LOG_INFO("Cleared %zu processed file entries during cleanup", originalSize);
        }
    }

    void notifyFileCallbacks(const AnariUsdMiddleware::FileData& fileData) {
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

    // Enhanced texture creation with comprehensive validation
    AnariUsdMiddleware::TextureData CreateTextureFromBuffer(const std::vector<uint8_t>& buffer) {
        if (!usdProcessor) {
            MIDDLEWARE_LOG_ERROR("USD processor not initialized");
            return AnariUsdMiddleware::TextureData();
        }

        if (shutdownRequested.load()) {
            MIDDLEWARE_LOG_WARNING("Texture creation aborted: shutdown requested");
            return AnariUsdMiddleware::TextureData();
        }

        try {
            UsdProcessor::TextureData processorTexData = usdProcessor->CreateTextureFromBuffer(buffer);

            // Convert to public API structure with validation
            AnariUsdMiddleware::TextureData result;
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
            return AnariUsdMiddleware::TextureData();
        }
    }

    // Enhanced USD buffer loading with type conversion safety
    bool LoadUSDBuffer(const std::vector<uint8_t>& buffer, const std::string& fileName,
                       std::vector<AnariUsdMiddleware::MeshData>& outMeshData) {
        if (!usdProcessor) {
            MIDDLEWARE_LOG_ERROR("USD processor not initialized");
            return false;
        }

        if (shutdownRequested.load()) {
            MIDDLEWARE_LOG_WARNING("USD loading aborted: shutdown requested");
            return false;
        }

        try {
            // Use internal processor mesh data format
            std::vector<UsdProcessor::MeshData> processorMeshData;

            // Progress callback for monitoring
            auto progressCallback = [this](float progress, const std::string& status) {
                if (progress == 1.0f) {
                    MIDDLEWARE_LOG_INFO("USD processing complete: %s", status.c_str());
                } else if (static_cast<int>(progress * 10) % 2 == 0) {
                    MIDDLEWARE_LOG_DEBUG("USD processing progress: %.1f%% - %s",
                                        progress * 100.0f, status.c_str());
                }
            };

            bool result = usdProcessor->LoadUSDBuffer(buffer, fileName, processorMeshData, progressCallback);

            if (result && !processorMeshData.empty()) {
                // Convert to public API structure with enhanced safety
                outMeshData.clear();
                outMeshData.reserve(processorMeshData.size());

                for (const auto& meshData : processorMeshData) {
                    AnariUsdMiddleware::MeshData publicMeshData;
                    if (!convertMeshData(meshData, publicMeshData)) {
                        MIDDLEWARE_LOG_WARNING("Failed to convert mesh data: %s", meshData.elementName.c_str());
                        continue;
                    }
                    outMeshData.push_back(std::move(publicMeshData));
                }

                MIDDLEWARE_LOG_INFO("Successfully converted %zu meshes to public API format", outMeshData.size());
            }

            return result;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in LoadUSDBuffer: %s", e.what());
            return false;
        }
    }

    // Enhanced disk loading with comprehensive file validation
    bool LoadUSDFromDisk(const std::string& filePath, std::vector<AnariUsdMiddleware::MeshData>& outMeshData) {
        MIDDLEWARE_LOG_INFO("Loading USD from disk with enhanced validation: %s", filePath.c_str());
        try {
            // Comprehensive file validation
            if (!validateFilePath(filePath)) {
                return false;
            }

            // Read file with enhanced error handling
            std::vector<uint8_t> buffer;
            if (!readFileToBuffer(filePath, buffer)) {
                return false;
            }

            // Use existing buffer processing
            return LoadUSDBuffer(buffer, filePath, outMeshData);
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in LoadUSDFromDisk: %s - %s", filePath.c_str(), e.what());
            return false;
        }
    }

    // Enhanced gradient processing with comprehensive validation
    bool WriteGradientLineAsPNG(const std::vector<uint8_t>& buffer, const std::string& outPath) {
        try {
            // Validate inputs
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
            // Validate input
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

private:
    // CRITICAL: Add the missing convertMeshData method
    bool convertMeshData(const UsdProcessor::MeshData& processorMeshData,
                    AnariUsdMiddleware::MeshData& publicMeshData) {
        try {
            publicMeshData.elementName = processorMeshData.elementName;
            publicMeshData.typeName = processorMeshData.typeName;

            // FIXED: Convert glm::vec3 points to flat float array
            publicMeshData.points.clear();
            publicMeshData.points.reserve(processorMeshData.points.size() * 3);
            for (const auto& point : processorMeshData.points) {
                publicMeshData.points.push_back(point.x);
                publicMeshData.points.push_back(point.y);
                publicMeshData.points.push_back(point.z);
            }

            // Direct copy for indices (both are std::vector<uint32_t>)
            publicMeshData.indices = processorMeshData.indices;

            // FIXED: Convert glm::vec3 normals to flat float array
            publicMeshData.normals.clear();
            publicMeshData.normals.reserve(processorMeshData.normals.size() * 3);
            for (const auto& normal : processorMeshData.normals) {
                publicMeshData.normals.push_back(normal.x);
                publicMeshData.normals.push_back(normal.y);
                publicMeshData.normals.push_back(normal.z);
            }

            // FIXED: Convert glm::vec2 UVs to flat float array
            publicMeshData.uvs.clear();
            publicMeshData.uvs.reserve(processorMeshData.uvs.size() * 2);
            for (const auto& uv : processorMeshData.uvs) {
                publicMeshData.uvs.push_back(uv.x);
                publicMeshData.uvs.push_back(uv.y);
            }

            return publicMeshData.isValid();
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in convertMeshData: %s", e.what());
            return false;
        }
    }


    // Helper methods
    void cleanup() {
        try {
            stopReceiving();
            zmqConnector.disconnect();
            usdProcessor.reset();

            std::lock_guard<std::mutex> lock(callbackMutex);
            updateCallbacks.clear();
            messageCallbacks.clear();
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception during cleanup: %s", e.what());
        }
    }

    bool isJsonMessage(const std::string& message) {
        if (message.empty()) return false;
        std::string trimmed = message;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
        return (trimmed.front() == '{' && trimmed.back() == '}') ||
               (trimmed.front() == '[' && trimmed.back() == ']');
    }

    bool isXmlMessage(const std::string& message) {
        if (message.empty()) return false;
        std::string trimmed = message;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        return trimmed.find("<?xml") == 0 || trimmed.find("<") == 0;
    }

    bool validateFilePath(const std::string& filePath) {
        if (filePath.empty()) {
            MIDDLEWARE_LOG_ERROR("File path is empty");
            return false;
        }

        if (filePath.size() > 1000) {
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

    void logStatistics() {
        try {
            auto zmqStats = zmqConnector.getMessageStats();
            if (usdProcessor) {
                auto usdStats = usdProcessor->getProcessingStats();
                MIDDLEWARE_LOG_INFO("Middleware Statistics - ZMQ: %zu msgs, %zu files, %zu bytes | "
                                    "USD: %zu files, %zu meshes, %zu errors",
                                    static_cast<size_t>(zmqStats.totalMessagesReceived),
                                    static_cast<size_t>(zmqStats.totalFilesReceived),
                                    static_cast<size_t>(zmqStats.totalBytesReceived),
                                    static_cast<size_t>(usdStats.filesProcessed),
                                    static_cast<size_t>(usdStats.meshesExtracted),
                                    static_cast<size_t>(usdStats.processingErrors));
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

// Public interface implementations with enhanced error handling
AnariUsdMiddleware::AnariUsdMiddleware() : pImpl(std::make_unique<Impl>()) {
    MIDDLEWARE_LOG_INFO("AnariUsdMiddleware created with enhanced safety features");
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

AnariUsdMiddleware::TextureData AnariUsdMiddleware::CreateTextureFromBuffer(const std::vector<uint8_t>& buffer) {
    return pImpl->CreateTextureFromBuffer(buffer);
}

bool AnariUsdMiddleware::LoadUSDBuffer(const std::vector<uint8_t>& buffer, const std::string& fileName,
                                       std::vector<MeshData>& outMeshData) {
    return pImpl->LoadUSDBuffer(buffer, fileName, outMeshData);
}

bool AnariUsdMiddleware::LoadUSDFromDisk(const std::string& filePath,
                                         std::vector<MeshData>& outMeshData) {
    return pImpl->LoadUSDFromDisk(filePath, outMeshData);
}

bool AnariUsdMiddleware::WriteGradientLineAsPNG(const std::vector<uint8_t>& buffer, const std::string& outPath) {
    return pImpl->WriteGradientLineAsPNG(buffer, outPath);
}

bool AnariUsdMiddleware::GetGradientLineAsPNGBuffer(const std::vector<uint8_t>& buffer,
                                                    std::vector<uint8_t>& outPngBuffer) {
    return pImpl->GetGradientLineAsPNGBuffer(buffer, outPngBuffer);
}

std::string AnariUsdMiddleware::getStatusInfo() const {
    try {
        std::ostringstream status;
        status << "AnariUsdMiddleware Status:\n";
        status << "  Connected: " << (isConnected() ? "Yes" : "No") << "\n";
        return status.str();
    } catch (const std::exception& e) {
        return "Error getting status: " + std::string(e.what());
    }
}

} // namespace anari_usd_middleware
