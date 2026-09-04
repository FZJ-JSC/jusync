#include "AnariUsdClient.h"
#include "MiddlewareLogging.h"

#include <regex>
#include <algorithm>
#include <thread>
#include <future>
#include <queue>
#include "../../external/nlohmann/single_include/nlohmann/json.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

using json = nlohmann::json;

namespace anari_usd_middleware {

AnariUsdClient::AnariUsdClient() {
    MIDDLEWARE_LOG_INFO("AnariUsdClient created - DEALER client for ANARI USD broker");
    connectionStats.reset();
    lastHealthCheck = std::chrono::steady_clock::now();
}

AnariUsdClient::~AnariUsdClient() {
    MIDDLEWARE_LOG_INFO("AnariUsdClient destructor called");
    disconnect(500); // Quick shutdown in destructor
}

bool AnariUsdClient::connect(const char* brokerEndpoint, int timeoutMs) {
    std::lock_guard<std::mutex> lock(connectionMutex);
    
    if (connectionStatus.load() == ConnectionStatus::Connected) {
        MIDDLEWARE_LOG_WARNING("AnariUsdClient already connected");
        return true;
    }

    MIDDLEWARE_LOG_INFO("Connecting to ANARI USD broker: %s (timeout %dms)", 
                        brokerEndpoint ? brokerEndpoint : "default", timeoutMs);
    connectionStatus.store(ConnectionStatus::Connecting);

    try {
        // Validate timeout
        if (timeoutMs <= 0 || timeoutMs > 30000) {
            MIDDLEWARE_LOG_ERROR("Invalid timeout value: %d (must be 1-30000ms)", timeoutMs);
            connectionStatus.store(ConnectionStatus::Error);
            return false;
        }

        // Initialize ZMQ context
        zmqContext = std::make_unique<zmq::context_t>(1);
        if (!zmqContext) {
            MIDDLEWARE_LOG_ERROR("Failed to create ZMQ context");
            connectionStatus.store(ConnectionStatus::Error);
            return false;
        }

        // Set context options
        zmqContext->set(zmq::ctxopt::max_sockets, 1024);
        zmqContext->set(zmq::ctxopt::io_threads, 1);

        // Create DEALER socket
        zmqSocket = std::make_unique<zmq::socket_t>(*zmqContext, zmq::socket_type::dealer);
        if (!zmqSocket) {
            MIDDLEWARE_LOG_ERROR("Failed to create ZMQ DEALER socket");
            cleanup();
            connectionStatus.store(ConnectionStatus::Error);
            return false;
        }

        // Configure socket
        if (!configureSocket(timeoutMs)) {
            MIDDLEWARE_LOG_ERROR("Failed to configure DEALER socket");
            cleanup();
            connectionStatus.store(ConnectionStatus::Error);
            return false;
        }

        // Validate and set endpoint
        std::string endpoint = brokerEndpoint ? brokerEndpoint : "tcp://localhost:5556";
        if (!validateEndpoint(endpoint)) {
            MIDDLEWARE_LOG_ERROR("Invalid broker endpoint: %s", endpoint.c_str());
            cleanup();
            connectionStatus.store(ConnectionStatus::Error);
            return false;
        }

        // Connect to broker
        try {
            zmqSocket->connect(endpoint);
            this->brokerEndpoint = endpoint;
            MIDDLEWARE_LOG_INFO("Successfully connected to ANARI USD broker: %s", endpoint.c_str());
        } catch (const zmq::error_t& e) {
            MIDDLEWARE_LOG_ERROR("Failed to connect to broker %s: %s (errno: %d)",
                                  endpoint.c_str(), e.what(), e.num());
            cleanup();
            connectionStatus.store(ConnectionStatus::Error);
            return false;
        }

        // Reset statistics
        connectionStats.reset();
        shutdownRequested.store(false);

        // Mark as connected
        connectionStatus.store(ConnectionStatus::Connected);

        // Start background dispatcher thread
        dispatcherActive.store(true);
        dispatchThread = std::thread(&AnariUsdClient::dispatcherThread, this);

        MIDDLEWARE_LOG_INFO("AnariUsdClient connected successfully");
        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error during connection: %s (errno: %d)", e.what(), e.num());
        cleanup();
        connectionStatus.store(ConnectionStatus::Error);
        return false;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception during connection: %s", e.what());
        cleanup();
        connectionStatus.store(ConnectionStatus::Error);
        return false;
    }
}

void AnariUsdClient::disconnect(int gracefulTimeoutMs) {
    std::lock_guard<std::mutex> lock(connectionMutex);
    
    if (connectionStatus.load() == ConnectionStatus::Disconnected) {
        return;
    }

    MIDDLEWARE_LOG_INFO("Disconnecting from ANARI USD broker (graceful timeout: %dms)", gracefulTimeoutMs);
    connectionStatus.store(ConnectionStatus::ShuttingDown);
    shutdownRequested.store(true);

    // Give pending requests time to complete
    if (gracefulTimeoutMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(gracefulTimeoutMs));
    }

    cleanup();
    connectionStatus.store(ConnectionStatus::Disconnected);
    MIDDLEWARE_LOG_INFO("AnariUsdClient disconnected");
}

bool AnariUsdClient::isConnected() const {
    return connectionStatus.load() == ConnectionStatus::Connected;
}

AnariUsdClient::ConnectionStatus AnariUsdClient::getConnectionStatus() const {
    return connectionStatus.load();
}

bool AnariUsdClient::configureSocket(int timeoutMs) {
    try {
        // Set socket options
        zmqSocket->set(zmq::sockopt::linger, 0);
        zmqSocket->set(zmq::sockopt::sndhwm, 1000);
        zmqSocket->set(zmq::sockopt::rcvhwm, 1000);
        zmqSocket->set(zmq::sockopt::sndtimeo, timeoutMs);
        zmqSocket->set(zmq::sockopt::rcvtimeo, timeoutMs);
        zmqSocket->set(zmq::sockopt::maxmsgsize, static_cast<int64_t>(maxMessageSize.load()));

#if PLATFORM_WINDOWS
        return configureWindowsSocket();
#elif PLATFORM_LINUX
        return configureLinuxSocket();
#else
        MIDDLEWARE_LOG_WARNING("Unknown platform - using default socket configuration");
        return true;
#endif

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("Failed to configure socket options: %s (errno: %d)", e.what(), e.num());
        return false;
    }
}

#if PLATFORM_WINDOWS
bool AnariUsdClient::configureWindowsSocket() {
    MIDDLEWARE_LOG_INFO("Applying Windows-specific ZMQ socket configuration");

    try {
        zmqSocket->set(zmq::sockopt::tcp_keepalive, 1);
        zmqSocket->set(zmq::sockopt::tcp_keepalive_idle, 300);
        zmqSocket->set(zmq::sockopt::tcp_keepalive_cnt, 3);
        zmqSocket->set(zmq::sockopt::tcp_keepalive_intvl, 30);
        // Increase buffer sizes for broadcast reliability (16 ranks × 8 files × ~10KB each = ~1.3MB)
        zmqSocket->set(zmq::sockopt::sndbuf, 2097152);  // 2MB send buffer
        zmqSocket->set(zmq::sockopt::rcvbuf, 4194304);  // 4MB receive buffer for burst responses

        MIDDLEWARE_LOG_INFO("Windows ZMQ socket configuration applied successfully");
        return true;
    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("Windows socket configuration failed: %s (errno: %d)", e.what(), e.num());
        return false;
    }
}
#endif

#if PLATFORM_LINUX
bool AnariUsdClient::configureLinuxSocket() {
    MIDDLEWARE_LOG_INFO("Applying Linux-specific ZMQ socket configuration");

    try {
        zmqSocket->set(zmq::sockopt::tcp_keepalive, 1);
        zmqSocket->set(zmq::sockopt::tcp_keepalive_idle, 600);
        zmqSocket->set(zmq::sockopt::tcp_keepalive_cnt, 5);
        zmqSocket->set(zmq::sockopt::tcp_keepalive_intvl, 60);
        zmqSocket->set(zmq::sockopt::sndbuf, 1048576);
        zmqSocket->set(zmq::sockopt::rcvbuf, 1048576);

        MIDDLEWARE_LOG_INFO("Linux ZMQ socket configuration applied successfully");
        return true;
    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("Linux socket configuration failed: %s (errno: %d)", e.what(), e.num());
        return false;
    }
}
#endif

bool AnariUsdClient::validateEndpoint(const std::string& endpoint) const {
    if (endpoint.empty()) {
        return false;
    }

    // TCP endpoint validation: tcp://host:port
    if (endpoint.find("tcp://") == 0) {
        std::regex tcpPattern(R"(^tcp://([^:]+|localhost):(\d+)$)");
        std::smatch matches;

        if (!std::regex_match(endpoint, matches, tcpPattern)) {
            return false;
        }

        // Validate port range
        int port = std::stoi(matches[2]);
        return (port > 0 && port <= 65535);
    }

    return false;
}

