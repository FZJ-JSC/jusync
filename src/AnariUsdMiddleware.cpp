#include "AnariUsdMiddleware.h"
#include "ZmqConnector.h"
#include "HashVerifier.h"
#include "UsdProcessor.h"
#include "MiddlewareLogging.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <chrono>
#include <filesystem>
#include <fstream>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <memory>
#include <iostream>
#include "stb_image_write.h"

namespace anari_usd_middleware {

class AnariUsdMiddleware::Impl {
public:
    Impl() : nextCallbackId(1), running(false) {
        MIDDLEWARE_LOG_INFO("AnariUsdMiddleware::Impl created");
    }

    ~Impl() {
        MIDDLEWARE_LOG_INFO("AnariUsdMiddleware::Impl destroyed");
        stopReceiving();
        zmqConnector.disconnect();
    }

    bool initialize(const char* endpoint) {
        MIDDLEWARE_LOG_INFO("Initializing AnariUsdMiddleware...");
        // Initialize USD processor
        try {
            usdProcessor = std::make_unique<UsdProcessor>();
            MIDDLEWARE_LOG_INFO("USD processor initialized");
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Failed to initialize USD processor: %s", e.what());
            return false;
        }

        // Initialize ZMQ connection
        bool result = zmqConnector.initialize(endpoint);
        if (result) {
            MIDDLEWARE_LOG_INFO("ZMQ connector initialized successfully");
        } else {
            MIDDLEWARE_LOG_ERROR("Failed to initialize ZMQ connector");
        }

        return result;
    }

    void shutdown() {
        MIDDLEWARE_LOG_INFO("Shutting down AnariUsdMiddleware...");
        stopReceiving();
        zmqConnector.disconnect();
        MIDDLEWARE_LOG_INFO("AnariUsdMiddleware shutdown complete");
    }

    bool isConnected() const {
        return zmqConnector.isConnected();
    }

    int registerUpdateCallback(AnariUsdMiddleware::FileUpdateCallback callback) {
        if (!callback) {
            MIDDLEWARE_LOG_ERROR("Attempted to register null file callback");
            return -1;
        }

        std::lock_guard<std::mutex> lock(callbackMutex);
        int callbackId = nextCallbackId++;
        updateCallbacks[callbackId] = callback;
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

        std::lock_guard<std::mutex> lock(callbackMutex);
        int callbackId = nextCallbackId++;
        messageCallbacks[callbackId] = callback;
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
        if (running) {
            MIDDLEWARE_LOG_INFO("Receiver thread already running");
            return true;
        }

        if (!zmqConnector.isConnected()) {
            MIDDLEWARE_LOG_ERROR("Cannot start receiver thread: ZMQ connector not connected");
            return false;
        }

        try {
            running = true;
            receiverThread = std::thread(&Impl::receiverLoop, this);
            MIDDLEWARE_LOG_INFO("Receiver thread started");
            return true;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Failed to start receiver thread: %s", e.what());
            running = false;
            return false;
        }
    }

    void stopReceiving() {
        if (!running) {
            return;
        }

        MIDDLEWARE_LOG_INFO("Stopping receiver thread...");
        running = false;
        // Give the thread time to observe the flag
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (receiverThread.joinable()) {
            receiverThread.join();
            MIDDLEWARE_LOG_INFO("Receiver thread joined");
        }
    }

