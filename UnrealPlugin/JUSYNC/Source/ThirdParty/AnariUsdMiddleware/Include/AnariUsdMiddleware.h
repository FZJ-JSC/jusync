#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <shared_mutex>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>

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

/**
 * Thread-safe main middleware interface class that provides a clean API for client applications
 * to connect to ANARI-USD data streams with comprehensive error handling and memory safety.
 */
class ANARI_USD_MIDDLEWARE_API AnariUsdMiddleware {
public:
    // File data structure with validation
    struct FileData {
        std::string filename;
        std::vector<uint8_t> data;
        std::string hash;
        std::string fileType;

        // Validation method
        bool isValid() const {
            return !filename.empty() &&
                   !data.empty() &&
                   data.size() <= safety::MAX_BUFFER_SIZE &&
                   !hash.empty() &&
                   !fileType.empty();
        }

        // Clear all data safely
        void clear() {
            filename.clear();
            data.clear();
            hash.clear();
            fileType.clear();
        }
    };

    // Mesh data structure with bounds checking (RealtimeMesh ready)
    struct MeshData {
        std::string elementName;
        std::string typeName;
        std::vector<float> points;    // Flat array: [x1,y1,z1, x2,y2,z2, ...]
        std::vector<uint32_t> indices; // Triangle indices
        std::vector<float> normals;   // Flat array: [nx1,ny1,nz1, nx2,ny2,nz2, ...]
        std::vector<float> uvs;       // Flat array: [u1,v1, u2,v2, ...]

        // Validation method
        bool isValid() const {
            return !elementName.empty() &&
                   points.size() <= safety::MAX_MESH_VERTICES * 3 &&
                   indices.size() <= safety::MAX_MESH_INDICES &&
                   normals.size() <= safety::MAX_MESH_VERTICES * 3 &&
                   uvs.size() <= safety::MAX_MESH_VERTICES * 2 &&
                   (points.size() % 3 == 0) &&
                   (normals.empty() || normals.size() % 3 == 0) &&
                   (uvs.empty() || uvs.size() % 2 == 0);
        }

        size_t getVertexCount() const {
            return points.size() / 3;
        }

        size_t getTriangleCount() const {
            return indices.size() / 3;
        }

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

    // Texture data structure with validation
    struct TextureData {
        int width = 0;
        int height = 0;
        int channels = 0;
        std::vector<uint8_t> data;

        // Validation method
        bool isValid() const {
            return width > 0 && height > 0 &&
                   channels > 0 && channels <= 4 &&
                   !data.empty() &&
                   data.size() == static_cast<size_t>(width * height * channels);
        }

        size_t getExpectedDataSize() const {
            return static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels);
        }

        void clear() {
            width = height = channels = 0;
            data.clear();
        }
    };

    // Safe callback types with exception handling
    using FileUpdateCallback = std::function<void(const FileData&)>;
    using MessageCallback = std::function<void(const std::string&)>;

    // Constructor and destructor
    AnariUsdMiddleware();
    ~AnariUsdMiddleware();

    // Disable copy constructor and assignment operator for safety
    AnariUsdMiddleware(const AnariUsdMiddleware&) = delete;
    AnariUsdMiddleware& operator=(const AnariUsdMiddleware&) = delete;

    /**
     * Initialize the middleware with enhanced error checking
     * @param endpoint The ZeroMQ endpoint to bind to (e.g., "tcp://*:13456")
     * @return True if initialization was successful, false otherwise
     */
    bool initialize(const char* endpoint = nullptr);

    /**
     * Shutdown the middleware safely
     */
    void shutdown();

    /**
     * Check connection status thread-safely
     * @return True if connected, false otherwise
     */
    bool isConnected() const;

    /**
     * Register a callback to be notified when a file is received (thread-safe)
     * @param callback The callback function to register
     * @return A unique identifier for the callback, -1 on failure
     */
    int registerUpdateCallback(FileUpdateCallback callback);

    /**
     * Unregister a previously registered callback (thread-safe)
     * @param callbackId The identifier of the callback to unregister
     */
    void unregisterUpdateCallback(int callbackId);

    /**
     * Register a callback to be notified when a message is received (thread-safe)
     * @param callback The callback function to register
     * @return A unique identifier for the callback, -1 on failure
     */
    int registerMessageCallback(MessageCallback callback);

    /**
     * Unregister a previously registered message callback (thread-safe)
     * @param callbackId The identifier of the callback to unregister
     */
    void unregisterMessageCallback(int callbackId);

    /**
     * Start receiving data (non-blocking, thread-safe)
     * @return True if the receiver thread was started successfully, false otherwise
     */
    bool startReceiving();

    /**
     * Stop receiving data (thread-safe)
     */
    void stopReceiving();

    /**
     * Create texture from raw buffer data with bounds checking
     * @param buffer Raw image data buffer
     * @return TextureData structure containing the processed texture
     */
    TextureData CreateTextureFromBuffer(const std::vector<uint8_t>& buffer);

    /**
     * Load USD data from buffer with comprehensive validation (RealtimeMesh ready)
     * @param buffer Raw USD data buffer
     * @param fileName Original filename (used for format detection)
     * @param outMeshData Output vector to store the extracted mesh data
     * @return True if loading was successful, false otherwise
     */
    bool LoadUSDBuffer(const std::vector<uint8_t>& buffer, const std::string& fileName,
                       std::vector<MeshData>& outMeshData);

    /**
     * Load USD data directly from disk with file validation (RealtimeMesh ready)
     * @param filePath Path to the USD file
     * @param outMeshData Output vector to store the extracted mesh data
     * @return True if loading was successful, false otherwise
     */
    bool LoadUSDFromDisk(const std::string& filePath, std::vector<MeshData>& outMeshData);

    /**
     * Write gradient line as PNG with error handling
     * @param buffer Raw image data buffer
     * @param outPath Output file path
     * @return True if successful, false otherwise
     */
    bool WriteGradientLineAsPNG(const std::vector<uint8_t>& buffer, const std::string& outPath);

    /**
     * Extract the gradient line from an image buffer and return the PNG as a memory buffer
     * @param buffer Raw image data buffer (input)
     * @param outPngBuffer Output vector to receive PNG bytes (output)
     * @return True if successful, false otherwise
     */
    bool GetGradientLineAsPNGBuffer(const std::vector<uint8_t>& buffer, std::vector<uint8_t>& outPngBuffer);

    /**
     * Get current status information for debugging
     * @return Status string with connection and processing information
     */
    std::string getStatusInfo() const;

private:
    // Forward declaration of implementation class (Pimpl idiom)
    class Impl;
    std::unique_ptr<Impl> pImpl;

    // Thread safety for public interface
    mutable std::shared_mutex statusMutex;
    std::atomic<bool> initialized{false};
    std::atomic<bool> shutdownRequested{false};
};

} // namespace anari_usd_middleware