bool AnariUsdClient::requestFileList(int32_t targetRank, FileListCallback callback, int timeoutMs) {
    if (connectionStatus.load() != ConnectionStatus::Connected || !zmqSocket) {
        MIDDLEWARE_LOG_ERROR("AnariUsdClient not connected");
        return false;
    }

    try {
        // Create file list request
        ZmqFileRequest request;
        request.magic = ANARI_USD_MAGIC;           // 0x55534446 ("USDF") - CRITICAL: Must be set!
        request.message_type = static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES);
        request.request_id = generateRequestId();
        request.target_rank = targetRank;
        request.chunk_size = 0; // Not applicable for file list
        request.setFilename(""); // No filename for list request

        MIDDLEWARE_LOG_INFO("Requesting file list from rank %d (request_id: %u)", 
                            targetRank, request.request_id);
        
        // DEBUG: Log request details
        MIDDLEWARE_LOG_INFO("DEBUG: File list request - magic=0x%08x, type=%u, size=%zu bytes",
                           request.magic, request.message_type, sizeof(request));

        // Send request
        if (!sendRequest(&request, sizeof(request), request.request_id)) {
            MIDDLEWARE_LOG_ERROR("Failed to send file list request");
            return false;
        }
        
        MIDDLEWARE_LOG_INFO("DEBUG: File list request sent successfully, waiting for response...");

        // Receive file list response — FILTER BY request_id
        uint32_t myRequestId = request.request_id;
        size_t headerSize = sizeof(ZmqFileChunk);

        // Receive chunk header + JSON
        FrameMsg dummyDelim1, dataFrame1;
        if (!waitForMatchingFrames(myRequestId, timeoutMs, dummyDelim1, dataFrame1)) {
            MIDDLEWARE_LOG_ERROR("Timeout waiting for file list response");
            return false;
        }

        if (dataFrame1->size() < headerSize) {
            MIDDLEWARE_LOG_ERROR("File list response too small: %zu bytes (expected at least %zu)",
                                 dataFrame1->size(), headerSize);
            return false;
        }

        const uint8_t* chunkData = static_cast<const uint8_t*>(dataFrame1->data());
        const ZmqFileChunk* chunk = reinterpret_cast<const ZmqFileChunk*>(chunkData);

        if (!MessageUtils::isValidMagic(chunk->magic)) {
            MIDDLEWARE_LOG_ERROR("Invalid magic number in file list response");
            return false;
        }

        if (chunk->message_type != static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK)) {
            MIDDLEWARE_LOG_ERROR("Unexpected message type in file list response: %u (expected RESP_FILE_CHUNK=201)",
                                chunk->message_type);
            return false;
        }

        std::string filename = chunk->getFilename();
        if (filename != "filelist.json") {
            MIDDLEWARE_LOG_WARNING("File list response has unexpected filename: %s", filename.c_str());
        }

        MIDDLEWARE_LOG_INFO("Received file list chunk header: %llu bytes from rank %d, chunk size: %u",
                            chunk->file_size, chunk->source_rank, chunk->chunk_size);

        std::vector<uint8_t> jsonData;
        if (chunk->chunk_size > 0) {
            size_t expectedTotalSize = headerSize + chunk->chunk_size;
            if (dataFrame1->size() < expectedTotalSize) {
                MIDDLEWARE_LOG_ERROR("Incomplete file list response: got %zu bytes, expected %zu",
                                     dataFrame1->size(), expectedTotalSize);
                return false;
            }
            jsonData.assign(chunkData + headerSize, chunkData + headerSize + chunk->chunk_size);
            MIDDLEWARE_LOG_INFO("Extracted JSON data: %zu bytes", jsonData.size());
        } else {
            MIDDLEWARE_LOG_WARNING("File list response has zero chunk size");
        }

        std::vector<std::string> files;
        try {
            std::string jsonStr(reinterpret_cast<const char*>(jsonData.data()), jsonData.size());
            auto json = json::parse(jsonStr);

            if (json.contains("files") && json["files"].is_array()) {
                for (const auto& fileObj : json["files"]) {
                    if (fileObj.contains("name") && fileObj["name"].is_string()) {
                        std::string fname = fileObj["name"].get<std::string>();
                        if (!fname.empty()) {
                            files.push_back(fname);
                        }
                    }
                }
            }

            MIDDLEWARE_LOG_INFO("Parsed %zu files from JSON file list", files.size());
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Failed to parse file list JSON: %s", e.what());
            return false;
        }

        // Receive completion message — also filtered by request_id
        FrameMsg dummyDelim2, dataFrame2;
        if (!waitForMatchingFrames(myRequestId, timeoutMs, dummyDelim2, dataFrame2)) {
            MIDDLEWARE_LOG_ERROR("Timeout waiting for file list completion");
        } else if (dataFrame2->size() >= sizeof(ZmqFileComplete)) {
            const ZmqFileComplete* complete = reinterpret_cast<const ZmqFileComplete*>(dataFrame2->data());
            if (complete->message_type != static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE)) {
                MIDDLEWARE_LOG_WARNING("Expected RESP_FILE_COMPLETE after file list, got: %u",
                                      complete->message_type);
            }
        }

        // Trigger callback
        if (callback) {
            callback(files);
        }

        connectionStats.totalResponsesReceived.fetch_add(1);
        connectionStats.lastActivityTime = std::chrono::steady_clock::now();

        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error in requestFileList: %s (errno: %d)", e.what(), e.num());
        return false;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in requestFileList: %s", e.what());
        return false;
    }
}

bool AnariUsdClient::requestFileListWithSizes(int32_t targetRank, FileListWithSizesCallback callback, int timeoutMs) {
    if (connectionStatus.load() != ConnectionStatus::Connected || !zmqSocket) {
        MIDDLEWARE_LOG_ERROR("AnariUsdClient not connected");
        return false;
    }

    try {
        // For broadcast requests (targetRank == -1), we need to receive multiple responses
        // For single rank requests, we just process one response
        bool isBroadcast = (targetRank == -1);
        
        // CRITICAL FIX: Query total worker count BEFORE sending request
        // In single-rank mode, convert broadcast to direct request to rank 0
        uint32_t totalWorkers = 1; // Default to single-rank mode (non-MPI)
        int32_t actualTargetRank = targetRank;
        
        if (isBroadcast) {
            uint32_t workerCount = 0;
            if (getTotalWorkerCountSync(workerCount, 2000)) {
                totalWorkers = workerCount;
                MIDDLEWARE_LOG_INFO("Dynamic worker count from broker: %u", totalWorkers);
                
                // SINGLE-RANK MODE FIX: If only rank 0 exists, convert broadcast to direct request
                if (totalWorkers == 1) {
                    MIDDLEWARE_LOG_INFO("Single-rank mode detected - converting broadcast (-1) to direct request to rank 0");
                    actualTargetRank = 0; // Change target from -1 to 0
                    isBroadcast = false;  // Disable broadcast mode
                }
            } else {
                MIDDLEWARE_LOG_WARNING("Failed to get worker count from broker, using default single-rank mode");
                // Keep original targetRank (could be -1 or specific rank)
            }
        }

        // Create file list request with CORRECTED target rank
        ZmqFileRequest request;
        request.magic = ANARI_USD_MAGIC;           // 0x55534446 ("USDF") - CRITICAL: Must be set!
        request.message_type = static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES);
        request.request_id = generateRequestId();
        request.target_rank = actualTargetRank;    // Use corrected target rank
        request.chunk_size = 0; // Not applicable for file list
        request.setFilename(""); // No filename for list request

        MIDDLEWARE_LOG_INFO("Requesting file list with sizes from rank %d (request_id: %u, isBroadcast: %d)",
                            actualTargetRank, request.request_id, isBroadcast ? 1 : 0);

        // Send request
        if (!sendRequest(&request, sizeof(request), request.request_id)) {
            MIDDLEWARE_LOG_ERROR("Failed to send file list request");
            return false;
        }

        std::vector<FileInfo> allFileInfos;
        auto startTime = std::chrono::steady_clock::now();
        auto timeoutDuration = std::chrono::milliseconds(timeoutMs);
        uint32_t broadcastRequestId = request.request_id;
        bool receivedAtLeastOneResponse = false;

        // Track which ranks have responded for broadcast requests
        std::vector<bool> respondedRanks(totalWorkers, false);

        do {
            auto currentTime = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
            if (elapsed >= timeoutDuration) {
                MIDDLEWARE_LOG_INFO("Timeout reached after %lld ms", elapsed.count());
                break;
            }

            int remainingTimeout = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(timeoutDuration - elapsed).count());
            if (remainingTimeout <= 0) break;

            // For broadcast: check if all ranks have responded
            if (isBroadcast) {
                int respondedCount = 0;
                for (int32_t rank = 0; rank < static_cast<int32_t>(totalWorkers); ++rank) {
                    if (respondedRanks[rank]) respondedCount++;
                }
                if (respondedCount >= static_cast<int32_t>(totalWorkers)) {
                    MIDDLEWARE_LOG_INFO("All %d ranks have responded - stopping broadcast loop", totalWorkers);
                    break;
                }
            }

            // Receive chunk header (filtered by request_id)
            FrameMsg dummyDelim1, dataFrame1;
            if (!waitForMatchingFrames(broadcastRequestId, remainingTimeout, dummyDelim1, dataFrame1)) {
                break; // timeout or shutdown
            }

            size_t totalSize = dataFrame1->size();
            size_t headerSize = sizeof(ZmqFileChunk);

            if (totalSize < headerSize) {
                MIDDLEWARE_LOG_ERROR("File list response too small: %zu bytes (expected at least %zu)",
                                    totalSize, headerSize);
                continue;
            }

            const uint8_t* chunkData = static_cast<const uint8_t*>(dataFrame1->data());
            const ZmqFileChunk* chunk = reinterpret_cast<const ZmqFileChunk*>(chunkData);

            if (!MessageUtils::isValidMagic(chunk->magic)) {
                MIDDLEWARE_LOG_ERROR("Invalid magic number in file list response");
                continue;
            }

            if (chunk->message_type != static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK)) {
                MIDDLEWARE_LOG_ERROR("Unexpected message type in file list response: %u (expected RESP_FILE_CHUNK=201)",
                                    chunk->message_type);
                continue;
            }

            std::string filename = chunk->getFilename();
            if (filename != "filelist.json") {
                MIDDLEWARE_LOG_WARNING("File list response has unexpected filename: %s", filename.c_str());
            }

            MIDDLEWARE_LOG_INFO("Received file list chunk header: %llu bytes from rank %d, chunk size: %u",
                                chunk->file_size, chunk->source_rank, chunk->chunk_size);

            std::vector<uint8_t> jsonData;
            if (chunk->chunk_size > 0) {
                size_t expectedTotalSize = headerSize + chunk->chunk_size;
                if (totalSize < expectedTotalSize) {
                    MIDDLEWARE_LOG_ERROR("Incomplete file list response: got %zu bytes, expected %zu",
                                        totalSize, expectedTotalSize);
                    continue;
                }
                jsonData.assign(chunkData + headerSize, chunkData + headerSize + chunk->chunk_size);
                MIDDLEWARE_LOG_INFO("Extracted JSON data: %zu bytes", jsonData.size());
            } else {
                MIDDLEWARE_LOG_WARNING("File list response has zero chunk size");
            }

            try {
                std::string jsonStr(reinterpret_cast<const char*>(jsonData.data()), jsonData.size());
                auto json = json::parse(jsonStr);
                int32_t jsonRank = chunk->source_rank;

                if (json.contains("files") && json["files"].is_array()) {
                    for (const auto& fileObj : json["files"]) {
                        if (fileObj.contains("name") && fileObj["name"].is_string()) {
                                std::string fname = fileObj["name"].get<std::string>();
                                if (!fname.empty()) {
                                    uint64_t fsize = 0;
                                    uint64_t hashLo = 0;
                                    uint64_t hashHi = 0;
                                    if (fileObj.contains("size") && fileObj["size"].is_number()) {
                                        fsize = fileObj["size"].get<uint64_t>();
                                    }
                                    if (fileObj.contains("hash_lo") && fileObj["hash_lo"].is_number()) {
                                        hashLo = fileObj["hash_lo"].get<uint64_t>();
                                    }
                                    if (fileObj.contains("hash_hi") && fileObj["hash_hi"].is_number()) {
                                        hashHi = fileObj["hash_hi"].get<uint64_t>();
                                    }
                                    FileInfo fi(fname, fsize, jsonRank);
                                    fi.hash128[0] = hashLo;
                                    fi.hash128[1] = hashHi;
                                    allFileInfos.push_back(std::move(fi));
                                }
                        }
                    }
                }
                MIDDLEWARE_LOG_INFO("Parsed files from rank %d", jsonRank);

                // Mark this rank as responded
                if (isBroadcast && jsonRank >= 0 && jsonRank < static_cast<int32_t>(totalWorkers)) {
                    respondedRanks[jsonRank] = true;
                    MIDDLEWARE_LOG_INFO("Rank %d responded (%d/%d total)", jsonRank,
                        std::count(respondedRanks.begin(), respondedRanks.end(), true), totalWorkers);
                }
            } catch (const std::exception& e) {
                MIDDLEWARE_LOG_ERROR("Failed to parse file list JSON: %s", e.what());
                continue;
            }

            // Receive completion message (filtered by request_id)
            FrameMsg dummyDelim2, dataFrame2;
            if (!waitForMatchingFrames(broadcastRequestId, remainingTimeout, dummyDelim2, dataFrame2)) {
                MIDDLEWARE_LOG_WARNING("Timeout waiting for completion from rank %d", chunk->source_rank);
            } else if (dataFrame2->size() >= sizeof(ZmqFileComplete)) {
                const ZmqFileComplete* complete = reinterpret_cast<const ZmqFileComplete*>(dataFrame2->data());
                if (complete->message_type != static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE)) {
                    MIDDLEWARE_LOG_WARNING("Expected RESP_FILE_COMPLETE after file list, got: %u",
                                          complete->message_type);
                }
            }

            receivedAtLeastOneResponse = true;
            connectionStats.totalResponsesReceived.fetch_add(1);

            if (!isBroadcast) {
                break;
            }

            // Try next rank — short timeout to avoid blocking on stragglers
            // Use a small per-rank timeout so 100 ranks don't take forever
        } while (isBroadcast);
        
        if (!receivedAtLeastOneResponse) {
            MIDDLEWARE_LOG_ERROR("No valid file list responses received");
            return false;
        }
        
        MIDDLEWARE_LOG_INFO("Accumulated %zu total files from %s",
                           allFileInfos.size(),
                           isBroadcast ? "multiple ranks (broadcast)" : "single rank");
        
        // For broadcast: implement retry logic for missing ranks (MPI mode only)
        // In single-rank mode (non-MPI), there's only rank 0, so no retry needed
        if (isBroadcast && allFileInfos.size() > 0) {
            // Use the respondedRanks vector that was already populated during the main loop
            int32_t maxRank = -1;
            int32_t minRank = static_cast<int32_t>(totalWorkers);
            
            for (int32_t rank = 0; rank < static_cast<int32_t>(totalWorkers); ++rank) {
                if (respondedRanks[rank]) {
                    if (rank > maxRank) maxRank = rank;
                    if (rank < minRank) minRank = rank;
                }
            }
            
            // Count responded ranks
            int32_t respondedCount = 0;
            for (int32_t rank = 0; rank < static_cast<int32_t>(totalWorkers); ++rank) {
                if (respondedRanks[rank]) respondedCount++;
            }
            
            MIDDLEWARE_LOG_INFO("Broadcast response summary: %d/%d ranks responded (ranks %d-%d)",
                                respondedCount, totalWorkers, minRank, maxRank);
            
            // Only retry missing ranks if we're in multi-rank MPI mode
            // In single-rank mode (totalWorkers == 1), don't retry
            bool isSingleRankMode = (totalWorkers == 1);
            
            if (!isSingleRankMode && respondedCount < static_cast<int32_t>(totalWorkers)) {
                MIDDLEWARE_LOG_INFO("Multi-rank mode detected - retrying %d missing ranks...",
                                    totalWorkers - respondedCount);
                
                // Retry each missing rank
                int32_t retryTimeout = timeoutMs / 3; // Shorter timeout for retries
                if (retryTimeout < 5000) retryTimeout = 5000; // Minimum 5 seconds
                
                // Create a temporary vector for retry results
                std::vector<FileInfo> retryResults = allFileInfos;
                
                for (int32_t rank = 0; rank < static_cast<int32_t>(totalWorkers); ++rank) {
                    if (!respondedRanks[rank]) {
                        MIDDLEWARE_LOG_INFO("Retrying rank %d with %d ms timeout", rank, retryTimeout);
                        
                        // Inline retry logic (simplified version of requestFileListForSingleRank)
                        std::vector<FileInfo> rankFiles;
                        
                        // Check connection
                        if (connectionStatus.load() != ConnectionStatus::Connected || !zmqSocket) {
                            MIDDLEWARE_LOG_ERROR("Not connected for retry request");
                            continue;
                        }
                        
                        try {
                            // Create file list request
                            ZmqFileRequest request;
                            request.magic = ANARI_USD_MAGIC;  // CRITICAL: Set magic number
                            request.message_type = static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES);
                            request.request_id = generateRequestId();
                            request.target_rank = rank;
                            request.chunk_size = 0;
                            request.setFilename("");
                            
                            MIDDLEWARE_LOG_INFO("Retry: requesting file list from rank %d (request_id: %u)",
                                               rank, request.request_id);
                            
                            // Send request
                            if (!sendRequest(&request, sizeof(request), request.request_id)) {
                                MIDDLEWARE_LOG_ERROR("Retry failed to send request to rank %d", rank);
                                continue;
                            }
                            
                            uint32_t retryRequestId = request.request_id;

                            // Receive chunk header (filtered by request_id)
                            FrameMsg dummyDelim1, dataFrame1;
                            if (!waitForMatchingFrames(retryRequestId, retryTimeout, dummyDelim1, dataFrame1)) {
                                MIDDLEWARE_LOG_WARNING("Retry timeout for rank %d after %d ms", rank, retryTimeout);
                                continue;
                            }

                            size_t totalSize = dataFrame1->size();
                            size_t headerSize = sizeof(ZmqFileChunk);
                            if (totalSize < headerSize) {
                                MIDDLEWARE_LOG_ERROR("Retry response too small from rank %d: %zu bytes", rank, totalSize);
                                continue;
                            }

                            const uint8_t* msgData = static_cast<const uint8_t*>(dataFrame1->data());
                            const ZmqFileChunk* chunk = reinterpret_cast<const ZmqFileChunk*>(msgData);

                            if (!MessageUtils::isValidMagic(chunk->magic)) {
                                MIDDLEWARE_LOG_ERROR("Retry invalid magic from rank %d", rank);
                                continue;
                            }

                            if (chunk->message_type != static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK)) {
                                MIDDLEWARE_LOG_ERROR("Retry wrong message type from rank %d: %u", rank, chunk->message_type);
                                continue;
                            }

                            if (chunk->chunk_size > 0 && totalSize >= headerSize + chunk->chunk_size) {
                                std::vector<uint8_t> jsonData(msgData + headerSize, msgData + headerSize + chunk->chunk_size);

                                try {
                                    std::string jsonStr(reinterpret_cast<const char*>(jsonData.data()), jsonData.size());
                                    auto json = json::parse(jsonStr);

                                    if (json.contains("files") && json["files"].is_array()) {
                                        for (const auto& fileObj : json["files"]) {
                                            if (fileObj.contains("name") && fileObj["name"].is_string()) {
                                                std::string fname = fileObj["name"].get<std::string>();
                                                if (!fname.empty()) {
                                                    uint64_t fsize = 0;
                                                    if (fileObj.contains("size") && fileObj["size"].is_number()) {
                                                        fsize = fileObj["size"].get<uint64_t>();
                                                    }
                                                    rankFiles.push_back({fname, fsize, rank});
                                                }
                                            }
                                        }
                                    }
                                } catch (const std::exception& e) {
                                    MIDDLEWARE_LOG_ERROR("Retry JSON parse error from rank %d: %s", rank, e.what());
                                }
                            }

                            // Clean up remaining completion message
                            FrameMsg dummyDelim2, dataFrame2;
                            waitForMatchingFrames(retryRequestId, std::min(retryTimeout / 3, 2000), dummyDelim2, dataFrame2);

                            MIDDLEWARE_LOG_INFO("Retry successful for rank %d: got %zu files", rank, rankFiles.size());
                            
                        } catch (const std::exception& e) {
                            MIDDLEWARE_LOG_ERROR("Exception in retry for rank %d: %s", rank, e.what());
                        }
                        
                        // Add retried files to results
                        for (const auto& fileInfo : rankFiles) {
                            retryResults.push_back(fileInfo);
                        }
                        
                        if (rankFiles.empty()) {
                            MIDDLEWARE_LOG_WARNING("Retry failed for rank %d", rank);
                        }
                    }
                }
                
                // Update allFileInfos with retry results
                allFileInfos = retryResults;
                MIDDLEWARE_LOG_INFO("After retries: %zu total files", allFileInfos.size());
            } else if (isSingleRankMode) {
                MIDDLEWARE_LOG_INFO("Single-rank mode detected - no retry needed (only rank 0 exists)");
            }
        }
        
        // Trigger callback
        if (callback) {
            callback(allFileInfos);
        }

        connectionStats.lastActivityTime = std::chrono::steady_clock::now();

        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error in requestFileListWithSizes: %s (errno: %d)", e.what(), e.num());
        return false;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in requestFileListWithSizes: %s", e.what());
        return false;
    }
}


