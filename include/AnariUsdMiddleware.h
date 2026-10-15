#pragma once
// AnariUsdMiddleware - public C++ API (thread-safe, UE-friendly)

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <shared_mutex>

#include "MiddlewareLogging.h"
#include "CollisionProcessor.h"     // NEW: collision types / data
#include "AnariUsdMessages.h"       // FileInfo struct

#ifndef ANARI_USD_MIDDLEWARE_API
# ifdef _WIN32
#   ifdef ANARI_USD_MIDDLEWARE_EXPORTS
#     define ANARI_USD_MIDDLEWARE_API __declspec(dllexport)
#   else
#     define ANARI_USD_MIDDLEWARE_API __declspec(dllimport)
#   endif
# else
#   define ANARI_USD_MIDDLEWARE_API __attribute__((visibility("default")))
# endif
#endif

namespace anari_usd_middleware {

namespace detail {
    namespace safety = anari_usd_middleware::safety;
}

struct FileData {
    std::string              filename;
    std::vector<uint8_t>     data;
    std::string              hash;       // SHA-256 (hex)
    std::string              fileType;   // "USD", "IMAGE", ...
    bool isValid() const {
        return !filename.empty() &&
               !data.empty() &&
               data.size() <= detail::safety::MAX_BUFFER_SIZE &&
               !hash.empty() && !fileType.empty();
    }
    void clear() { filename.clear(); data.clear(); hash.clear(); fileType.clear(); }
};


struct MeshData {
    std::string          elementName;   // prim name
    std::string          typeName;      // prim type
    std::vector<float>   points;        // xyz…
    std::vector<uint32_t>indices;       // tris
    std::vector<float>   normals;       // xyz…
    std::vector<float>   uvs;           // uv…
    std::vector<float>   vertex_colors; // rgba…
    CollisionData        collision;
    std::string          subdivisionScheme;  // e.g., "catmull-clark", "bilinear", "none"
    bool                 doubleSided = false;
    std::vector<uint32_t>faceVertexCounts;  // For heterogenous polygons
    std::vector<std::vector<float>> uvSets; // Multiple UV sets (each as flat array)
    std::vector<std::string> uvSetNames;    // Names of UV sets

    bool isValid() const {
        return !elementName.empty() &&
               !points.empty() &&
               !indices.empty() &&
               points.size() % 3 == 0 &&
               indices.size() % 3 == 0 &&
               (normals.empty() || normals.size() % 3 == 0) &&
               (uvs.empty()     || uvs.size() % 2  == 0) &&
               (vertex_colors.empty() || vertex_colors.size() % 4 == 0);
    }
    size_t getVertexCount()   const { return points.size()   / 3; }
    size_t getTriangleCount() const { return indices.size()  / 3; }
    size_t getUVSetCount()    const { return uvSets.size(); }
    bool hasSubdivision()     const { return !subdivisionScheme.empty() && subdivisionScheme != "none"; }
    void clear() { *this = MeshData(); }
};

struct TextureData {
    int                    width{0}, height{0}, channels{0};
    std::vector<uint8_t>   data;
    bool isValid() const {
        return width>0 && height>0 && channels>0 &&
               data.size() == static_cast<size_t>(width*height*channels);
    }
    void clear() { *this = TextureData(); }
};

using FileUpdateCallback = std::function<void(const FileData&)>;
using MessageCallback    = std::function<void(const std::string&)>;

class ANARI_USD_MIDDLEWARE_API AnariUsdMiddleware {
public:
    AnariUsdMiddleware();
    ~AnariUsdMiddleware();
    AnariUsdMiddleware(const AnariUsdMiddleware&)            = delete;
    AnariUsdMiddleware& operator=(const AnariUsdMiddleware&) = delete;

    bool initialize(const char* endpoint = nullptr);
    void shutdown();
    bool isConnected() const;

    bool connectToBroker(const char* brokerEndpoint, int timeoutMs = 5000);
    void disconnectFromBroker();
    bool isBrokerConnected() const;

    bool requestFileList(int32_t targetRank, std::vector<std::string>& outFiles, int timeoutMs = 10000);
    bool requestFileListWithSizes(int32_t targetRank, std::vector<FileInfo>& outFiles, int timeoutMs = 10000);
    bool requestFile(const std::string& filename, int32_t targetRank,
                     std::vector<uint8_t>& outFileData, int timeoutMs = 30000);
    bool requestFileIntoBuffer(const std::string& filename, int32_t targetRank,
                               uint8_t* outBuffer, size_t outCapacity,
                               size_t& outSize, int& outStatus, int timeoutMs = 30000,
                               uint64_t abortHandle = 0);

    uint64_t createAbortToken();
    bool triggerAbortToken(uint64_t handle);
    void releaseAbortToken(uint64_t handle);
    bool requestFrame(int32_t frameNumber, int32_t targetRank,
                      std::vector<std::pair<std::string, std::vector<uint8_t>>>& outFrameFiles,
                      int timeoutMs = 60000);

    bool requestWorkerCount(uint32_t& outWorkerCount, int timeoutMs = 5000);
    bool requestTotalWorkerCount(uint32_t& outTotalCount, int timeoutMs = 5000);  // Includes rank 0
    bool requestWorkerStatus(int32_t targetRank,
                             std::vector<std::tuple<int32_t, uint32_t, std::string, std::string, uint64_t>>& outWorkerStatus,
                             int timeoutMs = 10000);