    void receiverLoop() {
        MIDDLEWARE_LOG_INFO("Receiver thread started");
        while (running) {
            // Use polling with timeout instead of blocking receive
            zmq::pollitem_t items[] = {
                { zmqConnector.getSocket(), 0, ZMQ_POLLIN, 0 }
            };

            zmq::poll(items, 1, std::chrono::milliseconds(100));

            if (items[0].revents & ZMQ_POLLIN) {
                // Socket has data available
                // First try to receive as a file (multi-part message)
                AnariUsdMiddleware::FileData fileData;
                if (zmqConnector.receiveFile(fileData.filename, fileData.data, fileData.hash)) {
                    MIDDLEWARE_LOG_INFO("Received file: %s (size: %zu bytes, hash: %s)",
                        fileData.filename.c_str(), fileData.data.size(), fileData.hash.c_str());

                    // Check file type for logging
                    std::string extension = "";
                    size_t dotPos = fileData.filename.find_last_of('.');
                    if (dotPos != std::string::npos) {
                        extension = fileData.filename.substr(dotPos);
                    }

                    MIDDLEWARE_LOG_INFO("File extension: %s", extension.c_str());
                    // Preview file content for debugging
                    if (fileData.data.size() > 0) {
                        size_t previewSize = std::min(fileData.data.size(), size_t(50));
                        std::string preview(reinterpret_cast<const char*>(fileData.data.data()),
                            previewSize);
                        MIDDLEWARE_LOG_INFO("Content preview: %s%s",
                            preview.c_str(),
                            fileData.data.size() > previewSize ? "..." : "");
                    }

                    // Verify hash
                    if (HashVerifier::verifyHash(fileData.data, fileData.hash)) {
                        MIDDLEWARE_LOG_INFO("Hash verification succeeded for file: %s",
                            fileData.filename.c_str());

                        // Determine file type
                        std::string fileType;
                        if (fileData.filename.find(".usda") != std::string::npos ||
                            fileData.filename.find(".usd") != std::string::npos) {
                            fileType = "USD";
                        }
                        else if (fileData.filename.find(".png") != std::string::npos ||
                            fileData.filename.find(".jpg") != std::string::npos) {
                            fileType = "IMAGE";
                        }
                        else {
                            fileType = "UNKNOWN";
                        }

                        // Set file type in the data structure
                        fileData.fileType = fileType;

                        // Notify callbacks
                        std::lock_guard<std::mutex> lock(callbackMutex);
                        for (const auto& pair : updateCallbacks) {
                            try {
                                pair.second(fileData);
                            } catch (const std::exception& e) {
                                MIDDLEWARE_LOG_ERROR("Exception in file callback (ID: %d): %s",
                                    pair.first, e.what());
                            }
                        }
                    } else {
                        MIDDLEWARE_LOG_ERROR("Hash verification failed for file: %s",
                            fileData.filename.c_str());
                        // Calculate the hash we would expect to see
                        std::string calculatedHash = HashVerifier::calculateHash(fileData.data);
                        MIDDLEWARE_LOG_ERROR("Calculated hash: %s, Received hash: %s",
                            calculatedHash.c_str(), fileData.hash.c_str());
                    }

                    continue;
                }

                // If not a file, try to receive as a generic message
                if (zmqConnector.receiveAnyMessage()) {
                    const std::string& message = zmqConnector.getLastReceivedMessage();
                    MIDDLEWARE_LOG_INFO("Received message: %s", message.c_str());

                    // Try to parse as JSON if it looks like JSON
                    if ((message.find('{') == 0 && message.rfind('}') == message.length() - 1) ||
                        (message.find('[') == 0 && message.rfind(']') == message.length() - 1)) {
                        MIDDLEWARE_LOG_INFO("Message appears to be JSON format");
                    } else {
                        MIDDLEWARE_LOG_INFO("Message is plain text");
                    }

                    // Notify message callbacks
                    std::lock_guard<std::mutex> lock(callbackMutex);
                    for (const auto& pair : messageCallbacks) {
                        try {
                            pair.second(message);
                        } catch (const std::exception& e) {
                            MIDDLEWARE_LOG_ERROR("Exception in message callback (ID: %d): %s",
                                pair.first, e.what());
                        }
                    }
                } else {
                    MIDDLEWARE_LOG_WARNING("Failed to process incoming message");
                }
            }

            // Small sleep to prevent CPU spinning when no messages
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        MIDDLEWARE_LOG_INFO("Receiver thread stopped");
    }

    // USD Processing methods
    bool ConvertUSDtoUSDC(const std::string& inputFilePath, const std::string& outputFilePath) {
        if (!usdProcessor) {
            MIDDLEWARE_LOG_ERROR("USD processor not initialized");
            return false;
        }

        try {
            return usdProcessor->ConvertUSDtoUSDC(inputFilePath, outputFilePath);
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in ConvertUSDtoUSDC: %s", e.what());
            return false;
        }
    }

    AnariUsdMiddleware::TextureData CreateTextureFromBuffer(const std::vector<uint8_t>& buffer) {
        if (!usdProcessor) {
            MIDDLEWARE_LOG_ERROR("USD processor not initialized");
            return AnariUsdMiddleware::TextureData();
        }

        try {
            UsdProcessor::TextureData texData = usdProcessor->CreateTextureFromBuffer(buffer);
            // Convert to public API structure
            AnariUsdMiddleware::TextureData result;
            result.width = texData.width;
            result.height = texData.height;
            result.channels = texData.channels;
            result.data = std::move(texData.data);
            return result;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in CreateTextureFromBuffer: %s", e.what());
            return AnariUsdMiddleware::TextureData();
        }
    }

    bool LoadUSDBuffer(const std::vector<uint8_t>& buffer, const std::string& fileName,
        std::vector<AnariUsdMiddleware::MeshData>& outMeshData) {
        if (!usdProcessor) {
            MIDDLEWARE_LOG_ERROR("USD processor not initialized");
            return false;
        }

        try {
            std::vector<UsdProcessor::MeshData> processorMeshData;
            bool result = usdProcessor->LoadUSDBuffer(buffer, fileName, processorMeshData);

            if (result) {
                // Convert to public API structure
                outMeshData.clear();
                outMeshData.reserve(processorMeshData.size());

                for (const auto& meshData : processorMeshData) {
                    AnariUsdMiddleware::MeshData publicMeshData;
                    publicMeshData.elementName = meshData.elementName;
                    publicMeshData.typeName = meshData.typeName;

                    // Convert points
                    publicMeshData.points.reserve(meshData.points.size() * 3);
                    for (const auto& point : meshData.points) {
                        publicMeshData.points.push_back(point.x);
                        publicMeshData.points.push_back(point.y);
                        publicMeshData.points.push_back(point.z);
                    }

                    // Copy indices
                    publicMeshData.indices = meshData.indices;

                    // Convert normals
                    publicMeshData.normals.reserve(meshData.normals.size() * 3);
                    for (const auto& normal : meshData.normals) {
                        publicMeshData.normals.push_back(normal.x);
                        publicMeshData.normals.push_back(normal.y);
                        publicMeshData.normals.push_back(normal.z);
                    }

                    // Convert UVs
                    publicMeshData.uvs.reserve(meshData.uvs.size() * 2);
                    for (const auto& uv : meshData.uvs) {
                        publicMeshData.uvs.push_back(uv.x);
                        publicMeshData.uvs.push_back(uv.y);
                    }

                    outMeshData.push_back(std::move(publicMeshData));
                }
            }

            return result;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in LoadUSDBuffer: %s", e.what());
            return false;
        }
    }

private:
    ZmqConnector zmqConnector;
    std::map<int, AnariUsdMiddleware::FileUpdateCallback> updateCallbacks;
    std::map<int, AnariUsdMiddleware::MessageCallback> messageCallbacks;
    std::mutex callbackMutex;
    int nextCallbackId;
    std::thread receiverThread;
    std::atomic<bool> running;
    std::unique_ptr<UsdProcessor> usdProcessor;
};

// Public interface implementations
AnariUsdMiddleware::AnariUsdMiddleware() : pImpl(new Impl()) {
    MIDDLEWARE_LOG_INFO("AnariUsdMiddleware created");
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

bool AnariUsdMiddleware::ConvertUSDtoUSDC(const std::string& inputFilePath, const std::string& outputFilePath) {
    return pImpl->ConvertUSDtoUSDC(inputFilePath, outputFilePath);
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
    MIDDLEWARE_LOG_INFO("Loading USD from disk: %s", filePath.c_str());

    // 1. Verify file existence
    if (!std::filesystem::exists(filePath)) {
        MIDDLEWARE_LOG_ERROR("File not found: %s", filePath.c_str());
        return false;
    }

    // 2. Read file into buffer
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        MIDDLEWARE_LOG_ERROR("Failed to open file: %s", filePath.c_str());
        return false;
    }

    std::vector<uint8_t> buffer;
    try {
        size_t fileSize = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);
        buffer.resize(fileSize);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
            MIDDLEWARE_LOG_ERROR("Failed to read file contents: %s", filePath.c_str());
            return false;
        }
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("File read error: %s - %s", filePath.c_str(), e.what());
        return false;
    }