bool AnariUsdClient::requestFile(const std::string& filename, int32_t targetRank,
                                  FileChunkCallback chunkCallback,
                                  FileCompleteCallback completeCallback,
                                  ErrorCallback errorCallback,
                                  int timeoutMs) {
    if (connectionStatus.load() != ConnectionStatus::Connected || !zmqSocket) {
        MIDDLEWARE_LOG_ERROR("AnariUsdClient not connected");
        return false;
    }

    // Validate filename
    if (filename.empty()) {
        MIDDLEWARE_LOG_ERROR("Cannot request file - filename is empty");
        if (errorCallback) {
            errorCallback("Filename is empty");
        }
        return false;
    }

    // ✅ FIX: Removed outer requestMutex lock - sendRequest() handles its own locking
    // Holding lock during blocking recv loop serialized ALL network I/O and caused hangs

    try {
        // Create file request
        ZmqFileRequest request;
        request.magic = ANARI_USD_MAGIC;
        request.message_type = static_cast<uint32_t>(ZmqMessageType::REQ_GET_FILE);
        request.request_id = generateRequestId();
        request.target_rank = targetRank;
        request.chunk_size = DEFAULT_CHUNK_SIZE;
        request.setFilename(filename);

        MIDDLEWARE_LOG_DEBUG("Requesting file '%s' from rank %d (request_id: %u)", 
                            filename.c_str(), targetRank, request.request_id);

        // Send request
        if (!sendRequest(&request, sizeof(request), request.request_id)) {
            MIDDLEWARE_LOG_ERROR("Failed to send file request");
            return false;
        }

        // Receive file in chunks — FILTER BY request_id to avoid cross-talk
        bool fileComplete = false;
        uint64_t totalSize = 0;
        uint32_t myRequestId = request.request_id;

        while (!fileComplete && !shutdownRequested.load()) {
            // Receive delimiter + data frames, filtered by request_id
            FrameMsg dummyDelim, dataFrame;
            if (!waitForMatchingFrames(myRequestId, timeoutMs, dummyDelim, dataFrame)) {
                MIDDLEWARE_LOG_ERROR("Timeout waiting for file chunk (request_id %u)", myRequestId);
                if (errorCallback) {
                    errorCallback("Timeout waiting for file chunk");
                }
                return false;
            }

            if (dataFrame->size() < 8) {
                MIDDLEWARE_LOG_ERROR("Data frame too small: %zu bytes", dataFrame->size());
                continue; // skip malformed, try next
            }

            const uint8_t* combinedData = static_cast<const uint8_t*>(dataFrame->data());
            uint32_t magic = *reinterpret_cast<const uint32_t*>(combinedData);
            uint32_t messageType = *reinterpret_cast<const uint32_t*>(combinedData + 4);
            size_t combinedSize = dataFrame->size();

            if (!MessageUtils::isValidMagic(magic)) {
                MIDDLEWARE_LOG_ERROR("Invalid magic number in response: 0x%08X", magic);
                continue; // skip, try next
            }

            switch (static_cast<ZmqMessageType>(messageType)) {
                case ZmqMessageType::RESP_FILE_CHUNK: {
                    if (combinedSize < sizeof(ZmqFileChunk)) {
                        MIDDLEWARE_LOG_ERROR("File chunk response too small: %zu bytes (expected %zu)",
                                            combinedSize, sizeof(ZmqFileChunk));
                        break;
                    }
                    const ZmqFileChunk* chunk = reinterpret_cast<const ZmqFileChunk*>(combinedData);
                    size_t dataSize = combinedSize - sizeof(ZmqFileChunk);
                    if (dataSize != chunk->chunk_size) {
                        MIDDLEWARE_LOG_WARNING("Chunk size mismatch: header %u, data %zu",
                                              chunk->chunk_size, dataSize);
                    }

                    // Zero-copy: hand the chunk payload straight from the
                    // ZMQ buffer to the callback (no intermediate vector).
                    if (chunkCallback) {
                        chunkCallback(chunk->getFilename(),
                                      combinedData + sizeof(ZmqFileChunk),
                                      dataSize, chunk->chunk_offset, chunk->file_size);
                    }

                    connectionStats.totalBytesReceived.fetch_add(dataSize);
                    totalSize = chunk->file_size;
                    break;
                }

                case ZmqMessageType::RESP_FILE_COMPLETE: {
                    if (combinedSize < sizeof(ZmqFileComplete)) {
                        MIDDLEWARE_LOG_ERROR("File complete response too small: %zu bytes (expected %zu)",
                                            combinedSize, sizeof(ZmqFileComplete));
                        break;
                    }
                    const ZmqFileComplete* complete = reinterpret_cast<const ZmqFileComplete*>(combinedData);

                    fileComplete = true;
                    if (completeCallback) {
                        completeCallback(complete->getFilename(), complete->total_size);
                    }

                    MIDDLEWARE_LOG_DEBUG("File transfer complete: %s (%zu bytes)",
                                        complete->getFilename().c_str(), complete->total_size);
                    break;
                }

                case ZmqMessageType::RESP_NO_FILE: {
                    MIDDLEWARE_LOG_ERROR("File not found: %s", filename.c_str());
                    if (errorCallback) {
                        errorCallback("File not found: " + filename);
                    }
                    return false;
                }

                case ZmqMessageType::RESP_ERROR: {
                    if (combinedSize < sizeof(ZmqErrorResponse)) {
                        MIDDLEWARE_LOG_ERROR("Error response too small: %zu bytes (expected %zu)",
                                            combinedSize, sizeof(ZmqErrorResponse));
                        break;
                    }
                    const ZmqErrorResponse* error = reinterpret_cast<const ZmqErrorResponse*>(combinedData);

                    MIDDLEWARE_LOG_ERROR("Error response: %s", error->getErrorMessage().c_str());
                    if (errorCallback) {
                        errorCallback(error->getErrorMessage());
                    }
                    return false;
                }

                default:
                    MIDDLEWARE_LOG_WARNING("Unknown message type: %u", messageType);
                    break;
            }

            connectionStats.totalResponsesReceived.fetch_add(1);
        }

        connectionStats.lastActivityTime = std::chrono::steady_clock::now();
        return fileComplete;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error in requestFile: %s (errno: %d)", e.what(), e.num());
        return false;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in requestFile: %s", e.what());
        return false;
    }
}

