#pragma once

#include "MemoryMonitor.h"
#include "MiddlewareLogging.h"
#include "AnariUsdMessages.h"

#include <zmq.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <queue>
#include <future>
#include <memory>

namespace anari_usd_middleware {

class AnariUsdClient;

class ParallelDownloadManager {
public:
    using FileSpawnCallback = std::function<void(const std::string& filename,
                                                 const std::vector<uint8_t>& data)>;
    using CompletionCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string& filename,
                                             const std::string& error)>;

    explicit ParallelDownloadManager(std::shared_ptr<AnariUsdClient> client,
                                     size_t max_parallel_downloads = 0);

    ~ParallelDownloadManager();

    ParallelDownloadManager(const ParallelDownloadManager&) = delete;
    ParallelDownloadManager& operator=(const ParallelDownloadManager&) = delete;

    void downloadFilesStreaming(
        const std::vector<std::string>& filenames,
        const std::vector<int32_t>& target_ranks,
        FileSpawnCallback spawn_callback,
        CompletionCallback completion_callback = nullptr,
        ErrorCallback error_callback = nullptr,
        int timeout_ms = 30000);

    void stop();

    bool isRunning() const { return running.load(); }

    struct Stats {
        size_t total_files_requested = 0;
        size_t files_downloaded = 0;
        size_t files_spawned = 0;
        size_t download_errors = 0;
        size_t total_bytes_downloaded = 0;
        float memory_usage_percentage = 0.0f;
    };

    Stats getStats() const;

private:
    struct DownloadContext {
        uint32_t request_id;
        std::string filename;
        int32_t target_rank;
        size_t estimated_size;
        std::chrono::steady_clock::time_point start_time;
        std::vector<uint8_t> accumulated_data;
        uint64_t expected_file_size = 0;
        uint64_t received_bytes = 0;
        bool completed = false;
        bool failed = false;
        std::string error_message;

        FileSpawnCallback spawn_callback;
        ErrorCallback error_callback;
    };

    struct PendingDownload {
        std::string filename;
        int32_t target_rank;
        size_t estimated_size;
        FileSpawnCallback spawn_callback;
        ErrorCallback error_callback;
    };

private:
    std::shared_ptr<AnariUsdClient> client;
    MemoryMonitor memory_monitor;

    std::atomic<bool> running{false};
    std::atomic<bool> shutdown_requested{false};

    std::unordered_map<uint32_t, std::shared_ptr<DownloadContext>> active_downloads;
    std::queue<PendingDownload> pending_downloads;

    mutable std::mutex state_mutex;
    std::condition_variable state_cv;

    std::thread dispatcher_thread;
    std::thread download_scheduler_thread;
    std::vector<std::thread> download_worker_threads;

    std::atomic<size_t> total_files_requested{0};
    std::atomic<size_t> files_downloaded{0};
    std::atomic<size_t> files_spawned{0};
    std::atomic<size_t> download_errors{0};
    std::atomic<size_t> total_bytes_downloaded{0};

    size_t max_parallel_downloads;
    int default_timeout_ms;

private:
    void dispatcherThread();
    void downloadSchedulerThread();
    void downloadWorkerThread();

    void startDispatcher();
    void startScheduler();
    void startWorkers();

    bool sendFileRequest(const std::string& filename, int32_t target_rank,
                        uint32_t request_id, int timeout_ms);
    void processIncomingResponse(zmq::message_t& response);
    void handleFileChunk(const ZmqFileChunk* chunk, const std::vector<uint8_t>& chunk_data,
                        std::shared_ptr<DownloadContext> context);
    void handleFileComplete(const ZmqFileComplete* complete,
                           std::shared_ptr<DownloadContext> context);
    void handleDownloadError(std::shared_ptr<DownloadContext> context,
                            const std::string& error);

    void spawnFileImmediately(std::shared_ptr<DownloadContext> context);
    void cleanupCompletedDownload(std::shared_ptr<DownloadContext> context);

    size_t estimateFileSize(const std::string& filename) const;
    uint32_t generateRequestId();

    CompletionCallback completion_callback;
};

} // namespace anari_usd_middleware