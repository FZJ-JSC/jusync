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

class AnariUsdClient : public std::enable_shared_from_this<AnariUsdClient> {
public:
    enum class ConnectionStatus {
        Disconnected,
        Connecting,
        Connected,
        ShuttingDown,
        Error
    };

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

    using ProtocolDiagnosticsCallback = std::function<void(const std::string& event,
                                                            const std::string& message,
                                                            uint64_t value0,
                                                            uint64_t value1)>;

    using WorkerStatusCallback = std::function<void(int32_t rank,
                                                    uint32_t status,
                                                    const std::string& hostname,
                                                    const std::string& gpuInfo,
                                                    uint64_t lastHeartbeat)>;
    using WorkerCountCallback = std::function<void(uint32_t totalWorkers)>;

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
    AnariUsdClient();
    virtual ~AnariUsdClient();

    AnariUsdClient(const AnariUsdClient&) = delete;
    AnariUsdClient& operator=(const AnariUsdClient&) = delete;

    AnariUsdClient(AnariUsdClient&&) = default;
    AnariUsdClient& operator=(AnariUsdClient&&) = default;

    bool connect(const char* brokerEndpoint, int timeoutMs = 5000);
    void disconnect(int gracefulTimeoutMs = 1000);
    bool isConnected() const;
    ConnectionStatus getConnectionStatus() const;
    bool isShutdownRequested() const { return shutdownRequested.load(); }

    bool requestFileList(int32_t targetRank, FileListCallback callback, int timeoutMs = 10000);
    bool requestFileListWithSizes(int32_t targetRank, FileListWithSizesCallback callback, int timeoutMs = 10000);
    bool requestFile(const std::string& filename, int32_t targetRank,
                     FileChunkCallback chunkCallback,
                     FileCompleteCallback completeCallback,
                     ErrorCallback errorCallback = nullptr,
                     int timeoutMs = 30000,
                     const std::atomic<bool>* abortFlag = nullptr);

    void abandonRequestFrames(uint32_t requestId);
    void wakeResponseWaiters();
    bool requestFrame(int32_t frameNumber, int32_t targetRank,
                      FileChunkCallback chunkCallback,
                      FileCompleteCallback completeCallback,
                      ErrorCallback errorCallback = nullptr,
                      int timeoutMs = 60000);

    bool getFileSync(const std::string& filename, int32_t targetRank,
                     std::vector<uint8_t>& fileData, int timeoutMs = 30000);
    bool getFileListSync(int32_t targetRank, std::vector<std::string>& files, int timeoutMs = 10000);
    bool getFileListWithSizesSync(int32_t targetRank, std::vector<FileInfo>& files, int timeoutMs = 10000);

    bool requestWorkerStatus(int32_t targetRank, WorkerStatusCallback callback, int timeoutMs = 5000);
    bool requestWorkerCount(WorkerCountCallback callback, int timeoutMs = 5000);

    bool requestWorkerListString(std::vector<std::tuple<int32_t, std::string, std::string>>& outWorkers, int timeoutMs = 5000);

    bool getWorkerStatusSync(int32_t targetRank,
                             std::vector<std::tuple<int32_t, uint32_t, std::string, std::string, uint64_t>>& workers,
                             int timeoutMs = 10000);
    bool getWorkerCountSync(uint32_t& workerCount, int timeoutMs = 5000);

    // Total worker count including rank 0 (uses legacy string protocol)

    bool getTotalWorkerCountSync(uint32_t& totalCount, int timeoutMs = 5000);

    ConnectionStats::Snapshot getConnectionStats() const;
    void resetConnectionStats();
    void setMaxMessageSize(size_t maxSizeBytes);
    size_t getMaxMessageSize() const;

    bool testConnection();
    void updateHealthStatus();

    void setNotificationCallback(NotificationCallback callback);
    void setSceneUpdateCallback(SceneUpdateCallback callback);
    void setProtocolDiagnosticsCallback(ProtocolDiagnosticsCallback callback);
    uint32_t getProtocolVersion() const { return ANARI_USD_PROTOCOL_VERSION; }