bool AnariUsdClient::requestFrame(int32_t frameNumber, int32_t targetRank,
                                   FileChunkCallback chunkCallback,
                                   FileCompleteCallback completeCallback,
                                   ErrorCallback errorCallback,
                                   int timeoutMs) {
    if (connectionStatus.load() != ConnectionStatus::Connected || !zmqSocket) {
        MIDDLEWARE_LOG_ERROR("AnariUsdClient not connected");
        return false;
    }

    // ✅ FIX: Removed outer requestMutex lock - sendRequest() handles its own locking
    // Holding lock during blocking recv loop serialized ALL network I/O and caused hangs

    try {
        // Create frame request
        ZmqFileRequest request;
        request.message_type = static_cast<uint32_t>(ZmqMessageType::REQ_GET_FRAME);
        request.request_id = generateRequestId();
        request.target_rank = targetRank;
        request.chunk_size = DEFAULT_CHUNK_SIZE;
        
        // Encode frame number in filename
        std::string frameFilename = "frame_" + std::to_string(frameNumber);
        request.setFilename(frameFilename);

        MIDDLEWARE_LOG_DEBUG("Requesting frame %d from rank %d (request_id: %u)", 
                            frameNumber, targetRank, request.request_id);

        // Send request
        if (!sendRequest(&request, sizeof(request), request.request_id)) {
            MIDDLEWARE_LOG_ERROR("Failed to send frame request");
            return false;
        }

        // Receive frame files from the dispatcher-backed response index.  This used
        // to recv() directly from the socket here, which raced the dispatcher thread
        // (the sole recv owner) and could steal/desync frames.  Now requestFrame
        // matches its own request_id from the index exactly like requestFile.
        //
        // A frame spans multiple files with no single terminal marker, so — as before —
        // we treat the frame as delivered once the broker stays quiet for `timeoutMs`
        // and keep the original return-value semantics (quiescence returns false).
        uint32_t myRequestId = request.request_id;
        int filesReceived = 0;

        auto drain = [this, myRequestId]() {
            std::lock_guard<std::mutex> lock(responseQueueMutex);
            auto it = responseByRequest.find(myRequestId);
            if (it != responseByRequest.end()) {
                totalQueuedFrames.fetch_sub(it->second.size(), std::memory_order_relaxed);
                responseByRequest.erase(it); // release any not-yet-consumed frames
            }
        };

        while (!shutdownRequested.load()) {
            FrameMsg dummyDelim, dataFrame;
            if (!waitForMatchingFrames(myRequestId, timeoutMs, dummyDelim, dataFrame)) {
                drain();
                MIDDLEWARE_LOG_ERROR("Timeout waiting for frame data");
                if (errorCallback) {
                    errorCallback("Timeout waiting for frame data");
                }
                return false;
            }

            if (dataFrame->size() < 8) {
                continue; // skip malformed frame
            }

            const uint8_t* combinedData = static_cast<const uint8_t*>(dataFrame->data());
            uint32_t magic = *reinterpret_cast<const uint32_t*>(combinedData);
            uint32_t messageType = *reinterpret_cast<const uint32_t*>(combinedData + 4);
            size_t combinedSize = dataFrame->size();

            if (!MessageUtils::isValidMagic(magic)) {
                MIDDLEWARE_LOG_ERROR("Invalid magic number in frame response: 0x%08X", magic);
                continue;
            }

            switch (static_cast<ZmqMessageType>(messageType)) {
                case ZmqMessageType::RESP_FILE_CHUNK: {
                    if (combinedSize < sizeof(ZmqFileChunk)) {
                        MIDDLEWARE_LOG_ERROR("Frame chunk response too small: %zu bytes", combinedSize);
                        break;
                    }
                    const ZmqFileChunk* chunk = reinterpret_cast<const ZmqFileChunk*>(combinedData);
                    size_t dataSize = combinedSize - sizeof(ZmqFileChunk);
                    if (dataSize != chunk->chunk_size) {
                        MIDDLEWARE_LOG_WARNING("Chunk size mismatch: header %u, data %zu",
                                               chunk->chunk_size, dataSize);
                    }
                    // Zero-copy: hand the chunk payload straight from the
                    // ZMQ buffer to the callback (no intermediate vector).
                    if (chunkCallback) {
                        chunkCallback(chunk->getFilename(),
                                      combinedData + sizeof(ZmqFileChunk),
                                      dataSize, chunk->chunk_offset, chunk->file_size);
                    }
                    connectionStats.totalBytesReceived.fetch_add(dataSize);
                    break;
                }

                case ZmqMessageType::RESP_FILE_COMPLETE: {
                    if (combinedSize < sizeof(ZmqFileComplete)) {
                        MIDDLEWARE_LOG_ERROR("Frame complete response too small: %zu bytes", combinedSize);
                        break;
                    }
                    const ZmqFileComplete* complete = reinterpret_cast<const ZmqFileComplete*>(combinedData);
                    filesReceived++;
                    if (completeCallback) {
                        completeCallback(complete->getFilename(), complete->total_size);
                    }
                    MIDDLEWARE_LOG_DEBUG("Frame file %d complete: %s (%zu bytes)",
                                        filesReceived, complete->getFilename().c_str(), complete->total_size);

                    // Terminal frame marker from the broker: every file of this
                    // frame has already been delivered (wire ordering), so the
                    // frame is definitively complete. Return immediately instead
                    // of waiting out the legacy "broker stays quiet" quiescence
                    // heuristic (which cost a full timeout after every frame).
                    if (complete->getFilename() == "__frame_complete__") {
                        MIDDLEWARE_LOG_INFO("Frame %d complete marker received (%d file completes)",
                                            frameNumber, filesReceived);
                        return true;
                    }
                    break;
                }

                case ZmqMessageType::RESP_NO_FILE: {
                    MIDDLEWARE_LOG_ERROR("Frame not found: %d", frameNumber);
                    if (errorCallback) {
                        errorCallback("Frame not found: " + std::to_string(frameNumber));
                    }
                    drain();
                    return false;
                }

                case ZmqMessageType::RESP_ERROR: {
                    MIDDLEWARE_LOG_ERROR("Error response in frame request");
                    if (errorCallback) {
                        errorCallback("Error response in frame request");
                    }
                    drain();
                    return false;
                }

                default:
                    MIDDLEWARE_LOG_WARNING("Unknown frame message type: %u", messageType);
                    break;
            }

            connectionStats.totalResponsesReceived.fetch_add(1);
        }

        // Shutdown requested while in flight.
        drain();
        return false;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error in requestFrame: %s (errno: %d)", e.what(), e.num());
        return false;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in requestFrame: %s", e.what());
        return false;
    }
}

bool AnariUsdClient::getFileSync(const std::string& filename, int32_t targetRank,
                                  std::vector<uint8_t>& fileData, int timeoutMs) {
    struct FileData {
        std::vector<uint8_t> data;
        uint64_t totalSize = 0;
        bool complete = false;
    };

    FileData file;
    
    auto chunkCallback = [&file](const std::string& fname, const uint8_t* chunk,
                                  size_t chunkSize, uint64_t offset, uint64_t totalSize) {
        file.totalSize = totalSize;
        if (file.data.size() < offset + chunkSize) {
            file.data.resize(offset + chunkSize);
        }
        std::memcpy(file.data.data() + offset, chunk, chunkSize);
    };

    auto completeCallback = [&file](const std::string& fname, uint64_t totalSize) {
        file.complete = true;
        file.data.resize(totalSize);
    };

    if (!requestFile(filename, targetRank, chunkCallback, completeCallback, nullptr, timeoutMs)) {
        return false;
    }

    fileData = std::move(file.data);
    return file.complete;
}

bool AnariUsdClient::getFileListSync(int32_t targetRank, std::vector<std::string>& files, int timeoutMs) {
    bool received = false;
    
    auto callback = [&files, &received](const std::vector<std::string>& fileList) {
        files = fileList;
        received = true;
    };

    if (!requestFileList(targetRank, callback, timeoutMs)) {
        return false;
    }

    return received;
}

bool AnariUsdClient::getFileListWithSizesSync(int32_t targetRank, std::vector<FileInfo>& files, int timeoutMs) {
    bool received = false;
    
    auto callback = [&files, &received](const std::vector<FileInfo>& fileList) {
        files = fileList;
        received = true;
    };

    if (!requestFileListWithSizes(targetRank, callback, timeoutMs)) {
        return false;
    }

    return received;
}