    using WorkerCountCallback = std::function<void(uint32_t workerCount)>;
    using WorkerStatusCallback = std::function<void(const std::vector<std::tuple<int32_t, uint32_t, std::string, std::string, uint64_t>>& workerStatus)>;
    using FileListCallback = std::function<void(const std::vector<std::string>& files)>;
    using FileListWithSizesCallback = std::function<void(const std::vector<FileInfo>& files)>;
    using BrokerErrorCallback = std::function<void(const std::string& error)>;

    void requestWorkerCountAsync(int timeoutMs, WorkerCountCallback callback, BrokerErrorCallback errorCallback = nullptr);
    void requestTotalWorkerCountAsync(int timeoutMs, WorkerCountCallback callback, BrokerErrorCallback errorCallback = nullptr);
    void requestWorkerStatusAsync(int32_t targetRank, int timeoutMs, WorkerStatusCallback callback, BrokerErrorCallback errorCallback = nullptr);
    void requestFileListAsync(int32_t targetRank, int timeoutMs, FileListCallback callback, BrokerErrorCallback errorCallback = nullptr);
    void requestFileListWithSizesAsync(int32_t targetRank, int timeoutMs, FileListWithSizesCallback callback, BrokerErrorCallback errorCallback = nullptr);

    void requestFilesParallelAsync(
        const std::vector<std::string>& filenames,
        const std::vector<int32_t>& targetRanks,
        int timeoutMs,
        std::function<void(const std::string&, const std::vector<uint8_t>&)> fileReceivedCallback,
        std::function<void()> completionCallback = nullptr,
        std::function<void(const std::string&, const std::string&)> errorCallback = nullptr);

    bool requestWorkerListString(std::vector<std::tuple<int32_t, std::string, std::string>>& outWorkers, int timeoutMs = 5000);

    int  registerUpdateCallback (FileUpdateCallback cb);
    void unregisterUpdateCallback(int id);
    int  registerMessageCallback(MessageCallback cb);
    void unregisterMessageCallback(int id);

    using NotificationCallback = std::function<void(uint32_t messageType,
                                                     int32_t sourceRank,
                                                     const std::string& filename,
                                                     uint64_t fileSize,
                                                     uint64_t timestamp,
                                                     uint64_t hashLo,       // hash128[0] of new data
                                                     uint64_t hashHi,       // hash128[1] of new data
                                                     uint64_t hashPrevLo,   // hashPrev128[0] of old data
                                                     uint64_t hashPrevHi,   // hashPrev128[1] of old data
                                                     bool hasOldData)>;     // true if hashPrev128 is valid
    void setNotificationCallback(NotificationCallback cb);

    using SceneUpdateCallback = std::function<void(uint32_t messageType,
                                                    int32_t sourceRank,
                                                    uint64_t timestamp,
                                                    uint64_t commitId,
                                                    uint64_t revision,
                                                    const std::string& primPath,
                                                    const std::string& propertyName,
                                                    int32_t changeType,
                                                    int32_t valueType,
                                                    int64_t intValue,
                                                    float floatValue,
                                                    const float* vec4,
                                                    const std::string& stringValue,
                                                    uint32_t payloadSize)>;
    void setSceneUpdateCallback(SceneUpdateCallback cb);

    using ProtocolDiagnosticsCallback = std::function<void(const std::string& event,
                                                            const std::string& message,
                                                            uint64_t value0,
                                                            uint64_t value1)>;
    void setProtocolDiagnosticsCallback(ProtocolDiagnosticsCallback cb);

    uint32_t getProtocolVersion() const;

    bool startReceiving();
    void stopReceiving();

    TextureData CreateTextureFromBuffer(const std::vector<uint8_t>& buffer);

    bool LoadUSDBuffer(const std::vector<uint8_t>& buffer,
                       const std::string& fileName,
                       std::vector<MeshData>& outMeshes);

    bool LoadUSDFromDisk(const std::string& filePath,
                         std::vector<MeshData>& outMeshes);

    bool LoadUSDBufferWithCollision(const std::vector<uint8_t>& buffer,
                                    const std::string& fileName,
                                    ECollisionComplexity complexity,
                                    std::vector<MeshData>& outMeshes);

    bool LoadUSDFromDiskWithCollision(const std::string& filePath,
                                      ECollisionComplexity complexity,
                                      std::vector<MeshData>& outMeshes);

    void  setDefaultCollisionComplexity(ECollisionComplexity cpx);
    ECollisionComplexity getDefaultCollisionComplexity() const;

    void  setCollisionParameters(float simplificationRatio,
                                 float convexHullPrecision,
                                 int   maxConvexHulls);

    bool WriteGradientLineAsPNG(const std::vector<uint8_t>& buffer,
                                const std::string& outPath);
    bool GetGradientLineAsPNGBuffer(const std::vector<uint8_t>& buffer,
                                    std::vector<uint8_t>& outPng);

    bool GetCachedGradientTexture(std::vector<uint8_t>& outData, int& outWidth, int& outHeight);

    std::string getStatusInfo() const;

    class AnariUsdClient* getClient() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;

    mutable std::shared_mutex statusMutex;
    std::atomic<bool> initialized{false};
    std::atomic<bool> shutdownRequested{false};
};

} // namespace anari_usd_middleware