    // 3. Use existing buffer processing with the FULL PATH
    return LoadUSDBuffer(buffer, filePath, outMeshData);
}

bool AnariUsdMiddleware::WriteGradientLineAsPNG(const std::vector<uint8_t>& buffer, const std::string& outPath) {
    // Use existing logic to process the buffer
    TextureData texData = CreateTextureFromBuffer(buffer);

    // Only proceed if we have a valid 1-row texture
    if (texData.data.empty() || texData.width <= 0 || texData.height != 1) {
        MIDDLEWARE_LOG_ERROR("Gradient line PNG: No valid gradient data to write for %s", outPath.c_str());
        return false;
    }

    // Write as PNG (RGBA, 4 channels)
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

    MIDDLEWARE_LOG_INFO("Gradient line PNG written to %s", outPath.c_str());
    return true;
}

// Helper for writing PNG to memory buffer
static void writeToVector(void* context, void* data, int size) {
    auto* vec = reinterpret_cast<std::vector<uint8_t>*>(context);
    uint8_t* bytes = reinterpret_cast<uint8_t*>(data);
    vec->insert(vec->end(), bytes, bytes + size);
}

bool AnariUsdMiddleware::GetGradientLineAsPNGBuffer(
    const std::vector<uint8_t>& buffer, std::vector<uint8_t>& outPngBuffer) {

    TextureData texData = CreateTextureFromBuffer(buffer);
    if (texData.data.empty() || texData.width <= 0 || texData.height != 1) {
        MIDDLEWARE_LOG_ERROR("No valid gradient data to encode");
        return false;
    }

    outPngBuffer.clear();
    int stride = texData.width * texData.channels;
    int ok = stbi_write_png_to_func(
        writeToVector, &outPngBuffer,
        texData.width, texData.height, texData.channels,
        texData.data.data(), stride
    );

    if (!ok) {
        MIDDLEWARE_LOG_ERROR("Failed to encode PNG to memory buffer");
        return false;
    }

    MIDDLEWARE_LOG_INFO("Gradient line encoded as PNG buffer, size: %zu bytes", outPngBuffer.size());
    return true;
}

}