bool AnariUsdClient::sendRequest(const void* data, size_t size, uint32_t requestId) {
    try {
        std::lock_guard<std::recursive_mutex> lock(requestMutex);
        
        // DEBUG: Log request details
        const ZmqFileRequest* req = static_cast<const ZmqFileRequest*>(data);
        MIDDLEWARE_LOG_DEBUG("DEBUG sendRequest: magic=0x%08x, type=%u, size=%zu, request_id=%u",
                           req->magic, req->message_type, size, requestId);
        
        // Send empty delimiter frame with SNDMORE flag (first frame of 2-frame message)
        // DEALER sends: [empty delimiter] + [data] (2 frames)
        // ROUTER receives: [identity] + [empty delimiter] + [data] (3 frames)
        // ZeroMQ automatically adds identity frame for DEALER→ROUTER communication
        zmq::message_t emptyFrame(0);
        auto result1 = zmqSocket->send(emptyFrame, zmq::send_flags::sndmore);
        if (!result1) {
            MIDDLEWARE_LOG_ERROR("Failed to send empty delimiter frame (request_id: %u)", requestId);
            connectionStats.failedRequests.fetch_add(1);
            return false;
        }
        
        MIDDLEWARE_LOG_DEBUG("DEBUG sendRequest: Empty delimiter sent successfully");
        
        // Send binary struct (second/last frame, no SNDMORE flag)
        zmq::message_t msg(size);
        memcpy(msg.data(), data, size);
        auto result2 = zmqSocket->send(msg, zmq::send_flags::none);
        
        if (!result2 || result2.value() != size) {
            MIDDLEWARE_LOG_ERROR("Failed to send request data (request_id: %u)", requestId);
            connectionStats.failedRequests.fetch_add(1);
            return false;
        }

        MIDDLEWARE_LOG_DEBUG("DEBUG sendRequest: Request data sent successfully (%zu bytes)", size);
        connectionStats.totalRequestsSent.fetch_add(1);
        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error sending request: %s (errno: %d)", e.what(), e.num());
        connectionStats.failedRequests.fetch_add(1);
        return false;
    }
}

uint32_t AnariUsdClient::generateRequestId() {
    return nextRequestId.fetch_add(1);
}

AnariUsdClient::ConnectionStats::Snapshot AnariUsdClient::getConnectionStats() const {
    return connectionStats.getSnapshot();
}

void AnariUsdClient::resetConnectionStats() {
    connectionStats.reset();
}

void AnariUsdClient::setMaxMessageSize(size_t maxSizeBytes) {
    maxMessageSize.store(maxSizeBytes);
}

size_t AnariUsdClient::getMaxMessageSize() const {
    return maxMessageSize.load();
}

bool AnariUsdClient::testConnection() {
    if (connectionStatus.load() != ConnectionStatus::Connected) {
        return false;
    }

    try {
        // Send a simple file list request to test connection
        return requestFileList(0, nullptr, 1000);
    } catch (...) {
        return false;
    }
}

void AnariUsdClient::updateHealthStatus() {
    lastHealthCheck = std::chrono::steady_clock::now();
    // Additional health checks can be added here
}

void AnariUsdClient::setNotificationCallback(NotificationCallback callback) {
    std::lock_guard<std::mutex> lock(notificationCallbackMutex);
    notificationCallback = std::move(callback);
    MIDDLEWARE_LOG_INFO("Notification callback %s", notificationCallback ? "registered" : "cleared");
}

void AnariUsdClient::setSceneUpdateCallback(SceneUpdateCallback callback) {
    std::lock_guard<std::mutex> lock(sceneUpdateCallbackMutex);
    sceneUpdateCallback = std::move(callback);
    MIDDLEWARE_LOG_INFO("Scene update callback %s", sceneUpdateCallback ? "registered" : "cleared");
}

void AnariUsdClient::setProtocolDiagnosticsCallback(ProtocolDiagnosticsCallback callback) {
    std::lock_guard<std::mutex> lock(protocolDiagnosticsCallbackMutex);
    protocolDiagnosticsCallback = std::move(callback);
    MIDDLEWARE_LOG_INFO("Protocol diagnostics callback %s", protocolDiagnosticsCallback ? "registered" : "cleared");
}

void AnariUsdClient::emitProtocolDiagnostics(const std::string& event,
                                             const std::string& message,
                                             uint64_t value0,
                                             uint64_t value1) const
{
    MIDDLEWARE_LOG_INFO("[PROTOCOL-DIAG] %s: %s (%llu, %llu)",
                        event.c_str(),
                        message.c_str(),
                        static_cast<unsigned long long>(value0),
                        static_cast<unsigned long long>(value1));

    std::lock_guard<std::mutex> lock(protocolDiagnosticsCallbackMutex);
    if (!protocolDiagnosticsCallback) {
        return;
    }

    try {
        protocolDiagnosticsCallback(event, message, value0, value1);
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in protocol diagnostics callback: %s", e.what());
    } catch (...) {
        MIDDLEWARE_LOG_ERROR("Unknown exception in protocol diagnostics callback");
    }
}

void AnariUsdClient::handleSceneUpdate(const zmq::message_t& data)
{
    if (data.size() < sizeof(ZmqSceneUpdate)) {
        MIDDLEWARE_LOG_WARNING("Scene update message too small: %zu bytes", data.size());
        emitProtocolDiagnostics("scene_update_invalid", "message too small", data.size(), 0);
        return;
    }

    const ZmqSceneUpdate* update = reinterpret_cast<const ZmqSceneUpdate*>(data.data());

    if (!MessageUtils::isValidMagic(update->magic)) {
        MIDDLEWARE_LOG_WARNING("Invalid magic in scene update: 0x%08X", update->magic);
        emitProtocolDiagnostics("scene_update_invalid_magic", update->getPrimPath(), update->magic, 0);
        return;
    }

    const std::string messageType = MessageUtils::getMessageTypeName(update->message_type);
    const std::string primPath = update->getPrimPath();
    const std::string propertyName = update->getPropertyName();
    const std::string stringValue = update->getStringValue();

    MIDDLEWARE_LOG_INFO(
        "Received %s from rank %d: prim='%s' property='%s' change=%d type=%d commit=%llu revision=%llu int=%lld float=%f vec=(%f,%f,%f,%f) string='%s' payload=%u",
        messageType.c_str(),
        update->source_rank,
        primPath.c_str(),
        propertyName.c_str(),
        update->change_type,
        update->value_type,
        static_cast<unsigned long long>(update->commit_id),
        static_cast<unsigned long long>(update->revision),
        static_cast<long long>(update->int_value),
        static_cast<double>(update->float_value),
        static_cast<double>(update->vec4[0]),
        static_cast<double>(update->vec4[1]),
        static_cast<double>(update->vec4[2]),
        static_cast<double>(update->vec4[3]),
        stringValue.c_str(),
        update->payload_size);

    emitProtocolDiagnostics(
        "scene_update_received",
        primPath.empty() ? propertyName : primPath + " | " + propertyName,
        update->commit_id,
        update->revision);

    std::lock_guard<std::mutex> lock(sceneUpdateCallbackMutex);
    if (!sceneUpdateCallback) {
        MIDDLEWARE_LOG_WARNING("Scene update callback is NULL - scene update will not reach client");
        emitProtocolDiagnostics("scene_update_dropped", "callback not registered", update->commit_id, update->revision);
        return;
    }

    try {
        sceneUpdateCallback(
            update->message_type,
            update->source_rank,
            update->timestamp,
            update->commit_id,
            update->revision,
            primPath,
            propertyName,
            update->change_type,
            update->value_type,
            update->int_value,
            update->float_value,
            update->vec4,
            stringValue,
            update->payload_size);
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in scene update callback: %s", e.what());
        emitProtocolDiagnostics("scene_update_callback_error", e.what(), update->commit_id, update->revision);
    } catch (...) {
        MIDDLEWARE_LOG_ERROR("Unknown exception in scene update callback");
        emitProtocolDiagnostics("scene_update_callback_error", "unknown exception", update->commit_id, update->revision);
    }
}

void AnariUsdClient::handleNotification(const zmq::message_t& data) {
    if (data.size() < sizeof(ZmqFileNotification)) {
        MIDDLEWARE_LOG_WARNING("Notification message too small: %zu bytes", data.size());
        return;
    }

    const ZmqFileNotification* notif = reinterpret_cast<const ZmqFileNotification*>(data.data());

    if (!MessageUtils::isValidMagic(notif->magic)) {
        MIDDLEWARE_LOG_WARNING("Invalid magic in notification: 0x%08X", notif->magic);
        return;
    }

    std::string typeName = "NOTIFY_UNKNOWN";
    if (notif->message_type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_COMMIT_COMPLETE))
        typeName = "NOTIFY_COMMIT_COMPLETE";
    else if (notif->message_type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_FILE_UPDATE_V2))
        typeName = "NOTIFY_FILE_UPDATE_V2";
    else if (notif->message_type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_FILE_UPDATE))
        typeName = "NOTIFY_FILE_UPDATE";

    uint64_t hashLo          = notif->hash128[0];
    uint64_t hashHi          = notif->hash128[1];
    uint64_t hashPrevLo      = (notif->hashPrev128 && notif->hasOldData) ? notif->hashPrev128[0] : 0;
    uint64_t hashPrevHi      = (notif->hashPrev128 && notif->hasOldData) ? notif->hashPrev128[1] : 0;
    bool     hasOldDataFlag  = notif->hasOldData;

    MIDDLEWARE_LOG_INFO("Received %s from rank %d: '%s' (%llu bytes, hash %llx:%llx, old %llx:%llx, diff=%d)",
                        typeName.c_str(), notif->source_rank, notif->getFilename().c_str(),
                        static_cast<unsigned long long>(notif->file_size),
                        static_cast<unsigned long long>(notif->timestamp),
                        static_cast<unsigned long long>(hashLo),
                        static_cast<unsigned long long>(hashHi),
                        static_cast<unsigned long long>(hashPrevLo),
                        static_cast<unsigned long long>(hashPrevHi),
                        notif->hasOldData ? 1 : 0);

    std::lock_guard<std::mutex> lock(notificationCallbackMutex);
    if (notificationCallback) {
        try {
            notificationCallback(notif->message_type, notif->source_rank,
                                  notif->getFilename(), notif->file_size, notif->timestamp,
                                  hashLo, hashHi, hashPrevLo, hashPrevHi, hasOldDataFlag);
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Exception in notification callback: %s", e.what());
        } catch (...) {
            MIDDLEWARE_LOG_ERROR("Unknown exception in notification callback");
        }
    } else {
        MIDDLEWARE_LOG_WARNING("Notification callback is NULL - notifications will not reach client!");
    }
}

