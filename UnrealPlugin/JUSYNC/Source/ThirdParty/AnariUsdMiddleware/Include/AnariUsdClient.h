#pragma once

#include "AnariUsdMessages.h"
#include "MiddlewareLogging.h"

#include <zmq.hpp>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <map>

// Platform detection
#if defined(_WIN32)
    #define PLATFORM_WINDOWS 1
    #define PLATFORM_LINUX 0
#elif defined(__linux__)
    #define PLATFORM_WINDOWS 0
    #define PLATFORM_LINUX 1
#else
    #define PLATFORM_WINDOWS 0
    #define PLATFORM_LINUX 0
#endif

namespace anari_usd_middleware {

// Forward declaration
class ParallelDownloadManager;

/**
 * ANARI USD ZMQ DEALER Client
 * Connects to ANARI USD broker to request files from HPC workers
 */
class AnariUsdClient {
public:
    // Connection status enumeration
    enum class ConnectionStatus {
        Disconnected,
        Connecting,
        Connected,
        ShuttingDown,
        Error
    };

    // File transfer callback types
    using FileChunkCallback = std::function<void(const std::string& filename, 
                                                  const std::vector<uint8_t>& chunk,
                                                  uint64_t offset, 
                                                  uint64_t totalSize)>;
    using FileCompleteCallback = std::function<void(const std::string& filename,
                                                    uint64_t totalSize)>;
    using FileListCallback = std::function<void(const std::vector<std::string>& files)>;
    using FileListWithSizesCallback = std::function<void(const std::vector<FileInfo>& files)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    
    // Worker status callback types
    using WorkerStatusCallback = std::function<void(int32_t rank,
                                                    uint32_t status,
                                                    const std::string& hostname,
                                                    const std::string& gpuInfo,
                                                    uint64_t lastHeartbeat)>;
    using WorkerCountCallback = std::function<void(uint32_t totalWorkers)>;

    // Connection statistics
    struct ConnectionStats {
        std::atomic<uint64_t> totalRequestsSent{0};
        std::atomic<uint64_t> totalResponsesReceived{0};
        std::atomic<uint64_t> totalBytesReceived{0};
        std::atomic<uint64_t> failedRequests{0};
        std::chrono::steady_clock::time_point lastActivityTime;

        struct Snapshot {
            uint64_t totalRequestsSent;
            uint64_t totalResponsesReceived;
            uint64_t totalBytesReceived;
            uint64_t failedRequests;
            std::chrono::steady_clock::time_point lastActivityTime;
        };

        Snapshot getSnapshot() const {
            return {
                totalRequestsSent.load(),
                totalResponsesReceived.load(),
                totalBytesReceived.load(),
                failedRequests.load(),
                lastActivityTime
            };
        }

        void reset() {
            totalRequestsSent.store(0);
            totalResponsesReceived.store(0);
            totalBytesReceived.store(0);
            failedRequests.store(0);
            lastActivityTime = std::chrono::steady_clock::now();
        }
    };

public:
    // Constructor and destructor
    AnariUsdClient();
    virtual ~AnariUsdClient();

    // Disable copy constructor and assignment operator
    AnariUsdClient(const AnariUsdClient&) = delete;
    AnariUsdClient& operator=(const AnariUsdClient&) = delete;

    // Enable move constructor and assignment operator
    AnariUsdClient(AnariUsdClient&&) = default;
    AnariUsdClient& operator=(AnariUsdClient&&) = default;

    // Core connection management
    bool connect(const char* brokerEndpoint, int timeoutMs = 5000);
    void disconnect(int gracefulTimeoutMs = 1000);
    bool isConnected() const;
    ConnectionStatus getConnectionStatus() const;

    // File request methods
    bool requestFileList(int32_t targetRank, FileListCallback callback, int timeoutMs = 10000);
    bool requestFileListWithSizes(int32_t targetRank, FileListWithSizesCallback callback, int timeoutMs = 10000);
    bool requestFile(const std::string& filename, int32_t targetRank,
                     FileChunkCallback chunkCallback,
                     FileCompleteCallback completeCallback,
                     ErrorCallback errorCallback = nullptr,
                     int timeoutMs = 30000);
    bool requestFrame(int32_t frameNumber, int32_t targetRank,
                      FileChunkCallback chunkCallback,
                      FileCompleteCallback completeCallback,
                      ErrorCallback errorCallback = nullptr,
                      int timeoutMs = 60000);

