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

// Forward declaration
class AnariUsdClient;

/**
 * @brief Manages parallel file downloads with RAM awareness and immediate spawning
 * 
 * Uses a single DEALER socket with client-side multiplexing to handle
 * multiple concurrent file downloads. Tracks available RAM to prevent
 * memory exhaustion. Spawns files immediately as they complete and
 * frees memory right after spawning.
 */
class ParallelDownloadManager {
public:
    using FileSpawnCallback = std::function<void(const std::string& filename,
                                                 const std::vector<uint8_t>& data)>;
    using CompletionCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string& filename,
                                             const std::string& error)>;

    /**
     * @brief Construct a ParallelDownloadManager
     * @param client Existing AnariUsdClient (must stay connected)
     * @param max_parallel_downloads Maximum concurrent downloads (0 = auto)
     */
    explicit ParallelDownloadManager(std::shared_ptr<AnariUsdClient> client,
                                     size_t max_parallel_downloads = 0);
    
    ~ParallelDownloadManager();
    
    // Disable copying
    ParallelDownloadManager(const ParallelDownloadManager&) = delete;
    ParallelDownloadManager& operator=(const ParallelDownloadManager&) = delete;
    
    /**
     * @brief Start streaming parallel downloads
     * @param filenames Files to download
     * @param target_ranks Target ranks for each file
     * @param spawn_callback Called IMMEDIATELY when a file downloads
     * @param completion_callback Called when ALL downloads complete
     * @param error_callback Called on download errors
     * @param timeout_ms Timeout per file in milliseconds
     */
    void downloadFilesStreaming(
        const std::vector<std::string>& filenames,
        const std::vector<int32_t>& target_ranks,
        FileSpawnCallback spawn_callback,
        CompletionCallback completion_callback = nullptr,
        ErrorCallback error_callback = nullptr,
        int timeout_ms = 30000);
    
    /**
     * @brief Stop all downloads (graceful shutdown)
     */
    void stop();
    
    /**
     * @brief Check if manager is running
     */
    bool isRunning() const { return running.load(); }
    
    /**
     * @brief Get statistics
     */
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
        
        // Callbacks
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
    // Core components
    std::shared_ptr<AnariUsdClient> client;
    MemoryMonitor memory_monitor;
    
    // State management
    std::atomic<bool> running{false};
    std::atomic<bool> shutdown_requested{false};
    
    // Download tracking
    std::unordered_map<uint32_t, std::shared_ptr<DownloadContext>> active_downloads;
    std::queue<PendingDownload> pending_downloads;
    
    // Synchronization
    mutable std::mutex state_mutex;
    std::condition_variable state_cv;
    
    // Threads
    std::thread dispatcher_thread;
    std::thread download_scheduler_thread;
    std::vector<std::thread> download_worker_threads;
    
    // Statistics
    std::atomic<size_t> total_files_requested{0};
    std::atomic<size_t> files_downloaded{0};
    std::atomic<size_t> files_spawned{0};
    std::atomic<size_t> download_errors{0};
    std::atomic<size_t> total_bytes_downloaded{0};
    
    // Configuration
    size_t max_parallel_downloads;
    int default_timeout_ms;
    
private:
    // Thread functions
    void dispatcherThread();
    void downloadSchedulerThread();
    void downloadWorkerThread();
    
    // Internal methods
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
    
    // Callbacks (called when all downloads complete)
    CompletionCallback completion_callback;
};

} // namespace anari_usd_middleware