bool AnariUsdClient::requestWorkerListString(std::vector<std::tuple<int32_t, std::string, std::string>>& outWorkers, int timeoutMs) {
    if (connectionStatus.load() != ConnectionStatus::Connected || !zmqSocket) {
        MIDDLEWARE_LOG_ERROR("AnariUsdClient not connected");
        return false;
    }

    try {
        MIDDLEWARE_LOG_INFO("Requesting worker list using string protocol 'GET_WORKERS'");
        
        // Send GET_WORKERS string using HPC broker protocol
        // DEALER sends: [empty delimiter] + "GET_WORKERS" (2 frames)
        // ROUTER receives: [identity] + [empty delimiter] + "GET_WORKERS" (3 frames)
        // ✅ FIX: Lock only for send, release before blocking receive
        {
            std::lock_guard<std::recursive_mutex> lock(requestMutex);

            // Send empty delimiter frame with SNDMORE flag
            zmq::message_t emptyFrame(0);
            auto result = zmqSocket->send(emptyFrame, zmq::send_flags::sndmore);
            if (!result) {
                MIDDLEWARE_LOG_ERROR("Failed to send empty delimiter frame");
                connectionStats.failedRequests.fetch_add(1);
                return false;
            }

            // Send "GET_WORKERS" string (broker accepts any string)
            zmq::message_t requestMsg(strlen("GET_WORKERS"));
            memcpy(requestMsg.data(), "GET_WORKERS", strlen("GET_WORKERS"));
            result = zmqSocket->send(requestMsg, zmq::send_flags::none);

            if (!result || result.value() != strlen("GET_WORKERS")) {
                MIDDLEWARE_LOG_ERROR("Failed to send GET_WORKERS request");
                connectionStats.failedRequests.fetch_add(1);
                return false;
            }

            connectionStats.totalRequestsSent.fetch_add(1);
        } // ✅ Lock released here before blocking receive

        // Wait for the raw-string response via the dispatcher-backed index.
        // The GET_WORKERS reply carries no ANARI magic/request_id, so it is routed
        // to the id==0 FIFO by enqueueResponseFrame (the dispatcher is still the
        // sole owner of the socket recv).
        FrameMsg dummyDelim, responseData;
        if (!waitForMatchingFrames(0, timeoutMs, dummyDelim, responseData)) {
            MIDDLEWARE_LOG_ERROR("Timeout waiting for worker list response");
            return false;
        }

        // Parse response: "WORKER_LIST|rank1:hostname1:ip1;rank2:hostname2:ip2;..."
        std::string responseStr(reinterpret_cast<const char*>(responseData->data()), responseData->size());
        MIDDLEWARE_LOG_INFO("Received worker list response: %s", responseStr.c_str());

        // Check if response starts with "WORKER_LIST|"
        if (responseStr.find("WORKER_LIST|") != 0) {
            MIDDLEWARE_LOG_ERROR("Invalid worker list response format: %s", responseStr.c_str());
            return false;
        }

        // Extract worker data
        std::string workerData = responseStr.substr(12); // Skip "WORKER_LIST|"
        outWorkers.clear();
        
        size_t pos = 0;
        while (pos < workerData.length()) {
            size_t semicolonPos = workerData.find(';', pos);
            std::string workerStr;
            if (semicolonPos == std::string::npos) {
                workerStr = workerData.substr(pos);
                pos = workerData.length();
            } else {
                workerStr = workerData.substr(pos, semicolonPos - pos);
                pos = semicolonPos + 1;
            }
            
            if (workerStr.empty()) {
                continue;
            }
            
            // Parse "rank:hostname:ip"
            size_t colon1 = workerStr.find(':');
            size_t colon2 = workerStr.find(':', colon1 + 1);
            
            if (colon1 != std::string::npos && colon2 != std::string::npos) {
                try {
                    int32_t rank = std::stoi(workerStr.substr(0, colon1));
                    std::string hostname = workerStr.substr(colon1 + 1, colon2 - colon1 - 1);
                    std::string ip = workerStr.substr(colon2 + 1);
                    
                    outWorkers.push_back(std::make_tuple(rank, hostname, ip));
                } catch (const std::exception& e) {
                    MIDDLEWARE_LOG_WARNING("Failed to parse worker entry: %s", workerStr.c_str());
                }
            }
        }

        MIDDLEWARE_LOG_INFO("Parsed %zu workers from response", outWorkers.size());
        connectionStats.totalResponsesReceived.fetch_add(1);
        connectionStats.lastActivityTime = std::chrono::steady_clock::now();

        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error in requestWorkerListString: %s (errno: %d)", e.what(), e.num());
        return false;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in requestWorkerListString: %s", e.what());
        return false;
    }
}

bool AnariUsdClient::requestWorkerCount(WorkerCountCallback callback, int timeoutMs) {
    if (connectionStatus.load() != ConnectionStatus::Connected || !zmqSocket) {
        MIDDLEWARE_LOG_ERROR("AnariUsdClient not connected");
        return false;
    }

    try {
        // Create binary property request using ZmqFileRequest struct
        ZmqFileRequest request;
        request.magic = ANARI_USD_MAGIC;           // 0x55534446 ("USDF")
        request.message_type = static_cast<uint32_t>(ZmqMessageType::REQ_GET_PROPERTY);  // 400
        request.request_id = generateRequestId();
        request.target_rank = -1;                  // Request from all ranks
        request.setFilename("totalWorkerCount");   // Use totalWorkerCount (includes rank 0) not workerCount (excludes rank 0)
        request.chunk_size = 0;                    // Not applicable for property requests

        MIDDLEWARE_LOG_DEBUG("Requesting worker count via binary property 'totalWorkerCount' (request_id: %u)",
                             request.request_id);

        // ✅ FIX: Removed outer requestMutex lock - sendRequest() handles its own locking

        // Send binary struct (276 bytes) using existing sendRequest method
        if (!sendRequest(&request, sizeof(request), request.request_id)) {
            MIDDLEWARE_LOG_ERROR("Failed to send binary property request");
            return false;
        }

        connectionStats.totalRequestsSent.fetch_add(1);

        // Receive binary property response (filtered by request_id)
        FrameMsg dummyDelim, dataFrame;
        uint32_t workerCountRequestId = request.request_id;
        if (!waitForMatchingFrames(workerCountRequestId, timeoutMs, dummyDelim, dataFrame)) {
            MIDDLEWARE_LOG_ERROR("Timeout waiting for property response");
            return false;
        }

        // Validate response size
        size_t responseSize = dataFrame->size();
        if (responseSize < sizeof(ZmqPropertyResponse)) {
            MIDDLEWARE_LOG_ERROR("Property response too small: %zu bytes (expected at least %zu)",
                                 responseSize, sizeof(ZmqPropertyResponse));
            return false;
        }

        // Parse binary property response
        const ZmqPropertyResponse* response = reinterpret_cast<const ZmqPropertyResponse*>(dataFrame->data());
        MIDDLEWARE_LOG_DEBUG("DEBUG: Received binary property response: magic=0x%08x type=%u id=%u prop=%d int=%lld",
                             response->magic, response->message_type, response->request_id,
                             response->property_type, response->int_value);
        
        // Validate response
        if (!MessageUtils::isValidMagic(response->magic)) {
            MIDDLEWARE_LOG_ERROR("Invalid magic number in property response: 0x%08x", response->magic);
            return false;
        }

        if (response->message_type != static_cast<uint32_t>(ZmqMessageType::RESP_PROPERTY)) {
            MIDDLEWARE_LOG_ERROR("Unexpected message type in property response: %u (expected RESP_PROPERTY=401)",
                                response->message_type);
            return false;
        }

        if (response->request_id != request.request_id) {
            MIDDLEWARE_LOG_WARNING("Property response request_id mismatch: expected %u, got %u",
                                  request.request_id, response->request_id);
        }

        // Check property type
        if (response->property_type == -1) {
            MIDDLEWARE_LOG_ERROR("Property request failed with error (property_type = -1)");
            return false;
        }

        if (response->property_type != 0) {
            MIDDLEWARE_LOG_ERROR("Unexpected property type: %d (expected 0=int32)", response->property_type);
            return false;
        }

        // Get worker count from int_value field
        uint32_t totalWorkers = static_cast<uint32_t>(response->int_value);
        MIDDLEWARE_LOG_DEBUG("Parsed worker count: totalWorkers=%u", totalWorkers);

        // Trigger callback
        if (callback) {
            callback(totalWorkers);
        }

        connectionStats.totalResponsesReceived.fetch_add(1);
        connectionStats.lastActivityTime = std::chrono::steady_clock::now();

        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error in requestWorkerCount: %s (errno: %d)", e.what(), e.num());
        return false;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in requestWorkerCount: %s", e.what());
        return false;
    }
}

bool AnariUsdClient::requestWorkerStatus(int32_t targetRank, WorkerStatusCallback callback, int timeoutMs) {
    if (connectionStatus.load() != ConnectionStatus::Connected || !zmqSocket) {
        MIDDLEWARE_LOG_ERROR("AnariUsdClient not connected");
        return false;
    }

    try {
        MIDDLEWARE_LOG_WARNING("requestWorkerStatus: ANARI-USD broker doesn't support individual worker status queries");
        MIDDLEWARE_LOG_WARNING("Using REQ_GET_PROPERTY (400) with 'workerList' property instead of unsupported REQ_WORKER_STATUS (20)");
        
        // ANARI-USD broker doesn't support individual worker status queries
        // Only aggregate properties are supported: "workerCount", "mpiSize", "mpiRank", "workerList"
        // We'll use "workerList" to get all workers and find the specific rank
        ZmqFileRequest request;
        request.magic = ANARI_USD_MAGIC;
        request.message_type = static_cast<uint32_t>(ZmqMessageType::REQ_GET_PROPERTY);  // 400 instead of 20
        request.request_id = generateRequestId();
        request.target_rank = -1;  // Request from broker, not specific rank
        
        // Request worker list (supported property)
        request.setFilename("workerList");
        request.chunk_size = 0;

        MIDDLEWARE_LOG_INFO("Requesting worker status for rank %d using 'workerList' property (request_id: %u)",
                            targetRank, request.request_id);

        // ✅ FIX: Removed outer requestMutex lock - sendRequest() handles its own locking

        // Send request
        if (!sendRequest(&request, sizeof(request), request.request_id)) {
            MIDDLEWARE_LOG_ERROR("Failed to send worker status property request");
            return false;
        }

        connectionStats.totalRequestsSent.fetch_add(1);

        // Receive property response (filtered by request_id)
        FrameMsg dummyDelim, dataFrame;
        uint32_t workerStatusRequestId = request.request_id;
        if (!waitForMatchingFrames(workerStatusRequestId, timeoutMs, dummyDelim, dataFrame)) {
            MIDDLEWARE_LOG_ERROR("Timeout waiting for worker status property response");
            return false;
        }

        // Validate response size
        size_t responseSize = dataFrame->size();
        if (responseSize < sizeof(ZmqPropertyResponse)) {
            MIDDLEWARE_LOG_ERROR("Property response too small: %zu bytes (expected at least %zu)",
                                 responseSize, sizeof(ZmqPropertyResponse));
            return false;
        }

        // Parse property response
        const ZmqPropertyResponse* response = reinterpret_cast<const ZmqPropertyResponse*>(dataFrame->data());
        
        // Validate response
        if (!MessageUtils::isValidMagic(response->magic)) {
            MIDDLEWARE_LOG_ERROR("Invalid magic number in property response: 0x%08x", response->magic);
            return false;
        }

        if (response->message_type != static_cast<uint32_t>(ZmqMessageType::RESP_PROPERTY)) {
            MIDDLEWARE_LOG_ERROR("Unexpected message type in property response: %u (expected RESP_PROPERTY=401)",
                                response->message_type);
            return false;
        }

        if (response->request_id != request.request_id) {
            MIDDLEWARE_LOG_WARNING("Property response request_id mismatch: expected %u, got %u",
                                  request.request_id, response->request_id);
        }

        // Check if property request failed
        if (response->property_type == -1) {
            MIDDLEWARE_LOG_ERROR("Worker status property request failed (property_type = -1)");
            MIDDLEWARE_LOG_ERROR("ANARI-USD broker may not support individual worker status queries");
            return false;
        }

        MIDDLEWARE_LOG_INFO("Received property response for worker list request (property_type=%d)",
                           response->property_type);

        // Check if we got a string response (property_type = 1 for "workerList")
        if (response->property_type != 1) {
            MIDDLEWARE_LOG_ERROR("Expected string property type (1) for workerList, got %d", response->property_type);
            return false;
        }

        // Parse worker list string: "rank:hostname:ib_address;rank:hostname:ib_address;..."
        std::string workerList = response->getStringValue();
        MIDDLEWARE_LOG_INFO("Worker list: %s", workerList.c_str());

        // Parse to find specific rank
        std::string foundHostname = "unknown";
        std::string foundIp = "unknown";
        
        size_t start = 0;
        while (start < workerList.size()) {
            size_t end = workerList.find(';', start);
            if (end == std::string::npos) end = workerList.size();
            
            std::string workerEntry = workerList.substr(start, end - start);
            if (!workerEntry.empty()) {
                // Parse "rank:hostname:ib_address"
                size_t colon1 = workerEntry.find(':');
                size_t colon2 = workerEntry.find(':', colon1 + 1);
                
                if (colon1 != std::string::npos && colon2 != std::string::npos) {
                    std::string rankStr = workerEntry.substr(0, colon1);
                    std::string hostname = workerEntry.substr(colon1 + 1, colon2 - colon1 - 1);
                    std::string ip = workerEntry.substr(colon2 + 1);
                    
                    try {
                        int rank = std::stoi(rankStr);
                        if (rank == targetRank) {
                            foundHostname = hostname;
                            foundIp = ip;
                            break;
                        }
                    } catch (const std::exception& e) {
                        MIDDLEWARE_LOG_WARNING("Failed to parse rank from worker entry: %s", workerEntry.c_str());
                    }
                }
            }
            
            start = end + 1;
        }

        // Trigger callback with available information
        // Note: ANARI-USD broker doesn't provide worker status, GPU info, or heartbeat
        // We return placeholder values for those fields
        if (callback) {
            callback(targetRank, 1,  // status=1 (idle) - placeholder
                     foundHostname,   // actual hostname if found
                     "unknown",       // gpu_info - not provided by broker  
                     0);              // last_heartbeat - not provided by broker
        }

        connectionStats.totalResponsesReceived.fetch_add(1);
        connectionStats.lastActivityTime = std::chrono::steady_clock::now();

        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error in requestWorkerStatus: %s (errno: %d)", e.what(), e.num());
        return false;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in requestWorkerStatus: %s", e.what());
        return false;
    }
}

