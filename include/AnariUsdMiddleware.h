#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>
#include "MiddlewareLogging.h"

#ifndef ANARI_USD_MIDDLEWARE_API
#ifdef _WIN32
#define ANARI_USD_MIDDLEWARE_API __declspec(dllexport)
#else
#define ANARI_USD_MIDDLEWARE_API __attribute__((visibility("default")))
#endif
#endif

namespace anari_usd_middleware {

/**
 * Main middleware interface class that provides a clean API for client applications
 * to connect to ANARI-USD data streams.
 */
class ANARI_USD_MIDDLEWARE_API AnariUsdMiddleware {
public:
    // File data structure
    struct FileData {
        std::string filename;
        std::vector<uint8_t> data;
        std::string hash;
        std::string fileType;
    };

    // Mesh data structure
    struct MeshData {
        std::string elementName;
        std::string typeName;
        std::vector<float> points;
        std::vector<uint32_t> indices;
        std::vector<float> normals;
        std::vector<float> uvs;
    };

    // Texture data structure
    struct TextureData {
        int width = 0;
        int height = 0;
        int channels = 0;
        std::vector<uint8_t> data;
    };

    // Callback types
    using FileUpdateCallback = std::function<void(const FileData&)>;
    using MessageCallback = std::function<void(const std::string&)>;

    AnariUsdMiddleware();
    ~AnariUsdMiddleware();

    /**
     * Initialize the middleware
     * @param endpoint The ZeroMQ endpoint to bind to (e.g., "tcp://*:13456")
     * @return True if initialization was successful, false otherwise
     */
    bool initialize(const char* endpoint = nullptr);

    /**
     * Shutdown the middleware
     */
    void shutdown();

    /**
     * Check connection status
     * @return True if connected, false otherwise
     */
    bool isConnected() const;

    /**
     * Register a callback to be notified when a file is received
     * @param callback The callback function to register
     * @return A unique identifier for the callback
     */
    int registerUpdateCallback(FileUpdateCallback callback);

    /**
     * Unregister a previously registered callback
     * @param callbackId The identifier of the callback to unregister
     */
    void unregisterUpdateCallback(int callbackId);

    /**
     * Register a callback to be notified when a message is received
     * @param callback The callback function to register
     * @return A unique identifier for the callback
     */
    int registerMessageCallback(MessageCallback callback);

    /**
     * Unregister a previously registered message callback
     * @param callbackId The identifier of the callback to unregister
     */
    void unregisterMessageCallback(int callbackId);

    /**
     * Start receiving data (non-blocking)
     * @return True if the receiver thread was started successfully, false otherwise
     */
    bool startReceiving();

    /**
     * Stop receiving data
     */
    void stopReceiving();

    /**
     * Convert USD file to USDC format
     * @param inputFilePath Path to the input USD file
     * @param outputFilePath Path where the USDC file should be written
     * @return True if conversion was successful, false otherwise
     */
    bool ConvertUSDtoUSDC(const std::string& inputFilePath, const std::string& outputFilePath);

    /**
     * Create texture from raw buffer data
     * @param buffer Raw image data buffer
     * @return TextureData structure containing the processed texture
     */
    TextureData CreateTextureFromBuffer(const std::vector<uint8_t>& buffer);

    /**
     * Load USD data from buffer
     * @param buffer Raw USD data buffer
     * @param fileName Original filename (used for format detection)
     * @param outMeshData Output vector to store the extracted mesh data
     * @return True if loading was successful, false otherwise
     */
    bool LoadUSDBuffer(const std::vector<uint8_t>& buffer, const std::string& fileName,
                      std::vector<MeshData>& outMeshData);

    bool WriteGradientLineAsPNG(const std::vector<uint8_t>& buffer, const std::string& outPath);

    /**
     * Extract the gradient line from an image buffer and return the PNG as a memory buffer.
     * @param buffer Raw image data buffer (input)
     * @param outPngBuffer Output vector to receive PNG bytes (output)
     * @return True if successful, false otherwise.
     */
    bool GetGradientLineAsPNGBuffer(const std::vector<uint8_t>& buffer, std::vector<uint8_t>& outPngBuffer);
    
    /**
    * Load USD data directly from disk
    * @param filePath Path to the USD file
    * @param outMeshData Output vector to store the extracted mesh data
    * @return True if loading was successful, false otherwise
    */
    bool LoadUSDFromDisk(const std::string& filePath, std::vector<MeshData>& outMeshData);

    

private:
    // Internal implementation
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} 