    zmq::socket_t* getSocket() { return zmqSocket.get(); }
    const zmq::socket_t* getSocket() const { return zmqSocket.get(); }

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
    bool configureSocket(int timeoutMs);
    bool validateEndpoint(const std::string& endpoint) const;
    void cleanup();

    bool sendRequest(const void* data, size_t size, uint32_t requestId);

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

    uint32_t generateRequestId();

#if PLATFORM_WINDOWS
    bool configureWindowsSocket();
#elif PLATFORM_LINUX
    bool configureLinuxSocket();
#endif

    std::unique_ptr<zmq::context_t> zmqContext;
    std::unique_ptr<zmq::socket_t> zmqSocket;

    std::atomic<ConnectionStatus> connectionStatus{ConnectionStatus::Disconnected};
    std::atomic<bool> shutdownRequested{false};
    std::string brokerEndpoint;

    std::string clientRoutingId;

    mutable std::mutex connectionMutex;
    mutable std::recursive_mutex requestMutex;

    // Serialize ALL recv operations on the ZMQ DEALER socket.

    mutable std::mutex recvMutex;

    std::atomic<uint32_t> nextRequestId{1};
    std::map<uint32_t, bool> pendingRequests;

    using FrameMsg = std::shared_ptr<zmq::message_t>;

    using FramePair = std::pair<FrameMsg, FrameMsg>;
    using FramePairPtr = std::shared_ptr<FramePair>;

    // Out-of-order response buffering for multi-threaded safety.

    mutable std::mutex responseQueueMutex;
    std::condition_variable responseQueueCv;
    std::unordered_map<uint32_t, std::deque<FramePairPtr>> responseByRequest;
    std::deque<FramePairPtr> noIdResponseQueue;
    std::atomic<size_t> totalQueuedFrames{0};
    std::atomic<int64_t> lastQueueWarnMs{0};

    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> abandonedRequests;

    // Dispatcher thread — owns ALL ZMQ recv operations.

    std::thread dispatchThread;
    std::atomic<bool> dispatcherActive{false};

    std::atomic<bool> reconnectInProgress{false};
    std::atomic<bool> explicitDisconnect{false};
    int connectTimeoutMs{5000}; // last connect() timeout, reused by auto-reconnect

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
    void dispatcherThread();

    void sendIdlePing();

    ConnectionStats connectionStats;
    std::atomic<size_t> maxMessageSize{104857600}; // 100MB default
    std::chrono::steady_clock::time_point lastHealthCheck;

    std::mutex notificationCallbackMutex;
    NotificationCallback notificationCallback;

    std::mutex sceneUpdateCallbackMutex;
    SceneUpdateCallback sceneUpdateCallback;

    mutable std::mutex protocolDiagnosticsCallbackMutex;
    ProtocolDiagnosticsCallback protocolDiagnosticsCallback;

    static constexpr uint32_t DEFAULT_CHUNK_SIZE = 32 * 1024 * 1024; // 32MB

private:
    // Extract request_id from a data frame (first 4 bytes = magic, next 4 = type, next 4 = request_id)

    uint32_t extractRequestId(const uint8_t* data, size_t dataSize) const;

    // Only called by the dispatcher thread. Frames are moved in (zero-copy).

    void enqueueResponseFrame(zmq::message_t delimiter,
                              zmq::message_t data);

    bool waitForMatchingFrames(uint32_t requestId, int timeoutMs,
                               FrameMsg& outDelimiter,
                               FrameMsg& outData,
                               const std::atomic<bool>* abortFlag = nullptr);

    bool tryDequeueMatching(uint32_t requestId,
                             FrameMsg& outDelimiter,
                             FrameMsg& outData);

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

    bool asyncFileFrameHandler(uint32_t requestId, const uint8_t* data, size_t size, uint32_t msgType);

private:
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

    std::mutex asyncFilesMutex;
    std::map<uint32_t, std::unique_ptr<AsyncFileState>> asyncFiles;
};

} // namespace anari_usd_middleware