bool AnariUsdClient::getWorkerCountSync(uint32_t& workerCount, int timeoutMs) {
    std::promise<uint32_t> promise;
    std::future<uint32_t> future = promise.get_future();
    
    bool success = requestWorkerCount([&promise](uint32_t count) {
        promise.set_value(count);
    }, timeoutMs);
    
    if (!success) {
        return false;
    }
    
    auto status = future.wait_for(std::chrono::milliseconds(timeoutMs));
    if (status == std::future_status::ready) {
        workerCount = future.get();
        return true;
    }
    
    return false;
}

bool AnariUsdClient::getWorkerStatusSync(int32_t targetRank,
                                         std::vector<std::tuple<int32_t, uint32_t, std::string, std::string, uint64_t>>& workers,
                                         int timeoutMs) {
    // For now, just get single worker status
    std::promise<bool> promise;
    std::future<bool> future = promise.get_future();
    
    bool success = requestWorkerStatus(targetRank, 
        [&promise, &workers](int32_t rank, uint32_t status, const std::string& hostname, 
                             const std::string& gpuInfo, uint64_t lastHeartbeat) {
            workers.push_back(std::make_tuple(rank, status, hostname, gpuInfo, lastHeartbeat));
            promise.set_value(true);
        }, timeoutMs);
    
    if (!success) {
        return false;
    }
    
    auto status = future.wait_for(std::chrono::milliseconds(timeoutMs));
    return status == std::future_status::ready;
}

bool AnariUsdClient::getTotalWorkerCountSync(uint32_t& totalCount, int timeoutMs) {
    MIDDLEWARE_LOG_DEBUG("=== getTotalWorkerCountSync ENTERED (timeout=%d ms) ===", timeoutMs);
    
    // Use binary protocol instead of legacy string protocol
    std::promise<uint32_t> promise;
    std::future<uint32_t> future = promise.get_future();
    
    bool success = requestWorkerCount([&promise](uint32_t count) {
        MIDDLEWARE_LOG_DEBUG("Worker count callback received: count=%u", count);
        promise.set_value(count);
    }, timeoutMs);
    
    if (!success) {
        MIDDLEWARE_LOG_ERROR("Failed to retrieve worker count using binary protocol");
        return false;
    }
    
    // Wait for the result
    auto status = future.wait_for(std::chrono::milliseconds(timeoutMs));
    if (status != std::future_status::ready) {
        MIDDLEWARE_LOG_ERROR("Timeout waiting for worker count response");
        return false;
    }
    
    totalCount = future.get();
    MIDDLEWARE_LOG_DEBUG("=== getTotalWorkerCountSync COMPLETE: totalCount=%u ===", totalCount);
    return true;
}

bool AnariUsdClient::requestFilesParallel(
    const std::vector<std::string>& filenames,
    const std::vector<int32_t>& target_ranks,
    std::function<void(const std::string&, const std::vector<uint8_t>&)> spawn_callback,
    std::function<void()> completion_callback,
    std::function<void(const std::string&, const std::string&)> error_callback,
    int timeout_ms) {
    
    // Extreme crash protection - check for stack corruption
    try {
        MIDDLEWARE_LOG_DEBUG("=== ENTERING requestFilesParallel ===");
    } catch (...) {
        // If logging fails, we have serious memory corruption
        return false;
    }
    MIDDLEWARE_LOG_INFO("Filenames: %zu, TargetRanks: %zu", filenames.size(), target_ranks.size());
    
    #ifdef _WIN32
    char debug_msg[512];
    snprintf(debug_msg, sizeof(debug_msg), "[ANARI] requestFilesParallel: %zu files, first: %s\n", 
             filenames.size(), filenames.empty() ? "(none)" : filenames[0].c_str());
    OutputDebugStringA(debug_msg);
    #endif
    
    if (!isConnected()) {
        MIDDLEWARE_LOG_ERROR("Cannot start parallel downloads: client not connected");
        if (error_callback) {
            for (const auto& filename : filenames) {
                error_callback(filename, "Client not connected");
            }
        }
        return false;
    }
    
    MIDDLEWARE_LOG_DEBUG("Client is connected");
    
    if (filenames.size() != target_ranks.size()) {
        MIDDLEWARE_LOG_ERROR("Filename count (%zu) doesn't match target_ranks count (%zu)",
                           filenames.size(), target_ranks.size());
        return false;
    }
    
    MIDDLEWARE_LOG_DEBUG("Input validation passed");

    MIDDLEWARE_LOG_DEBUG("Starting parallel download of %zu files using same logic as async node", filenames.size());
    
    // Create shared state for tracking all downloads
    struct ParallelDownloadState {
        std::atomic<size_t> completed_files{0};
        std::atomic<size_t> successful_files{0};
        std::mutex completion_mutex;
    };
    
    auto state = std::make_shared<ParallelDownloadState>();
    size_t total_files = filenames.size();
    
    // Ensure the bounded parallel-download worker pool is running.
    {
        std::lock_guard<std::mutex> lock(downloadTaskMutex);
        if (!downloadPoolActive.load(std::memory_order_acquire)) {
            downloadPoolActive.store(true, std::memory_order_release);
            const unsigned int hw = std::thread::hardware_concurrency();
            const size_t n = std::min<size_t>(MAX_PARALLEL_DOWNLOADS,
                                              hw > 0 ? hw : static_cast<unsigned int>(MAX_PARALLEL_DOWNLOADS));
            downloadWorkers.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                downloadWorkers.emplace_back(&AnariUsdClient::downloadWorkerLoop, this);
            }
            MIDDLEWARE_LOG_INFO("Parallel download pool started with %zu workers", n);
        }
    }

    // Build a per-file download task and hand it to the worker pool for overlap.
    for (size_t i = 0; i < filenames.size(); ++i) {
        std::string filename = filenames[i];
        int32_t target_rank = target_ranks[i];
        
        // Create per-file state for accumulation
        struct FileDownloadState {
            std::vector<uint8_t> data;
            uint64_t total_size = 0;
            bool complete = false;
            std::string filename;
        };
        
        auto file_state = std::make_shared<FileDownloadState>();
        file_state->filename = filename;
        
        // Define callbacks (same pattern as getFileSync)
        // Use explicit std::function to ensure proper type conversion
        std::function<void(const std::string&, const uint8_t*, size_t, uint64_t, uint64_t)> chunk_callback = 
            [file_state](const std::string& fname, const uint8_t* chunk, size_t chunkSize,
                        uint64_t offset, uint64_t total_size) {
                file_state->total_size = total_size;
                if (file_state->data.size() < offset + chunkSize) {
                    file_state->data.resize(offset + chunkSize);
                }
                std::memcpy(file_state->data.data() + offset, chunk, chunkSize);
            };
        
        std::function<void(const std::string&, uint64_t)> complete_callback = 
            [file_state, state, spawn_callback, total_files, completion_callback](
                const std::string& fname, uint64_t total_size) {
                
                file_state->complete = true;
                file_state->data.resize(total_size);
                
                MIDDLEWARE_LOG_INFO("Parallel download complete: %s (%zu bytes)", 
                                   fname.c_str(), file_state->data.size());
                
                // Call spawn callback with complete file data
                if (spawn_callback && !file_state->data.empty()) {
                    spawn_callback(fname, file_state->data);
                }
                
                // Update completion state
                size_t completed = state->completed_files.fetch_add(1) + 1;
                size_t successful = state->successful_files.fetch_add(1) + 1;
                
                MIDDLEWARE_LOG_DEBUG("Parallel progress: %zu/%zu files", completed, total_files);
                
                // Check if all files are done
                if (completed >= total_files) {
                    MIDDLEWARE_LOG_INFO("All parallel downloads completed: %zu/%zu successful", 
                                       successful, total_files);
                    
                    if (completion_callback && successful == total_files) {
                        completion_callback();
                    }
                }
            };
        
        std::function<void(const std::string&)> error_callback_wrapper = 
            [filename, error_callback, state, total_files](const std::string& error_msg) {
                
                MIDDLEWARE_LOG_ERROR("Parallel download error for %s: %s", filename.c_str(), error_msg.c_str());
                
                if (error_callback) {
                    error_callback(filename, error_msg);
                }
                
                // Update completion state (with error)
                size_t completed = state->completed_files.fetch_add(1) + 1;
                
                // Check if all files are done (including errors)
                if (completed >= total_files) {
                    MIDDLEWARE_LOG_INFO("All parallel downloads completed (with errors)");
                    // Don't call completion_callback since there were errors
                }
            };
        
        // Hand off this file to the worker pool so downloads overlap (up to
        // MAX_PARALLEL_DOWNLOADS concurrent transfers).  Previously requestFile
        // ran synchronously here, serializing all N files on a single thread
        // despite the "parallel" name.
        auto task = FileDownloadTask{};
        task.run = [this, filename, target_rank, timeout_ms,
                    chunk_callback, complete_callback, error_callback_wrapper,
                    error_callback, state]() {
            if (!this->requestFile(filename, target_rank,
                                   chunk_callback, complete_callback,
                                   error_callback_wrapper, timeout_ms)) {
                // requestFile only returns false without routing an error through
                // error_callback_wrapper in this "not started" (send) case.
                MIDDLEWARE_LOG_ERROR("Failed to start parallel download for: %s", filename.c_str());
                if (error_callback) {
                    error_callback(filename, "Failed to start download");
                }
                // Count as completed (with error)
                state->completed_files.fetch_add(1);
            }
        };

        {
            std::lock_guard<std::mutex> lock(downloadTaskMutex);
            downloadTaskQueue.push_back(std::move(task));
        }
    }

    downloadTaskCv.notify_all();
    
    // Return true immediately - downloads run asynchronously
    // Callbacks will handle completion and spawning
    return true;
}

// ============================================================================
// Request-ID matching receive helpers
// ============================================================================

