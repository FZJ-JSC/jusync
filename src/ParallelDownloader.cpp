#include "ParallelDownloader.h"
#include "MiddlewareLogging.h"
#include "StreamDiagnostics.h"
#include <algorithm>
#include <thread>
#include <chrono>

namespace anari_usd_middleware {

ParallelDownloader::ParallelDownloader(std::shared_ptr<AnariUsdClient> client)
    : client_(client), running_(false) {

    if (!client_) {
        throw std::invalid_argument("ParallelDownloader requires a valid AnariUsdClient");
    }

    startTime_ = std::chrono::steady_clock::now();
    MIDDLEWARE_LOG_INFO("ParallelDownloader created");
}

ParallelDownloader::~ParallelDownloader() {
    cancelAll();
    stopPollThread();
    if (streamingThread_.joinable()) {
        streamingThread_.join();
    }
    MIDDLEWARE_LOG_INFO("ParallelDownloader destroyed");
}

std::vector<ParallelDownloader::DownloadResult>
ParallelDownloader::downloadFiles(const std::vector<std::string>& files,
                                  int32_t targetRank,
                                  size_t maxParallel,
                                  int timeoutMs) {

    if (files.empty()) {
        MIDDLEWARE_LOG_WARNING("No files to download");
        return {};
    }

    if (!client_->isConnected()) {
        MIDDLEWARE_LOG_ERROR("Client not connected");
        std::vector<DownloadResult> results;
        for (const auto& file : files) {
            DownloadResult result;
            result.filename = file;
            result.success = false;
            result.error = "Client not connected";
            results.push_back(result);
        }
        return results;
    }

    MIDDLEWARE_LOG_INFO("Starting parallel download of %zu files", files.size());

    size_t actualMaxParallel = maxParallel > 0 ? maxParallel : files.size();
    if (actualMaxParallel > files.size()) {
        actualMaxParallel = files.size();
    }

    std::shared_ptr<std::vector<std::unique_ptr<DownloadTask>>> tasks = std::make_shared<std::vector<std::unique_ptr<DownloadTask>>>();
    std::vector<std::future<DownloadResult>> futures;

    for (const auto& file : files) {
        auto task = std::make_unique<DownloadTask>();
        task->filename = file;
        task->targetRank = targetRank;
        task->timeoutMs = timeoutMs;
        task->startTime = std::chrono::steady_clock::now();

        futures.push_back(task->promise.get_future());
        tasks->push_back(std::move(task));
    }

    size_t started = 0;
    size_t completed = 0;
    std::vector<uint32_t> requestIds;

    while (completed < files.size()) {
        while (started - completed < actualMaxParallel && started < files.size()) {
            auto& task = (*tasks)[started];

            jusync_diag::Diagnostics::instance().mark(task->filename, jusync_diag::STAGE_DL_START);
            uint32_t requestId = client_->requestFileAsync(
                task->filename,
                task->targetRank,
                [this, started, tasks](const std::string& filename,
                                       const std::vector<uint8_t>& chunk,
                                       uint64_t offset,
                                       uint64_t totalSize) {
                    auto& task = (*tasks)[started];
                    if (task->result.data.size() < offset + chunk.size()) {
                        task->result.data.resize(offset + chunk.size());
                    }
                    std::copy(chunk.begin(), chunk.end(),
                             task->result.data.begin() + offset);
                    task->downloaded += chunk.size();
                    task->totalSize = totalSize;

                    totalBytesDownloaded_.fetch_add(chunk.size());

                    if (progressCallback_) {
                        progressCallback_(filename, task->downloaded,
                                         task->totalSize, activeDownloads_.load());
                    }
                },
                [this, started, tasks](const std::string& filename, uint64_t totalSize) {
                    auto& task = (*tasks)[started];
                    task->completed = true;
                    task->result.success = true;
                    task->result.size = totalSize;
                    jusync_diag::Diagnostics::instance().mark(
                        task->filename, jusync_diag::STAGE_DL_DONE, totalSize);
                    task->result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - task->startTime);

                    if (task->result.data.size() > totalSize) {
                        task->result.data.resize(totalSize);
                    }

                    task->promise.set_value(task->result);

                    activeDownloads_.fetch_sub(1);
                    completedDownloads_.fetch_add(1);

                    if (completionCallback_) {
                        completionCallback_(task->result);
                    }

                    MIDDLEWARE_LOG_DEBUG("File download completed: %s (%llu bytes)",
                                        filename.c_str(), totalSize);
                },
                [this, started, tasks](const std::string& error) {
                    auto& task = (*tasks)[started];
                    task->completed = true;
                    task->result.success = false;
                    task->result.error = error;
                    task->result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - task->startTime);

                    task->promise.set_value(task->result);

                    activeDownloads_.fetch_sub(1);
                    completedDownloads_.fetch_add(1);

                    if (completionCallback_) {
                        completionCallback_(task->result);
                    }

                    MIDDLEWARE_LOG_ERROR("File download failed: %s - %s",
                                        task->filename.c_str(), error.c_str());
                },
                timeoutMs);

            if (requestId == 0) {
                task->result.filename = task->filename;
                task->result.success = false;
                task->result.error = "Failed to start download";
                task->promise.set_value(task->result);
                completed++;
            } else {
                requestIds.push_back(requestId);
                activeDownloads_.fetch_add(1);
                MIDDLEWARE_LOG_DEBUG("Started download: %s (request_id: %u)",
                                    task->filename.c_str(), requestId);
            }

            started++;
        }

