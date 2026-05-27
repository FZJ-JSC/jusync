#pragma once

#include "AnariUsdClient.h"
#include <vector>
#include <string>
#include <future>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>

namespace anari_usd_middleware {

/**
 * Parallel Downloader Utility
 * Provides high-level interface for parallel file downloads
 */
class ParallelDownloader {
public:
    // Download result structure
    struct DownloadResult {
        std::string filename;
        std::vector<uint8_t> data;
        bool success{false};
        std::string error;
        uint64_t size{0};
        std::chrono::milliseconds duration{0};
    };

    // Progress callback type
    using ProgressCallback = std::function<void(const std::string& filename,
                                                uint64_t downloaded,
                                                uint64_t total,
                                                size_t activeTransfers)>;

    // Completion callback type  
    using CompletionCallback = std::function<void(const DownloadResult& result)>;

public:
    ParallelDownloader(std::shared_ptr<AnariUsdClient> client);
    ~ParallelDownloader();

    // Disable copy
    ParallelDownloader(const ParallelDownloader&) = delete;
    ParallelDownloader& operator=(const ParallelDownloader&) = delete;

    // Enable move
    ParallelDownloader(ParallelDownloader&&) = default;
    ParallelDownloader& operator=(ParallelDownloader&&) = default;

    /**
     * Download multiple files in parallel
     * @param files List of filenames to download
     * @param targetRank Target worker rank (-1 for all)
     * @param maxParallel Maximum parallel downloads (0 = unlimited)
     * @param timeoutMs Timeout per file in milliseconds
     * @return Vector of download results
     */
    std::vector<DownloadResult> downloadFiles(const std::vector<std::string>& files,
                                              int32_t targetRank = -1,
                                              size_t maxParallel = 0,
                                              int timeoutMs = 30000);

    /**
     * Download multiple files asynchronously
     * @param files List of filenames to download
     * @param targetRank Target worker rank (-1 for all)
     * @param progressCallback Called for each chunk received
     * @param completionCallback Called when each file completes
     * @param maxParallel Maximum parallel downloads (0 = unlimited)
     * @param timeoutMs Timeout per file in milliseconds
     */
    void downloadFilesAsync(const std::vector<std::string>& files,
                            int32_t targetRank,
                            ProgressCallback progressCallback = nullptr,
                            CompletionCallback completionCallback = nullptr,
                            size_t maxParallel = 0,
                            int timeoutMs = 30000);

    /**
     * Cancel all ongoing downloads
     */
    void cancelAll();

    /**
     * Wait for all downloads to complete
     * @param timeoutMs Maximum time to wait in milliseconds
     * @return True if all downloads completed, false on timeout
     */
    bool waitForCompletion(int timeoutMs = 60000);

    /**
     * Get number of active downloads
     */
    size_t getActiveDownloadCount() const;

    /**
     * Get number of completed downloads
     */
    size_t getCompletedDownloadCount() const;

    /**
     * Get total bytes downloaded
     */
    uint64_t getTotalBytesDownloaded() const;

    /**
     * Get average download speed in bytes per second
     */
    double getAverageSpeed() const;

    /**
     * Streaming pipeline: Download files with RAM-aware parallelism
     * Downloads files in parallel but processes them sequentially to control RAM usage
     * @param files List of filenames to download
     * @param targetRank Target worker rank (-1 for all)
     * @param maxMemoryBytes Maximum memory to use for downloads (0 = unlimited)
     * @param maxParallel Maximum parallel downloads (0 = unlimited)
     * @param timeoutMs Timeout per file in milliseconds
     * @param onFileReady Callback when each file is ready for processing
     * @param onComplete Callback when all files are processed
     */
    void downloadWithStreaming(const std::vector<std::string>& files,
                              int32_t targetRank = -1,
                              uint64_t maxMemoryBytes = 0,
                              size_t maxParallel = 0,
                              int timeoutMs = 30000,
                              std::function<void(const DownloadResult&)> onFileReady = nullptr,
                              std::function<void()> onComplete = nullptr);

    /**
     * Get current memory usage from active downloads
     */
    uint64_t getCurrentMemoryUsage() const;

private:
    struct DownloadTask {
        std::string filename;
        int32_t targetRank;
        int timeoutMs;
        std::promise<DownloadResult> promise;
        std::future<DownloadResult> future;
        std::chrono::steady_clock::time_point startTime;
        uint64_t totalSize{0};
        uint64_t downloaded{0};
        bool completed{false};
        
        DownloadResult result;
    };

    // Client instance
    std::shared_ptr<AnariUsdClient> client_;
    
    // Task management
    std::vector<std::unique_ptr<DownloadTask>> tasks_;
    std::unordered_map<uint32_t, DownloadTask*> requestIdToTask_;
    mutable std::mutex tasksMutex_;
    std::condition_variable tasksCV_;
    
    // Statistics
    std::atomic<uint64_t> totalBytesDownloaded_{0};
    std::atomic<size_t> activeDownloads_{0};
    std::atomic<size_t> completedDownloads_{0};
    std::chrono::steady_clock::time_point startTime_;
    
    // Callbacks
    ProgressCallback progressCallback_;
    CompletionCallback completionCallback_;
    
    // Worker thread
    std::thread pollThread_;
    std::atomic<bool> running_{false};
    
    // Private methods
    void startPollThread();
    void stopPollThread();
    void pollThreadFunc();
    void processCompletedTasks();
    void cleanupCompletedTasks();
    
    // Callback handlers
    void onChunkReceived(uint32_t requestId, const std::string& filename,
                        const std::vector<uint8_t>& chunk,
                        uint64_t offset, uint64_t totalSize);
    void onFileComplete(uint32_t requestId, const std::string& filename,
                       uint64_t totalSize);
    void onError(uint32_t requestId, const std::string& error);
};

} // namespace anari_usd_middleware