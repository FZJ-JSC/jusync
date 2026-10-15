#pragma once

#include "AnariUsdClient.h"
#include <vector>
#include <string>
#include <future>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>

#ifndef ANARI_USD_DISABLE_PARALLEL_DOWNLOADER_ERROR
#error "ParallelDownloader is broken — use ParallelDownloadManager instead. Define ANARI_USD_DISABLE_PARALLEL_DOWNLOADER_ERROR to suppress this."
#endif

namespace anari_usd_middleware {

class ParallelDownloader {
public:
    struct DownloadResult {
        std::string filename;
        std::vector<uint8_t> data;
        bool success{false};
        std::string error;
        uint64_t size{0};
        std::chrono::milliseconds duration{0};
    };

    using ProgressCallback = std::function<void(const std::string& filename,
                                                uint64_t downloaded,
                                                uint64_t total,
                                                size_t activeTransfers)>;

    using CompletionCallback = std::function<void(const DownloadResult& result)>;

public:
    ParallelDownloader(std::shared_ptr<AnariUsdClient> client);
    ~ParallelDownloader();

    ParallelDownloader(const ParallelDownloader&) = delete;
    ParallelDownloader& operator=(const ParallelDownloader&) = delete;

    ParallelDownloader(ParallelDownloader&&) = default;
    ParallelDownloader& operator=(ParallelDownloader&&) = default;

    std::vector<DownloadResult> downloadFiles(const std::vector<std::string>& files,
                                              int32_t targetRank = -1,
                                              size_t maxParallel = 0,
                                              int timeoutMs = 30000);

    void downloadFilesAsync(const std::vector<std::string>& files,
                            int32_t targetRank,
                            ProgressCallback progressCallback = nullptr,
                            CompletionCallback completionCallback = nullptr,
                            size_t maxParallel = 0,
                            int timeoutMs = 30000);

    void cancelAll();

    bool waitForCompletion(int timeoutMs = 60000);

    size_t getActiveDownloadCount() const;

    size_t getCompletedDownloadCount() const;

    uint64_t getTotalBytesDownloaded() const;

    double getAverageSpeed() const;

    void downloadWithStreaming(const std::vector<std::string>& files,
                              int32_t targetRank = -1,
                              uint64_t maxMemoryBytes = 0,
                              size_t maxParallel = 0,
                              int timeoutMs = 30000,
                              std::function<void(const DownloadResult&)> onFileReady = nullptr,
                              std::function<void()> onComplete = nullptr);

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

    std::shared_ptr<AnariUsdClient> client_;

    std::vector<std::unique_ptr<DownloadTask>> tasks_;
    std::unordered_map<uint32_t, DownloadTask*> requestIdToTask_;
    mutable std::mutex tasksMutex_;
    std::condition_variable tasksCV_;

    std::atomic<uint64_t> totalBytesDownloaded_{0};
    std::atomic<size_t> activeDownloads_{0};
    std::atomic<size_t> completedDownloads_{0};
    std::chrono::steady_clock::time_point startTime_;

    ProgressCallback progressCallback_;
    CompletionCallback completionCallback_;

    std::thread pollThread_;
    std::thread streamingThread_;
    std::atomic<bool> running_{false};

    void startPollThread();
    void stopPollThread();
    void pollThreadFunc();
    void processCompletedTasks();
    void cleanupCompletedTasks();

    void onChunkReceived(uint32_t requestId, const std::string& filename,
                        const std::vector<uint8_t>& chunk,
                        uint64_t offset, uint64_t totalSize);
    void onFileComplete(uint32_t requestId, const std::string& filename,
                       uint64_t totalSize);
    void onError(uint32_t requestId, const std::string& error);
};

} // namespace anari_usd_middleware