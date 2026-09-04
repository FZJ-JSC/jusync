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
#include <deque>
#include <unordered_map>
#include <condition_variable>
#include <thread>

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

    // File transfer callback types.
    // chunk/chunkSize point into a buffer owned by the request thread for the
    // duration of the callback — copy or consume it inside the callback
    // (no hidden copy is made for you).
    using FileChunkCallback = std::function<void(const std::string& filename,
                                                  const uint8_t* chunk,
                                                  size_t chunkSize,
                                                  uint64_t offset,
                                                  uint64_t totalSize)>;
    using FileCompleteCallback = std::function<void(const std::string& filename,
                                                    uint64_t totalSize)>;
    using FileListCallback = std::function<void(const std::vector<std::string>& files)>;
    using FileListWithSizesCallback = std::function<void(const std::vector<FileInfo>& files)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    
    // Notification callback types (for live update support, V2-aware)
    using NotificationCallback = std::function<void(uint32_t messageType,
                                                      int32_t sourceRank,
                                                      const std::string& filename,
                                                      uint64_t fileSize,
                                                      uint64_t timestamp,
                                                      uint64_t hashLo,
                                                      uint64_t hashHi,
                                                      uint64_t hashPrevLo,
                                                      uint64_t hashPrevHi,
                                                      bool hasOldData)>;

    // Typed scene / property update callback (NOTIFY_SCENE_UPDATE / NOTIFY_PROPERTY_UPDATE)
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

    // Lightweight protocol diagnostics callback
    using ProtocolDiagnosticsCallback = std::function<void(const std::string& event,
                                                            const std::string& message,
                                                            uint64_t value0,
                                                            uint64_t value1)>;

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
    bool isShutdownRequested() const { return shutdownRequested.load(); }

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

    // Notification callbacks (live update support)
    void setNotificationCallback(NotificationCallback callback);
    void setSceneUpdateCallback(SceneUpdateCallback callback);
    void setProtocolDiagnosticsCallback(ProtocolDiagnosticsCallback callback);
    uint32_t getProtocolVersion() const { return ANARI_USD_PROTOCOL_VERSION; }

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

    // Async file request — fire-and-forget, never blocks calling thread.
    // Callbacks are invoked from the dispatcher thread when responses arrive.
    bool requestFileAsync(const std::string& filename, int32_t targetRank,
                           FileChunkCallback chunkCallback,
                           FileCompleteCallback completeCallback,
                           ErrorCallback errorCallback = nullptr,
                           int timeoutMs = 30000);

private:
    // Connection management helpers
    bool configureSocket(int timeoutMs);
    bool validateEndpoint(const std::string& endpoint) const;
    void cleanup();

    // Message sending helpers
    bool sendRequest(const void* data, size_t size, uint32_t requestId);

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

    // Platform-specific configuration
#if PLATFORM_WINDOWS
    bool configureWindowsSocket();
#elif PLATFORM_LINUX
    bool configureLinuxSocket();
#endif

    // Member variables
    std::unique_ptr<zmq::context_t> zmqContext;
    std::unique_ptr<zmq::socket_t> zmqSocket;

    // Connection state
    std::atomic<ConnectionStatus> connectionStatus{ConnectionStatus::Disconnected};
    std::atomic<bool> shutdownRequested{false};
    std::string brokerEndpoint;

    // Thread safety
    mutable std::mutex connectionMutex;
    mutable std::recursive_mutex requestMutex;

    // Serialize ALL recv operations on the ZMQ DEALER socket.
    // ZMQ is NOT safe for concurrent recv on the same socket.
    mutable std::mutex recvMutex;

    // Request tracking
    std::atomic<uint32_t> nextRequestId{1};
    std::map<uint32_t, bool> pendingRequests;

    // Zero-copy frame handle: owns the ZMQ message buffer pulled by the
    // dispatcher; consumers read via data()/size() without any memcpy.
    using FrameMsg = std::shared_ptr<zmq::message_t>;

    // Incoming frame pair pulled by the dispatcher: <delimiterFrame, dataFrame>.
    using FramePair = std::pair<FrameMsg, FrameMsg>;
    using FramePairPtr = std::shared_ptr<FramePair>;

    // Out-of-order response buffering for multi-threaded safety.
    // A dedicated dispatcher thread continuously pulls frame pairs from the ZMQ
    // socket and indexes them by request_id (O(1) lookup for requesters).
    // Request threads never call recv() — they only dequeue from this index,
    // making the receive path truly async and contention-free.
    //
    // request_id 0 is reserved for the raw-string GET_WORKERS response, which
    // carries no ANARI magic/request_id, so it is routed to a separate FIFO.
    mutable std::mutex responseQueueMutex;
    std::condition_variable responseQueueCv;
    std::unordered_map<uint32_t, std::deque<FramePairPtr>> responseByRequest;
    std::deque<FramePairPtr> noIdResponseQueue;
    std::atomic<size_t> totalQueuedFrames{0};
    std::atomic<int64_t> lastQueueWarnMs{0};

    // Dispatcher thread — owns ALL ZMQ recv operations.
    std::thread dispatchThread;
    std::atomic<bool> dispatcherActive{false};

    // Bounded parallel-download worker pool used by requestFilesParallel.
    // Owned by the client and torn down in cleanup() BEFORE the socket closes,
    // so in-flight download threads are always joined and cannot outlive the
    // ZMQ context.
    struct FileDownloadTask {
        std::function<void()> run;
    };
    void downloadWorkerLoop();
    std::atomic<bool> downloadPoolActive{false};
    std::deque<FileDownloadTask> downloadTaskQueue;
    std::mutex downloadTaskMutex;
    std::condition_variable downloadTaskCv;
    std::vector<std::thread> downloadWorkers;
    static constexpr int MAX_PARALLEL_DOWNLOADS = 8;