        client_->poll(100);

        for (size_t i = 0; i < started; i++) {
            if ((*tasks)[i]->completed) {
                completed++;
            }
        }

        if (completed < files.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::vector<DownloadResult> results;
    for (auto& future : futures) {
        results.push_back(future.get());
    }

    MIDDLEWARE_LOG_INFO("Parallel download completed: %zu successful, %zu failed",
                       std::count_if(results.begin(), results.end(),
                                    [](const DownloadResult& r) { return r.success; }),
                       std::count_if(results.begin(), results.end(),
                                    [](const DownloadResult& r) { return !r.success; }));

    return results;
}

void ParallelDownloader::downloadFilesAsync(const std::vector<std::string>& files,
                                           int32_t targetRank,
                                           ProgressCallback progressCallback,
                                           CompletionCallback completionCallback,
                                           size_t maxParallel,
                                           int timeoutMs) {

    progressCallback_ = progressCallback;
    completionCallback_ = completionCallback;

    if (!running_) {
        startPollThread();
    }

    std::thread downloadThread([this, files, targetRank, maxParallel, timeoutMs]() {
        downloadFiles(files, targetRank, maxParallel, timeoutMs);
    });

    if (streamingThread_.joinable()) {
        streamingThread_.detach(); // Let old worker finish on its own
    }
    streamingThread_ = std::move(downloadThread);
}

void ParallelDownloader::cancelAll() {
    std::lock_guard<std::mutex> lock(tasksMutex_);

    for (const auto& pair : requestIdToTask_) {
        client_->cancelRequest(pair.first);
    }

    tasks_.clear();
    requestIdToTask_.clear();

    activeDownloads_.store(0);
    completedDownloads_.store(0);
    totalBytesDownloaded_.store(0);

    MIDDLEWARE_LOG_INFO("All downloads cancelled");
}

bool ParallelDownloader::waitForCompletion(int timeoutMs) {
    auto startTime = std::chrono::steady_clock::now();

    while (true) {
        if (activeDownloads_.load() == 0) {
            return true;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime);
        if (elapsed.count() >= timeoutMs) {
            MIDDLEWARE_LOG_WARNING("Timeout waiting for downloads to complete");
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

size_t ParallelDownloader::getActiveDownloadCount() const {
    return activeDownloads_.load();
}

size_t ParallelDownloader::getCompletedDownloadCount() const {
    return completedDownloads_.load();
}

uint64_t ParallelDownloader::getTotalBytesDownloaded() const {
    return totalBytesDownloaded_.load();
}

double ParallelDownloader::getAverageSpeed() const {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - startTime_);

    if (duration.count() == 0) {
        return 0.0;
    }

    return static_cast<double>(totalBytesDownloaded_.load()) / duration.count();
}

void ParallelDownloader::startPollThread() {
    if (running_) {
        return;
    }

    running_ = true;
    pollThread_ = std::thread(&ParallelDownloader::pollThreadFunc, this);
    MIDDLEWARE_LOG_DEBUG("Poll thread started");
}

void ParallelDownloader::stopPollThread() {
    if (!running_) {
        return;
    }

    running_ = false;
    if (pollThread_.joinable()) {
        pollThread_.join();
    }
    MIDDLEWARE_LOG_DEBUG("Poll thread stopped");
}

void ParallelDownloader::pollThreadFunc() {
    while (running_) {
        client_->poll(100);

        processCompletedTasks();

        cleanupCompletedTasks();

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ParallelDownloader::processCompletedTasks() {
}

void ParallelDownloader::cleanupCompletedTasks() {
    std::lock_guard<std::mutex> lock(tasksMutex_);

    auto now = std::chrono::steady_clock::now();
    auto fiveMinutes = std::chrono::minutes(5);

    auto it = tasks_.begin();
    while (it != tasks_.end()) {
        auto& task = *it;
        if (task->completed) {
            auto age = now - task->startTime;
            if (age > fiveMinutes) {
                for (auto mapIt = requestIdToTask_.begin(); mapIt != requestIdToTask_.end(); ) {
                    if (mapIt->second == task.get()) {
                        mapIt = requestIdToTask_.erase(mapIt);
                    } else {
                        ++mapIt;
                    }
                }

                it = tasks_.erase(it);
            } else {
                ++it;
            }
        } else {
            ++it;
        }
    }
}

uint64_t ParallelDownloader::getCurrentMemoryUsage() const {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    uint64_t total = 0;

    for (const auto& task : tasks_) {
        if (!task->completed) {
            total += task->downloaded;
            total += 1024; // Overhead per task
        }
    }

    return total;
}

void ParallelDownloader::downloadWithStreaming(const std::vector<std::string>& files,
                                              int32_t targetRank,
                                              uint64_t maxMemoryBytes,
                                              size_t maxParallel,
                                              int timeoutMs,
                                              std::function<void(const DownloadResult&)> onFileReady,
                                              std::function<void()> onComplete) {

    if (files.empty()) {
        MIDDLEWARE_LOG_WARNING("No files to download for streaming");
        if (onComplete) onComplete();
        return;
    }

    if (!client_->isConnected()) {
        MIDDLEWARE_LOG_ERROR("Client not connected for streaming");
        for (const auto& file : files) {
            DownloadResult result;
            result.filename = file;
            result.success = false;
            result.error = "Client not connected";
            if (onFileReady) onFileReady(result);
        }
        if (onComplete) onComplete();
        return;
    }

    MIDDLEWARE_LOG_INFO("Starting streaming download of %zu files (max memory: %llu bytes, max parallel: %zu)",
                       files.size(), maxMemoryBytes, maxParallel);

    if (!running_) {
        startPollThread();
    }

    std::thread streamingThread([this, files, targetRank, maxMemoryBytes, maxParallel,
                                timeoutMs, onFileReady, onComplete]() {

        std::vector<std::string> pendingFiles = files;
        std::vector<std::unique_ptr<DownloadTask>> activeTasks;
        std::mutex activeTasksMutex;
        std::condition_variable tasksCV;

        auto startNextDownload = [&]() -> bool {
            std::lock_guard<std::mutex> lock(activeTasksMutex);

            if (pendingFiles.empty()) {
                return false; // No more files
            }

            if (maxParallel > 0 && activeTasks.size() >= maxParallel) {
                return false; // At parallel limit
            }

            if (maxMemoryBytes > 0) {
                uint64_t currentMemory = 0;
                for (const auto& task : activeTasks) {
                    currentMemory += task->downloaded;
                    currentMemory += 1024; // Overhead
                }

                uint64_t estimatedNextFile = 1024 * 1024;
                if (currentMemory + estimatedNextFile > maxMemoryBytes) {
                    return false; // Would exceed memory limit
                }
            }

            std::string filename = pendingFiles.back();
            pendingFiles.pop_back();

            auto task = std::make_unique<DownloadTask>();
            task->filename = filename;
            task->targetRank = targetRank;
            task->timeoutMs = timeoutMs;
            task->startTime = std::chrono::steady_clock::now();
            task->result.filename = filename;

            jusync_diag::Diagnostics::instance().mark(filename, jusync_diag::STAGE_DL_START);
            uint32_t requestId = client_->requestFileAsync(
                filename,
                targetRank,
                [this, taskPtr = task.get()](const std::string& fname,
                                           const std::vector<uint8_t>& chunk,
                                           uint64_t offset,
                                           uint64_t totalSize) {
                    taskPtr->downloaded += chunk.size();
                    taskPtr->totalSize = totalSize;

                    if (taskPtr->result.data.size() < offset + chunk.size()) {
                        taskPtr->result.data.resize(offset + chunk.size());
                    }
                    std::copy(chunk.begin(), chunk.end(),
                             taskPtr->result.data.begin() + offset);
                },
                [this, taskPtr = task.get()](const std::string& fname, uint64_t totalSize) {
                    taskPtr->completed = true;
                    taskPtr->result.success = true;
                    taskPtr->result.size = totalSize;
                    jusync_diag::Diagnostics::instance().mark(
                        taskPtr->filename, jusync_diag::STAGE_DL_DONE, totalSize);
                    taskPtr->result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - taskPtr->startTime);

                    if (taskPtr->result.data.size() > totalSize) {
                        taskPtr->result.data.resize(totalSize);
                    }
                },
                [this, taskPtr = task.get()](const std::string& error) {
                    taskPtr->completed = true;
                    taskPtr->result.success = false;
                    taskPtr->result.error = error;
                    taskPtr->result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - taskPtr->startTime);
                },
                timeoutMs
            );

            {
                std::lock_guard<std::mutex> lock(tasksMutex_);
                requestIdToTask_[requestId] = task.get();
                tasks_.push_back(std::move(task));
            }

            activeTasks.push_back(std::move(task));
            MIDDLEWARE_LOG_DEBUG("Started streaming download: %s", filename.c_str());

            return true;
        };

        auto checkCompletedTasks = [&]() {
            std::lock_guard<std::mutex> lock(activeTasksMutex);

            auto it = activeTasks.begin();
            while (it != activeTasks.end()) {
                auto& task = *it;

                if (task->completed) {
                    if (onFileReady) {
                        onFileReady(task->result);
                    }

                    it = activeTasks.erase(it);

                    tasksCV.notify_one();
                } else {
                    ++it;
                }
            }
        };

        size_t initialDownloads = std::min(maxParallel > 0 ? maxParallel : 4, files.size());
        for (size_t i = 0; i < initialDownloads; ++i) {
            if (!startNextDownload()) {
                break;
            }
        }

        while (!pendingFiles.empty() || !activeTasks.empty()) {
            checkCompletedTasks();

            while (startNextDownload()) {
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        MIDDLEWARE_LOG_INFO("Streaming download completed for %zu files", files.size());

        if (onComplete) {
            onComplete();
        }
    });

    streamingThread_ = std::move(streamingThread);
}

} // namespace anari_usd_middleware