    // Synchronous file request (blocks until complete)
    bool getFileSync(const std::string& filename, int32_t targetRank,
                     std::vector<uint8_t>& fileData, int timeoutMs = 30000);
    bool getFileListSync(int32_t targetRank, std::vector<std::string>& files, int timeoutMs = 10000);
    bool getFileListWithSizesSync(int32_t targetRank, std::vector<FileInfo>& files, int timeoutMs = 10000);

    // Worker status queries
    bool requestWorkerStatus(int32_t targetRank, WorkerStatusCallback callback, int timeoutMs = 5000);
    bool requestWorkerCount(WorkerCountCallback callback, int timeoutMs = 5000);
    
    // String-based worker list (compatible with Python broker)
    bool requestWorkerListString(std::vector<std::tuple<int32_t, std::string, std::string>>& outWorkers, int timeoutMs = 5000);
    
    // Synchronous worker status queries
    bool getWorkerStatusSync(int32_t targetRank,
                             std::vector<std::tuple<int32_t, uint32_t, std::string, std::string, uint64_t>>& workers,
                             int timeoutMs = 10000);
    bool getWorkerCountSync(uint32_t& workerCount, int timeoutMs = 5000);
    
    // Total worker count including rank 0 (uses legacy string protocol)
    bool getTotalWorkerCountSync(uint32_t& totalCount, int timeoutMs = 5000);

    // Statistics and monitoring
    ConnectionStats::Snapshot getConnectionStats() const;
    void resetConnectionStats();
    void setMaxMessageSize(size_t maxSizeBytes);
    size_t getMaxMessageSize() const;

    // Connection testing
    bool testConnection();
    void updateHealthStatus();

    // Parallel download support
    zmq::socket_t* getSocket() { return zmqSocket.get(); }
    const zmq::socket_t* getSocket() const { return zmqSocket.get(); }
    
    // Parallel file requests
    bool requestFilesParallel(
        const std::vector<std::string>& filenames,
        const std::vector<int32_t>& target_ranks,
        std::function<void(const std::string&, const std::vector<uint8_t>&)> spawn_callback,
        std::function<void()> completion_callback = nullptr,
        std::function<void(const std::string&, const std::string&)> error_callback = nullptr,
        int timeout_ms = 30000);

private:
    // Connection management helpers
    bool configureSocket(int timeoutMs);
    bool validateEndpoint(const std::string& endpoint) const;
    void cleanup();

    // Message sending helpers
    bool sendRequest(const void* data, size_t size, uint32_t requestId);
    bool receiveResponse(void* buffer, size_t size, int timeoutMs);

    // Response handling
    bool handleFileChunkResponse(const ZmqFileChunk& chunk,
                                  const std::vector<uint8_t>& chunkData,
                                  FileChunkCallback chunkCallback);
    bool handleFileCompleteResponse(const ZmqFileComplete& complete,
                                     FileCompleteCallback completeCallback);
    bool handleFileListResponse(const ZmqFileListResponse& list,
                                 const std::vector<uint8_t>& data,
                                 FileListCallback callback);
    bool handleFileListWithSizesResponse(const ZmqFileListResponse& list,
                                           const std::vector<uint8_t>& data,
                                           FileListWithSizesCallback callback);
    bool handleErrorResponse(const ZmqErrorResponse& error,
                             ErrorCallback errorCallback);

    // Request ID management
    uint32_t generateRequestId();
    bool waitForResponse(uint32_t requestId, int timeoutMs);

    // Platform-specific configuration
#if PLATFORM_WINDOWS
    bool configureWindowsSocket();
#elif PLATFORM_LINUX
    bool configureLinuxSocket();
#endif

    // Member variables
    std::unique_ptr<zmq::context_t> zmqContext;
    std::unique_ptr<zmq::socket_t> zmqSocket;
    std::unique_ptr<ParallelDownloadManager> parallelDownloadManager;

    // Connection state
    std::atomic<ConnectionStatus> connectionStatus{ConnectionStatus::Disconnected};
    std::atomic<bool> shutdownRequested{false};
    std::string brokerEndpoint;

    // Thread safety
    mutable std::mutex connectionMutex;
    mutable std::recursive_mutex requestMutex;

    // Request tracking
    std::atomic<uint32_t> nextRequestId{1};
    std::map<uint32_t, bool> pendingRequests;

    // Statistics and monitoring
    ConnectionStats connectionStats;
    std::atomic<size_t> maxMessageSize{104857600}; // 100MB default
    std::chrono::steady_clock::time_point lastHealthCheck;

    // Default chunk size for file requests
    static constexpr uint32_t DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024; // 4MB
};

} // namespace anari_usd_middleware