uint32_t AnariUsdClient::extractRequestId(const uint8_t* data, size_t dataSize) const {
    if (!data || dataSize < 12) return 0; // need magic(4) + type(4) + request_id(4)
    uint32_t magic = *reinterpret_cast<const uint32_t*>(data);
    if (magic != ANARI_USD_MAGIC) return 0;
    uint32_t msgType = *reinterpret_cast<const uint32_t*>(data + 4);
    if (msgType < 200) return 0;
    return *reinterpret_cast<const uint32_t*>(data + 8);
}

// ---------------------------------------------------------------------------
// Dispatcher thread: owns ALL ZMQ recv. Pulls frame pairs from the socket
// and enqueues them.  Request threads never call recv().
// ---------------------------------------------------------------------------

void AnariUsdClient::dispatcherThread() {
    MIDDLEWARE_LOG_INFO("Dispatcher thread started");

    zmq::pollitem_t items[] = {{ zmqSocket->handle(), 0, ZMQ_POLLIN, 0 }};

    while (dispatcherActive.load()) {
        try {
            // Poll with 100ms timeout so we wake up promptly on activity but
            // also check dispatcherActive periodically.
            int pollResult = zmq::poll(items, 1, std::chrono::milliseconds(100));
            if (pollResult <= 0) continue;

            // Poll said data available — receive the complete pair under recvMutex
            zmq::message_t delimMsg, dataMsg;

            {
                std::lock_guard<std::mutex> recvLock(recvMutex);
                auto dRes = zmqSocket->recv(delimMsg, zmq::recv_flags::none);
                if (!dRes) continue;

                auto dataRes = zmqSocket->recv(dataMsg, zmq::recv_flags::none);
                if (!dataRes) continue;
            }

            // Zero-copy: move the received ZMQ buffers straight into the
            // response index (previously two full memcpys per message here).
            enqueueResponseFrame(std::move(delimMsg), std::move(dataMsg));

        } catch (const zmq::error_t& e) {
            if (e.num() != EINVAL) { // EINVAL = context terminated on purpose
                MIDDLEWARE_LOG_WARNING("Dispatcher ZMQ error: %s (errno: %d)",
                                       e.what(), e.num());
            }
        } catch (...) {
            MIDDLEWARE_LOG_WARNING("Dispatcher unknown error");
        }
    }

    MIDDLEWARE_LOG_INFO("Dispatcher thread stopped");
}

// ---------------------------------------------------------------------------
// enqueueResponseFrame — called ONLY by dispatcher thread
// ---------------------------------------------------------------------------

void AnariUsdClient::enqueueResponseFrame(zmq::message_t delimiter,
                                            zmq::message_t data) {
    // Check if this is a notification message (NOTIFY_FILE_UPDATE=300, NOTIFY_COMMIT_COMPLETE=301,
    // NOTIFY_FILE_UPDATE_V2=302, NOTIFY_SCENE_UPDATE=303, NOTIFY_PROPERTY_UPDATE=304).
    // Notifications have no request_id and would never be matched by waiting threads.
    if (data.size() >= 8) {
        uint32_t magic = *reinterpret_cast<const uint32_t*>(data.data());
        uint32_t msgType = *reinterpret_cast<const uint32_t*>(data.data() + 4);

        if (magic == ANARI_USD_MAGIC && MessageUtils::isSceneUpdateType(msgType)) {
            handleSceneUpdate(data);
            return;
        }

        if (magic == ANARI_USD_MAGIC && MessageUtils::isNotificationType(msgType)) {
            handleNotification(data);
            return; // Don't enqueue notification into response index
        }

        if (magic == ANARI_USD_MAGIC && !MessageUtils::isValidMessageType(msgType)) {
            const uint32_t requestId = extractRequestId(static_cast<const uint8_t*>(data.data()), data.size());
            MIDDLEWARE_LOG_WARNING(
                "Unknown ANARI-USD message type %u (request_id=%u, size=%zu) - %s",
                msgType,
                requestId,
                data.size(),
                requestId == 0 ? "dropping push message" : "enqueueing as unmatched response");

            emitProtocolDiagnostics("unknown_message_type", MessageUtils::getMessageTypeName(msgType), msgType, data.size());

            if (requestId == 0) {
                return;
            }
        }
    }

    // Extract the request_id BEFORE moving `data` (move leaves it empty).
    const uint32_t requestId = extractRequestId(static_cast<const uint8_t*>(data.data()), data.size());

    auto entry = std::make_shared<FramePair>(
        std::make_shared<zmq::message_t>(std::move(delimiter)),
        std::make_shared<zmq::message_t>(std::move(data)));

    {
        std::lock_guard<std::mutex> lock(responseQueueMutex);
        if (requestId == 0) {
            // Raw-string response (e.g. GET_WORKERS) carrying no ANARI magic/id.
            noIdResponseQueue.push_back(std::move(entry));
        } else {
            responseByRequest[requestId].push_back(std::move(entry));
        }
        totalQueuedFrames.fetch_add(1, std::memory_order_relaxed);

        // High-watermark safety valve: warn (never drop) if the index grows
        // unbounded because a consumer stalled.  A stuck consumer only leaks its
        // own request_id entry, which is drained when it completes/times out.
        if (totalQueuedFrames.load(std::memory_order_relaxed) > 4000) {
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t last = lastQueueWarnMs.exchange(nowMs);
            if (nowMs - last > 1000) {
                MIDDLEWARE_LOG_WARNING("Response index high watermark (%zu frames) — a consumer may be stalled",
                                       totalQueuedFrames.load(std::memory_order_relaxed));
            }
        }
    }
    responseQueueCv.notify_all(); // Wake waiting threads matching their id
}

// ---------------------------------------------------------------------------
// tryDequeueMatching — non-blocking dequeue attempt
// ---------------------------------------------------------------------------

bool AnariUsdClient::tryDequeueMatching(uint32_t requestId,
                                        FrameMsg& outDelimiter,
                                        FrameMsg& outData) {
    std::lock_guard<std::mutex> lock(responseQueueMutex);
    return tryDequeueMatchingLocked(requestId, outDelimiter, outData);
}

bool AnariUsdClient::tryDequeueMatchingLocked(uint32_t requestId,
                                              FrameMsg& outDelimiter,
                                              FrameMsg& outData) {
    FramePairPtr entry;
    if (requestId == 0) {
        if (!noIdResponseQueue.empty()) {
            entry = std::move(noIdResponseQueue.front());
            noIdResponseQueue.pop_front();
        }
    } else {
        auto it = responseByRequest.find(requestId);
        if (it != responseByRequest.end() && !it->second.empty()) {
            entry = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty()) {
                responseByRequest.erase(it); // bound memory once a request fully drains
            }
        }
    }
    if (!entry) {
        return false;
    }
    totalQueuedFrames.fetch_sub(1, std::memory_order_relaxed);
    outDelimiter = std::move(entry->first);
    outData = std::move(entry->second);
    return true;
}

// ---------------------------------------------------------------------------
// waitForMatchingFrames — blocks on condition_variable until matching frame
// arrives or timeout expires.  Game threads NEVER call recv().
// ---------------------------------------------------------------------------

bool AnariUsdClient::waitForMatchingFrames(uint32_t requestId, int timeoutMs,
                                            FrameMsg& outDelimiter,
                                            FrameMsg& outData) {
    // Fast path: frame already in the index
    if (tryDequeueMatching(requestId, outDelimiter, outData)) return true;

    // Slow path: wait on the condition_variable until a frame for THIS id arrives.
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeoutMs);

    std::unique_lock<std::mutex> lock(responseQueueMutex);
    while (true) {
        if (shutdownRequested.load() ||
            connectionStatus.load() != ConnectionStatus::Connected) {
            return false;
        }

        // O(1) predicate: is there a frame buffered for this specific request_id?
        auto hasFrame = [this, requestId]() -> bool {
            if (requestId == 0) {
                return !noIdResponseQueue.empty();
            }
            auto it = responseByRequest.find(requestId);
            return it != responseByRequest.end() && !it->second.empty();
        };
        if (hasFrame()) break;

        // Wait on condition_variable with deadline
        if (responseQueueCv.wait_until(lock, deadline) == std::cv_status::timeout) {
            MIDDLEWARE_LOG_WARNING("Timeout %d ms waiting for request_id %u",
                                   timeoutMs, requestId);
            return false;
        }
    }

    // A frame is present — dequeue our own id's frame.
    // Use the locked variant: we already hold responseQueueMutex, and
    // re-locking the same non-recursive mutex self-deadlocks (glibc parks
    // the thread in the futex forever; UE Stop then hangs in cleanup()).
    return tryDequeueMatchingLocked(requestId, outDelimiter, outData);
}

// ---------------------------------------------------------------------------
// downloadWorkerLoop — pool worker.  Runs per-file download tasks (requestFile)
// so requestFilesParallel can overlap up to MAX_PARALLEL_DOWNLOADS transfers.
// Exits cleanly when cleanup() stops the pool and the task queue drains.
// ---------------------------------------------------------------------------
void AnariUsdClient::downloadWorkerLoop() {
    for (;;) {
        FileDownloadTask task;
        {
            std::unique_lock<std::mutex> lock(downloadTaskMutex);
            downloadTaskCv.wait(lock, [this]() {
                return !downloadPoolActive.load(std::memory_order_acquire) ||
                       !downloadTaskQueue.empty();
            });
            if (!downloadPoolActive.load(std::memory_order_acquire)) {
                break; // pool stopped: finish without starting queued tasks
            }
            if (downloadTaskQueue.empty()) {
                break; // spurious wake
            }
            task = std::move(downloadTaskQueue.front());
            downloadTaskQueue.pop_front();
        }
        if (task.run) {
            try {
                task.run();
            } catch (const std::exception& e) {
                MIDDLEWARE_LOG_ERROR("Parallel download worker exception: %s", e.what());
            } catch (...) {
                MIDDLEWARE_LOG_ERROR("Parallel download worker unknown exception");
            }
        }
    }
}

// ============================================================================
// Cleanup
// ============================================================================

void AnariUsdClient::cleanup() {
    // 1) Signal ALL background loops to stop and wake ALL waiters BEFORE any
    //    join.  Download workers run requestFile, which blocks in
    //    waitForMatchingFrames on responseQueueCv; if we joined them first, a
    //    worker mid-request would sleep out its full timeout (up to 10 minutes
    //    for large files) before noticing shutdownRequested, stalling Stop.
    downloadPoolActive.store(false, std::memory_order_release);
    dispatcherActive.store(false);
    downloadTaskCv.notify_all();
    responseQueueCv.notify_all();

    // 2) Stop the parallel-download worker pool.  Its workers run
    //    requestFile (send path + response index), so join them before the
    //    dispatcher / socket / context are torn down.
    for (auto& w : downloadWorkers) {
        if (w.joinable()) {
            w.join();
        }
    }
    downloadWorkers.clear();

    // 3) Stop dispatcher thread — it owns all ZMQ receive
    if (dispatchThread.joinable()) {
        dispatchThread.join();
    }

    if (zmqSocket) {
        try {
            zmqSocket->close();
        } catch (...) {
            // Ignore errors during cleanup
        }
        zmqSocket.reset();
    }

    if (zmqContext) {
        try {
            zmqContext->close();
        } catch (...) {
            // Ignore errors during cleanup
        }
        zmqContext.reset();
    }

    pendingRequests.clear();

    // 4) Empty the response index and the download task queue.
    {
        std::lock_guard<std::mutex> lock(responseQueueMutex);
        responseByRequest.clear();
        noIdResponseQueue.clear();
    }
    totalQueuedFrames.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(downloadTaskMutex);
        downloadTaskQueue.clear();
    }
}

} // namespace anari_usd_middleware