private:
    // Dedicated background dispatcher: continuously polls the ZMQ socket
    // and enqueues every incoming frame pair into responseQueue.
    void dispatcherThread();

    // Statistics and monitoring
    ConnectionStats connectionStats;
    std::atomic<size_t> maxMessageSize{104857600}; // 100MB default
    std::chrono::steady_clock::time_point lastHealthCheck;

    // Notification callback (live update support)
    std::mutex notificationCallbackMutex;
    NotificationCallback notificationCallback;

    // Typed scene/property update callback
    std::mutex sceneUpdateCallbackMutex;
    SceneUpdateCallback sceneUpdateCallback;

    // Protocol diagnostics callback
    mutable std::mutex protocolDiagnosticsCallbackMutex;
    ProtocolDiagnosticsCallback protocolDiagnosticsCallback;

    // Default chunk size for file requests
    // Larger chunks = fewer ZMQ round-trips through the (single-loop) broker:
    // a 76MB file is ~2-3 messages instead of 19. The broker/worker honor the
    // requested size (capped server-side at 128MB).
    static constexpr uint32_t DEFAULT_CHUNK_SIZE = 32 * 1024 * 1024; // 32MB

private:
    // Extract request_id from a data frame (first 4 bytes = magic, next 4 = type, next 4 = request_id)
    uint32_t extractRequestId(const uint8_t* data, size_t dataSize) const;

    // Enqueue a frame pair into the response queue and wake waiters.
    // Only called by the dispatcher thread. Frames are moved in (zero-copy).
    void enqueueResponseFrame(zmq::message_t delimiter,
                              zmq::message_t data);

    // Wait for a matching message pair for the given request_id.
    // Blocks until a matching frame arrives or timeout expires.
    bool waitForMatchingFrames(uint32_t requestId, int timeoutMs,
                               FrameMsg& outDelimiter,
                               FrameMsg& outData);

    // Attempt to dequeue a matching message (non-blocking).
    bool tryDequeueMatching(uint32_t requestId,
                             FrameMsg& outDelimiter,
                             FrameMsg& outData);

    // Same as tryDequeueMatching, but assumes responseQueueMutex is ALREADY held
    // by the caller.  Calling tryDequeueMatching while holding responseQueueMutex
    // self-deadlocks (re-locking the same non-recursive mutex).
    bool tryDequeueMatchingLocked(uint32_t requestId,
                                  FrameMsg& outDelimiter,
                                  FrameMsg& outData);

    // Handle notification message (called from dispatcher thread)
    void handleNotification(const zmq::message_t& data);

    // Handle typed scene/property update messages (called from dispatcher thread)
    void handleSceneUpdate(const zmq::message_t& data);

    // Emit diagnostics through ProtocolDiagnosticsCallback if registered.
    void emitProtocolDiagnostics(const std::string& event,
                                 const std::string& message,
                                 uint64_t value0 = 0,
                                 uint64_t value1 = 0) const;

    // Async file download frame handler — called from dispatcher thread.
    // Returns true if frame was consumed by an async download, false to forward to blocking queue.
    bool asyncFileFrameHandler(uint32_t requestId, const uint8_t* data, size_t size, uint32_t msgType);

private:
    // Async file state for non-blocking downloads.
    struct AsyncFileState {
        uint32_t request_id;
        std::string filename;
        int32_t target_rank;
        std::chrono::steady_clock::time_point deadline;
        std::vector<uint8_t> accumulated_data;
        uint64_t expected_file_size = 0;
        uint64_t received_bytes = 0;
        bool completed = false;
        bool failed = false;
        FileChunkCallback chunk_callback;
        FileCompleteCallback complete_callback;
        ErrorCallback error_callback;
    };

    // Async download tracking — populated by requestFileAsync, consumed by dispatcher.
    std::mutex asyncFilesMutex;
    std::map<uint32_t, std::unique_ptr<AsyncFileState>> asyncFiles;
};

} // namespace anari_usd_middleware