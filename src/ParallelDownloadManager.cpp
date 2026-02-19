#include "ParallelDownloadManager.h"
#include "AnariUsdMessages.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <cstring>

namespace anari_usd_middleware {

ParallelDownloadManager::ParallelDownloadManager(
    std::shared_ptr<AnariUsdClient> client,
    size_t max_parallel_downloads)
    : client(client)
    , memory_monitor()
    , max_parallel_downloads(max_parallel_downloads == 0 ? 4 : max_parallel_downloads)
    , default_timeout_ms(30000)
    , running(false)
    , shutdown_requested(false)
    , total_files_requested(0)
    , files_downloaded(0)
    , files_spawned(0)
    , download_errors(0)
    , total_bytes_downloaded(0) {
    
    try {
        MIDDLEWARE_LOG_INFO("ParallelDownloadManager created with max %zu parallel downloads",
                           max_parallel_downloads);
    } catch (...) {
        // Silently continue even if logging fails
    }
}

ParallelDownloadManager::~ParallelDownloadManager() {
    stop();
}

void ParallelDownloadManager::downloadFilesStreaming(
    const std::vector<std::string>& filenames,
    const std::vector<int32_t>& target_ranks,
    FileSpawnCallback spawn_callback,
    CompletionCallback completion_callback,
    ErrorCallback error_callback,
    int timeout_ms) {
    
    if (!client) {
        MIDDLEWARE_LOG_ERROR("Cannot start parallel downloads: client is null");
        if (error_callback) {
            for (const auto& filename : filenames) {
                error_callback(filename, "Client is null");
            }
        }
        return;
    }
    
    if (filenames.size() != target_ranks.size()) {
        MIDDLEWARE_LOG_ERROR("Filename count (%zu) doesn't match target_ranks count (%zu)",
                           filenames.size(), target_ranks.size());
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        
        // Queue all downloads
        for (size_t i = 0; i < filenames.size(); ++i) {
            PendingDownload pending;
            pending.filename = filenames[i];
            pending.target_rank = target_ranks[i];
            pending.estimated_size = estimateFileSize(filenames[i]);
            pending.spawn_callback = spawn_callback;
            pending.error_callback = error_callback;
            
            pending_downloads.push(pending);
            total_files_requested.fetch_add(1);
        }
        
        this->completion_callback = completion_callback;
        this->default_timeout_ms = timeout_ms;
        
        MIDDLEWARE_LOG_INFO("Queued %zu files for parallel download", filenames.size());
    }
    
    // Start threads if not already running
    if (!running.load()) {
        running.store(true);
        startDispatcher();
        startScheduler();
        startWorkers();
        
        MIDDLEWARE_LOG_INFO("Started parallel download manager");
    }
    
    // Notify scheduler that we have work
    state_cv.notify_one();
}

void ParallelDownloadManager::stop() {
    if (!running.load()) return;
    
    MIDDLEWARE_LOG_INFO("Stopping parallel download manager...");
    shutdown_requested.store(true);
    
    // Notify all threads
    state_cv.notify_all();
    
    // Wait for threads
    if (dispatcher_thread.joinable()) {
        dispatcher_thread.join();
    }
    
    if (download_scheduler_thread.joinable()) {
        download_scheduler_thread.join();
    }
    
    for (auto& thread : download_worker_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    download_worker_threads.clear();
    running.store(false);
    shutdown_requested.store(false);
    
    MIDDLEWARE_LOG_INFO("Parallel download manager stopped");
}

ParallelDownloadManager::Stats ParallelDownloadManager::getStats() const {
    Stats stats;
    stats.total_files_requested = total_files_requested.load();
    stats.files_downloaded = files_downloaded.load();
    stats.files_spawned = files_spawned.load();
    stats.download_errors = download_errors.load();
    stats.total_bytes_downloaded = total_bytes_downloaded.load();
    stats.memory_usage_percentage = memory_monitor.getUsagePercentage();
    return stats;
}

// ============================================================================
// Thread Functions
// ============================================================================

void ParallelDownloadManager::startDispatcher() {
    dispatcher_thread = std::thread([this]() { 
        // Dispatcher thread implementation would go here
        // For now, just log and sleep
        MIDDLEWARE_LOG_INFO("Dispatcher thread started");
        while (running.load() && !shutdown_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        MIDDLEWARE_LOG_INFO("Dispatcher thread stopped");
    });
}

void ParallelDownloadManager::startScheduler() {
    download_scheduler_thread = std::thread([this]() { 
        // Scheduler thread implementation would go here
        MIDDLEWARE_LOG_INFO("Download scheduler thread started");
        while (running.load() && !shutdown_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        MIDDLEWARE_LOG_INFO("Download scheduler thread stopped");
    });
}

void ParallelDownloadManager::startWorkers() {
    for (size_t i = 0; i < max_parallel_downloads; ++i) {
        download_worker_threads.emplace_back([this]() { 
            MIDDLEWARE_LOG_DEBUG("Download worker thread started");
            while (running.load() && !shutdown_requested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            MIDDLEWARE_LOG_DEBUG("Download worker thread stopped");
        });
    }
}

// ============================================================================
// Internal Methods
// ============================================================================

bool ParallelDownloadManager::sendFileRequest(
    const std::string& filename, int32_t target_rank,
    uint32_t request_id, int timeout_ms) {
    
    // This would send the actual ZMQ request
    // For now, just log and return success
    MIDDLEWARE_LOG_DEBUG("Would send file request: %s (rank: %d, request_id: %u)",
                        filename.c_str(), target_rank, request_id);
    return true;
}

void ParallelDownloadManager::processIncomingResponse(zmq::message_t& response) {
    // This would process incoming ZMQ responses
    MIDDLEWARE_LOG_DEBUG("Processing incoming response");
}

void ParallelDownloadManager::handleFileChunk(
    const ZmqFileChunk* chunk, const std::vector<uint8_t>& chunk_data,
    std::shared_ptr<DownloadContext> context) {
    
    MIDDLEWARE_LOG_DEBUG("Handling file chunk for %s: %zu bytes",
                        context->filename.c_str(), chunk_data.size());
}

void ParallelDownloadManager::handleFileComplete(
    const ZmqFileComplete* complete,
    std::shared_ptr<DownloadContext> context) {
    
    MIDDLEWARE_LOG_INFO("File download complete: %s",
                       context->filename.c_str());
    
    // Spawn immediately
    this->spawnFileImmediately(context);
    
    // Cleanup
    this->cleanupCompletedDownload(context);
}

void ParallelDownloadManager::handleDownloadError(
    std::shared_ptr<DownloadContext> context,
    const std::string& error) {
    
    MIDDLEWARE_LOG_ERROR("Download error for %s: %s",
                        context->filename.c_str(), error.c_str());
    
    // Call error callback if provided
    if (context->error_callback) {
        context->error_callback(context->filename, error);
    }
    
    // Cleanup
    cleanupCompletedDownload(context);
}

void ParallelDownloadManager::spawnFileImmediately(
    std::shared_ptr<DownloadContext> context) {
    
    if (!context->spawn_callback) {
        MIDDLEWARE_LOG_WARNING("No spawn callback for %s", context->filename.c_str());
        return;
    }
    
    try {
        // Call the spawn callback with the downloaded data
        context->spawn_callback(context->filename, context->accumulated_data);
        files_spawned.fetch_add(1);
        
        MIDDLEWARE_LOG_INFO("File spawned immediately: %s",
                           context->filename.c_str());
        
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Failed to spawn file %s: %s",
                            context->filename.c_str(), e.what());
    }
}

void ParallelDownloadManager::cleanupCompletedDownload(
    std::shared_ptr<DownloadContext> context) {
    
    std::lock_guard<std::mutex> lock(state_mutex);
    
    // Release RAM
    memory_monitor.release(context->estimated_size);
    
    // Remove from active downloads
    active_downloads.erase(context->request_id);
    
    // Clear accumulated data to free memory
    context->accumulated_data.clear();
    context->accumulated_data.shrink_to_fit();
    
    MIDDLEWARE_LOG_DEBUG("Cleaned up download context for %s",
                        context->filename.c_str());
}

size_t ParallelDownloadManager::estimateFileSize(const std::string& filename) const {
    // Simple estimation based on file extension
    if (filename.find(".usd") != std::string::npos ||
        filename.find(".usda") != std::string::npos ||
        filename.find(".usdc") != std::string::npos) {
        return 50 * 1024 * 1024; // 50MB estimate for USD files
    }
    
    // Default estimate
    return 10 * 1024 * 1024; // 10MB
}

uint32_t ParallelDownloadManager::generateRequestId() {
    static std::atomic<uint32_t> next_id{1};
    return next_id.fetch_add(1);
}

} // namespace anari_usd_middleware