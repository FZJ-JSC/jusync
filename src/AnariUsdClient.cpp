#include "AnariUsdClient.h"
#include "MiddlewareLogging.h"
#include "ParallelDownloadManager.h"

#include <regex>
#include <algorithm>
#include <thread>
#include <future>
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

        // Wait for response
        if (!waitForResponse(request.request_id, timeoutMs)) {
            MIDDLEWARE_LOG_ERROR("Timeout waiting for file list response");
            return false;
        }

        // Receive file list response as combined ZmqFileChunk + JSON data (single message)
        // HPC broker sends: ZmqFileChunk header (292 bytes) + JSON data combined in single message
        size_t headerSize = sizeof(ZmqFileChunk);
        
        // Receive the entire combined message (header + JSON) in one ZeroMQ message
        // We need to receive without knowing the exact size first
        
        // Receive empty delimiter frame (zero-length)
        zmq::message_t emptyDelimiter;
        auto delimResult = zmqSocket->recv(emptyDelimiter, zmq::recv_flags::none);
        if (!delimResult) {
            MIDDLEWARE_LOG_ERROR("Failed to receive empty delimiter");
            return false;
        }
        if (delimResult.value() != 0) {
            MIDDLEWARE_LOG_WARNING("Empty delimiter frame has non-zero size: %zu", delimResult.value());
        }

        // Receive the combined header+data ZeroMQ message
        zmq::message_t msg;
        auto result = zmqSocket->recv(msg, zmq::recv_flags::none);
        
        if (!result) {
            MIDDLEWARE_LOG_ERROR("Failed to receive file list response");
            return false;
        }
        
        size_t totalSize = result.value();
        MIDDLEWARE_LOG_INFO("Received file list response: %zu bytes total", totalSize);
        
        // Check minimum size (header)
        if (totalSize < headerSize) {
            MIDDLEWARE_LOG_ERROR("File list response too small: %zu bytes (expected at least %zu)", 
                                totalSize, headerSize);
            return false;
        }
        
        // Parse header from first part of message
        const uint8_t* msgData = static_cast<const uint8_t*>(msg.data());
        const ZmqFileChunk* chunk = reinterpret_cast<const ZmqFileChunk*>(msgData);
        
        // Validate response
        if (!MessageUtils::isValidMagic(chunk->magic)) {
            MIDDLEWARE_LOG_ERROR("Invalid magic number in file list response");
            return false;
        }

        if (chunk->message_type != static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK)) {
            MIDDLEWARE_LOG_ERROR("Unexpected message type in file list response: %u (expected RESP_FILE_CHUNK=201)", 
                                chunk->message_type);
            return false;
        }

        // Check if this is a file list response (filename should be "filelist.json")
        std::string filename = chunk->getFilename();
        if (filename != "filelist.json") {
            MIDDLEWARE_LOG_WARNING("File list response has unexpected filename: %s", filename.c_str());
        }

        MIDDLEWARE_LOG_INFO("Received file list chunk header: %llu bytes from rank %d, chunk size: %u", 
                            chunk->file_size, chunk->source_rank, chunk->chunk_size);
        
        // Extract JSON data from remaining part of message
        std::vector<uint8_t> jsonData;
        if (chunk->chunk_size > 0) {
            // Check if we have enough data
            size_t expectedTotalSize = headerSize + chunk->chunk_size;
            if (totalSize < expectedTotalSize) {
                MIDDLEWARE_LOG_ERROR("Incomplete file list response: got %zu bytes, expected %zu", 
                                    totalSize, expectedTotalSize);
                return false;
            }
            
            // Copy JSON data from message
            jsonData.assign(msgData + headerSize, msgData + headerSize + chunk->chunk_size);
            MIDDLEWARE_LOG_INFO("Extracted JSON data: %zu bytes", jsonData.size());
        } else {
            MIDDLEWARE_LOG_WARNING("File list response has zero chunk size");
        }

        // Parse JSON
        std::vector<std::string> files;
        try {
            std::string jsonStr(reinterpret_cast<const char*>(jsonData.data()), jsonData.size());
            auto json = json::parse(jsonStr);
            
            // Check JSON structure: {"rank": X, "files": [{"name": "...", "size": N, "mime": "..."}, ...]}
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

        // Receive empty delimiter for complete message
        zmq::message_t emptyDelimiter2;
        auto delimResult2 = zmqSocket->recv(emptyDelimiter2, zmq::recv_flags::none);
        if (!delimResult2) {
            MIDDLEWARE_LOG_ERROR("Failed to receive empty delimiter for complete message");
            return false;
        }
        if (delimResult2.value() != 0) {
            MIDDLEWARE_LOG_WARNING("Empty delimiter frame has non-zero size: %zu", delimResult2.value());
        }

        // Receive ZmqFileComplete header
        zmq::message_t completeMsg;
        auto completeResult = zmqSocket->recv(completeMsg, zmq::recv_flags::none);
        if (!completeResult || completeResult.value() != sizeof(ZmqFileComplete)) {
            MIDDLEWARE_LOG_ERROR("Failed to receive file complete message");
            return false;
        }
        const ZmqFileComplete* complete = reinterpret_cast<const ZmqFileComplete*>(completeMsg.data());

        if (complete->message_type != static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE)) {
            MIDDLEWARE_LOG_WARNING("Expected RESP_FILE_COMPLETE after file list, got: %u",
                                  complete->message_type);
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

        // Wait for initial response
        if (!waitForResponse(request.request_id, timeoutMs)) {
            MIDDLEWARE_LOG_ERROR("Timeout waiting for file list response");
            return false;
        }

        std::vector<FileInfo> allFileInfos;
        auto startTime = std::chrono::steady_clock::now();
        auto timeoutDuration = std::chrono::milliseconds(timeoutMs);
        
        bool receivedAtLeastOneResponse = false;
        
        do {
            // Check if we've exceeded timeout
            auto currentTime = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
            if (elapsed >= timeoutDuration) {
                MIDDLEWARE_LOG_INFO("Timeout reached after %lld ms", elapsed.count());
                break;
            }
            
            // Check if there's more data available with remaining timeout
            int remainingTimeout = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(timeoutDuration - elapsed).count());
            if (remainingTimeout <= 0) {
                break;
            }
            
            // Set socket to non-blocking for poll
            zmqSocket->set(zmq::sockopt::rcvtimeo, remainingTimeout);
            
            // Receive empty delimiter frame (zero-length)
            zmq::message_t emptyDelimiter;
            auto delimResult = zmqSocket->recv(emptyDelimiter, zmq::recv_flags::none);
            if (!delimResult) {
                // No more data available
                break;
            }
            if (delimResult.value() != 0) {
                MIDDLEWARE_LOG_WARNING("Empty delimiter frame has non-zero size: %zu", delimResult.value());
            }

            // Receive the combined header+data ZeroMQ message
            zmq::message_t msg;
            auto result = zmqSocket->recv(msg, zmq::recv_flags::none);
            
            if (!result) {
                // No more data
                break;
            }
            
            size_t totalSize = result.value();
            MIDDLEWARE_LOG_INFO("Received file list response: %zu bytes total", totalSize);
            
            size_t headerSize = sizeof(ZmqFileChunk);
            // Check minimum size (header)
            if (totalSize < headerSize) {
                MIDDLEWARE_LOG_ERROR("File list response too small: %zu bytes (expected at least %zu)", 
                                    totalSize, headerSize);
                continue; // Skip this malformed response
            }
            
            // Parse header from first part of message
            const uint8_t* msgData = static_cast<const uint8_t*>(msg.data());
            const ZmqFileChunk* chunk = reinterpret_cast<const ZmqFileChunk*>(msgData);
            
            // Validate response
            if (!MessageUtils::isValidMagic(chunk->magic)) {
                MIDDLEWARE_LOG_ERROR("Invalid magic number in file list response");
                continue;
            }

            if (chunk->message_type != static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK)) {
                MIDDLEWARE_LOG_ERROR("Unexpected message type in file list response: %u (expected RESP_FILE_CHUNK=201)", 
                                    chunk->message_type);
                continue;
            }

            // Check if this is a file list response (filename should be "filelist.json")
            std::string filename = chunk->getFilename();
            if (filename != "filelist.json") {
                MIDDLEWARE_LOG_WARNING("File list response has unexpected filename: %s", filename.c_str());
            }

            MIDDLEWARE_LOG_INFO("Received file list chunk header: %llu bytes from rank %d, chunk size: %u", 
                                chunk->file_size, chunk->source_rank, chunk->chunk_size);
            
            // Extract JSON data from remaining part of message
            std::vector<uint8_t> jsonData;
            if (chunk->chunk_size > 0) {
                // Check if we have enough data
                size_t expectedTotalSize = headerSize + chunk->chunk_size;
                if (totalSize < expectedTotalSize) {
                    MIDDLEWARE_LOG_ERROR("Incomplete file list response: got %zu bytes, expected %zu", 
                                        totalSize, expectedTotalSize);
                    continue;
                }
                
                // Copy JSON data from message
                jsonData.assign(msgData + headerSize, msgData + headerSize + chunk->chunk_size);
                MIDDLEWARE_LOG_INFO("Extracted JSON data: %zu bytes", jsonData.size());
            } else {
                MIDDLEWARE_LOG_WARNING("File list response has zero chunk size");
            }

            // Parse JSON into FileInfo vector with source rank
            try {
                std::string jsonStr(reinterpret_cast<const char*>(jsonData.data()), jsonData.size());
                auto json = json::parse(jsonStr);
                
                // Check JSON structure: {"rank": X, "files": [{"name": "...", "size": N, "mime": "..."}, ...]}
                // Note: JSON may contain "rank" field, but we use chunk->source_rank which is more reliable
                int32_t jsonRank = chunk->source_rank;
                if (json.contains("rank") && json["rank"].is_number()) {
                    // Use JSON rank if available, but chunk->source_rank should be authoritative
                    int32_t jsonRankValue = json["rank"].get<int32_t>();
                    if (jsonRankValue != chunk->source_rank) {
                        MIDDLEWARE_LOG_WARNING("JSON rank (%d) doesn't match chunk source_rank (%d), using chunk value", 
                                              jsonRankValue, chunk->source_rank);
                    }
                }
                
                if (json.contains("files") && json["files"].is_array()) {
                    for (const auto& fileObj : json["files"]) {
                        if (fileObj.contains("name") && fileObj["name"].is_string()) {
                            std::string fname = fileObj["name"].get<std::string>();
                            if (!fname.empty()) {
                                uint64_t fsize = 0;
                                if (fileObj.contains("size") && fileObj["size"].is_number()) {
                                    fsize = fileObj["size"].get<uint64_t>();
                                }
                                // Store file with source rank
                                allFileInfos.push_back({fname, fsize, jsonRank});
                            }
                        }
                    }
                }
                
                MIDDLEWARE_LOG_INFO("Parsed %zu files with sizes from rank %d", allFileInfos.size() - (allFileInfos.size() - jsonData.size()), jsonRank);
            } catch (const std::exception& e) {
                MIDDLEWARE_LOG_ERROR("Failed to parse file list JSON: %s", e.what());
                continue;
            }

            // Receive empty delimiter for complete message
            zmq::message_t emptyDelimiter2;
            auto delimResult2 = zmqSocket->recv(emptyDelimiter2, zmq::recv_flags::none);
            if (!delimResult2) {
                MIDDLEWARE_LOG_ERROR("Failed to receive empty delimiter for complete message");
                continue;
            }
            if (delimResult2.value() != 0) {
                MIDDLEWARE_LOG_WARNING("Empty delimiter frame has non-zero size: %zu", delimResult2.value());
            }

            // Receive ZmqFileComplete header
            zmq::message_t completeMsg;
            auto completeResult = zmqSocket->recv(completeMsg, zmq::recv_flags::none);
            if (!completeResult || completeResult.value() != sizeof(ZmqFileComplete)) {
                MIDDLEWARE_LOG_ERROR("Failed to receive file complete message");
                continue;
            }
            const ZmqFileComplete* complete = reinterpret_cast<const ZmqFileComplete*>(completeMsg.data());

            if (complete->message_type != static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE)) {
                MIDDLEWARE_LOG_WARNING("Expected RESP_FILE_COMPLETE after file list, got: %u",
                                      complete->message_type);
            }
            
            receivedAtLeastOneResponse = true;
            connectionStats.totalResponsesReceived.fetch_add(1);
            
            // If not broadcast, we're done after first response
            if (!isBroadcast) {
                break;
            }
            
            // For broadcast, continue receiving until timeout
            // Reset socket timeout for next poll
            zmqSocket->set(zmq::sockopt::rcvtimeo, -1);
            
        } while (isBroadcast);
        
        // Reset socket to default blocking mode
        zmqSocket->set(zmq::sockopt::rcvtimeo, -1);
        
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
            // Track which ranks responded - use dynamic size based on totalWorkers
            std::vector<bool> responded(totalWorkers, false);
            int32_t maxRank = -1;
            int32_t minRank = static_cast<int32_t>(totalWorkers);
            
            for (const auto& fileInfo : allFileInfos) {
                if (fileInfo.source_rank >= 0 && fileInfo.source_rank < static_cast<int32_t>(totalWorkers)) {
                    responded[fileInfo.source_rank] = true;
                    if (fileInfo.source_rank > maxRank) maxRank = fileInfo.source_rank;
                    if (fileInfo.source_rank < minRank) minRank = fileInfo.source_rank;
                }
            }
            
            // Count responded ranks
            int32_t respondedCount = 0;
            for (int32_t rank = 0; rank < static_cast<int32_t>(totalWorkers); ++rank) {
                if (responded[rank]) respondedCount++;
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
                    if (!responded[rank]) {
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
                            
                            // Wait for response
                            if (!waitForResponse(request.request_id, retryTimeout)) {
                                MIDDLEWARE_LOG_WARNING("Retry timeout for rank %d after %d ms", rank, retryTimeout);
                                continue;
                            }
                            
                            // Receive the response
                            zmq::message_t emptyDelimiter;
                            auto delimResult = zmqSocket->recv(emptyDelimiter, zmq::recv_flags::none);
                            if (!delimResult) {
                                MIDDLEWARE_LOG_ERROR("Retry failed to receive delimiter for rank %d", rank);
                                continue;
                            }
                            
                            zmq::message_t msg;
                            auto recvResult = zmqSocket->recv(msg, zmq::recv_flags::none);
                            if (!recvResult) {
                                MIDDLEWARE_LOG_ERROR("Retry failed to receive message for rank %d", rank);
                                continue;
                            }
                            
                            size_t totalSize = recvResult.value();
                            if (totalSize < sizeof(ZmqFileChunk)) {
                                MIDDLEWARE_LOG_ERROR("Retry response too small from rank %d: %zu bytes", rank, totalSize);
                                continue;
                            }
                            
                            // Parse the response
                            const uint8_t* msgData = static_cast<const uint8_t*>(msg.data());
                            const ZmqFileChunk* chunk = reinterpret_cast<const ZmqFileChunk*>(msgData);
                            
                            if (!MessageUtils::isValidMagic(chunk->magic)) {
                                MIDDLEWARE_LOG_ERROR("Retry invalid magic from rank %d", rank);
                                continue;
                            }
                            
                            if (chunk->message_type != static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK)) {
                                MIDDLEWARE_LOG_ERROR("Retry wrong message type from rank %d: %u", rank, chunk->message_type);
                                continue;
                            }
                            
                            // Extract JSON data
                            size_t headerSize = sizeof(ZmqFileChunk);
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
                            
                            // Clean up remaining message parts
                            zmq::message_t emptyDelimiter2;
                            zmqSocket->recv(emptyDelimiter2, zmq::recv_flags::none);
                            zmq::message_t completeMsg;
                            zmqSocket->recv(completeMsg, zmq::recv_flags::none);
                            
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

    // Lock to prevent multiple concurrent file requests
    std::lock_guard<std::recursive_mutex> lock(requestMutex);

    try {
        // Create file request
        ZmqFileRequest request;
        request.message_type = static_cast<uint32_t>(ZmqMessageType::REQ_GET_FILE);
        request.request_id = generateRequestId();
        request.target_rank = targetRank;
        request.chunk_size = DEFAULT_CHUNK_SIZE;
        request.setFilename(filename);

        MIDDLEWARE_LOG_INFO("Requesting file '%s' from rank %d (request_id: %u)", 
                            filename.c_str(), targetRank, request.request_id);

        // Send request
        if (!sendRequest(&request, sizeof(request), request.request_id)) {
            MIDDLEWARE_LOG_ERROR("Failed to send file request");
            return false;
        }

        // Receive file in chunks
        bool fileComplete = false;
        uint64_t totalSize = 0;

        while (!fileComplete && !shutdownRequested.load()) {
            // Poll for message
            zmq::pollitem_t items[] = {{ zmqSocket->handle(), 0, ZMQ_POLLIN, 0 }};
            int pollResult = zmq::poll(items, 1, std::chrono::milliseconds(timeoutMs));

            if (pollResult <= 0) {
                MIDDLEWARE_LOG_ERROR("Timeout waiting for file chunk");
                if (errorCallback) {
                    errorCallback("Timeout waiting for file chunk");
                }
                return false;
            }

            // Receive empty delimiter frame (zero-length)
            zmq::message_t emptyDelimiter;
            auto delimResult = zmqSocket->recv(emptyDelimiter, zmq::recv_flags::none);
            if (!delimResult) {
                MIDDLEWARE_LOG_ERROR("Failed to receive empty delimiter");
                return false;
            }
            if (delimResult.value() != 0) {
                MIDDLEWARE_LOG_WARNING("Empty delimiter frame has non-zero size: %zu", delimResult.value());
            }

            // Receive combined header+data frame
            zmq::message_t combined;
            auto combResult = zmqSocket->recv(combined, zmq::recv_flags::none);
            if (!combResult) {
                MIDDLEWARE_LOG_ERROR("Failed to receive combined header+data");
                return false;
            }
            size_t combinedSize = combResult.value();
            if (combinedSize < 8) { // need at least magic + message_type
                MIDDLEWARE_LOG_ERROR("Combined message too small: %zu bytes", combinedSize);
                return false;
            }

            const uint8_t* combinedData = static_cast<const uint8_t*>(combined.data());
            // Parse magic and message type
            uint32_t magic = *reinterpret_cast<const uint32_t*>(combinedData);
            if (!MessageUtils::isValidMagic(magic)) {
                MIDDLEWARE_LOG_ERROR("Invalid magic number in response: 0x%08X", magic);
                return false;
            }
            uint32_t messageType = *reinterpret_cast<const uint32_t*>(combinedData + 4);

            // Handle based on message type
            switch (static_cast<ZmqMessageType>(messageType)) {
                case ZmqMessageType::RESP_FILE_CHUNK: {
                    if (combinedSize < sizeof(ZmqFileChunk)) {
                        MIDDLEWARE_LOG_ERROR("File chunk response too small: %zu bytes (expected %zu)",
                                            combinedSize, sizeof(ZmqFileChunk));
                        return false;
                    }
                    const ZmqFileChunk* chunk = reinterpret_cast<const ZmqFileChunk*>(combinedData);
                    size_t dataSize = combinedSize - sizeof(ZmqFileChunk);
                    if (dataSize != chunk->chunk_size) {
                        MIDDLEWARE_LOG_WARNING("Chunk size mismatch: header %u, data %zu",
                                              chunk->chunk_size, dataSize);
                        // proceed anyway
                    }

                    // Extract chunk data
                    std::vector<uint8_t> chunkData;
                    if (dataSize > 0) {
                        chunkData.assign(combinedData + sizeof(ZmqFileChunk),
                                         combinedData + sizeof(ZmqFileChunk) + dataSize);
                    }

                    // Trigger chunk callback
                    if (chunkCallback) {
                        chunkCallback(chunk->getFilename(), chunkData, chunk->chunk_offset, chunk->file_size);
                    }

                    connectionStats.totalBytesReceived.fetch_add(dataSize);
                    totalSize = chunk->file_size;
                    break;
                }

                case ZmqMessageType::RESP_FILE_COMPLETE: {
                    if (combinedSize < sizeof(ZmqFileComplete)) {
                        MIDDLEWARE_LOG_ERROR("File complete response too small: %zu bytes (expected %zu)",
                                            combinedSize, sizeof(ZmqFileComplete));
                        return false;
                    }
                    const ZmqFileComplete* complete = reinterpret_cast<const ZmqFileComplete*>(combinedData);

                    fileComplete = true;
                    if (completeCallback) {
                        completeCallback(complete->getFilename(), complete->total_size);
                    }

                    MIDDLEWARE_LOG_INFO("File transfer complete: %s (%zu bytes)",
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
                        return false;
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

    // Lock to prevent multiple concurrent frame requests
    std::lock_guard<std::recursive_mutex> lock(requestMutex);

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

        MIDDLEWARE_LOG_INFO("Requesting frame %d from rank %d (request_id: %u)", 
                            frameNumber, targetRank, request.request_id);

        // Send request
        if (!sendRequest(&request, sizeof(request), request.request_id)) {
            MIDDLEWARE_LOG_ERROR("Failed to send frame request");
            return false;
        }

        // Receive frame files (similar to requestFile but may receive multiple files)
        bool frameComplete = false;
        int filesReceived = 0;

        while (!frameComplete && !shutdownRequested.load()) {
            // Poll for message
            zmq::pollitem_t items[] = {{ zmqSocket->handle(), 0, ZMQ_POLLIN, 0 }};
            int pollResult = zmq::poll(items, 1, std::chrono::milliseconds(timeoutMs));

            if (pollResult <= 0) {
                MIDDLEWARE_LOG_ERROR("Timeout waiting for frame data");
                if (errorCallback) {
                    errorCallback("Timeout waiting for frame data");
                }
                return false;
            }

            // Receive message type
            uint32_t messageType;
            zmq::message_t msgType;
            auto res = zmqSocket->recv(msgType, zmq::recv_flags::none);
            if (!res || res.value() != sizeof(messageType)) {
                MIDDLEWARE_LOG_ERROR("Failed to receive message type");
                return false;
            }
            messageType = *static_cast<uint32_t*>(msgType.data());

            // Handle based on message type
            switch (static_cast<ZmqMessageType>(messageType)) {
                case ZmqMessageType::RESP_FILE_CHUNK: {
                    ZmqFileChunk chunk;
                    if (!receiveResponse(&chunk, sizeof(chunk), timeoutMs)) {
                        MIDDLEWARE_LOG_ERROR("Failed to receive file chunk header");
                        return false;
                    }

                    // Receive chunk data
                    std::vector<uint8_t> chunkData(chunk.chunk_size);
                    if (chunk.chunk_size > 0) {
                        if (!receiveResponse(chunkData.data(), chunkData.size(), timeoutMs)) {
                            MIDDLEWARE_LOG_ERROR("Failed to receive chunk data");
                            return false;
                        }
                    }

                    // Trigger chunk callback
                    if (chunkCallback) {
                        chunkCallback(chunk.getFilename(), chunkData, chunk.chunk_offset, chunk.file_size);
                    }

                    connectionStats.totalBytesReceived.fetch_add(chunk.chunk_size);
                    break;
                }

                case ZmqMessageType::RESP_FILE_COMPLETE: {
                    ZmqFileComplete complete;
                    if (!receiveResponse(&complete, sizeof(complete), timeoutMs)) {
                        MIDDLEWARE_LOG_ERROR("Failed to receive file complete message");
                        return false;
                    }

                    filesReceived++;
                    if (completeCallback) {
                        completeCallback(complete.getFilename(), complete.total_size);
                    }

                    MIDDLEWARE_LOG_INFO("Frame file %d complete: %s (%zu bytes)", 
                                        filesReceived, complete.getFilename().c_str(), complete.total_size);
                    break;
                }

                case ZmqMessageType::RESP_NO_FILE: {
                    MIDDLEWARE_LOG_ERROR("Frame not found: %d", frameNumber);
                    if (errorCallback) {
                        errorCallback("Frame not found: " + std::to_string(frameNumber));
                    }
                    return false;
                }

                case ZmqMessageType::RESP_ERROR: {
                    ZmqErrorResponse error;
                    if (!receiveResponse(&error, sizeof(error), timeoutMs)) {
                        MIDDLEWARE_LOG_ERROR("Failed to receive error message");
                        return false;
                    }

                    MIDDLEWARE_LOG_ERROR("Error response: %s", error.getErrorMessage().c_str());
                    if (errorCallback) {
                        errorCallback(error.getErrorMessage());
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
        return true;

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
    
    auto chunkCallback = [&file](const std::string& fname, const std::vector<uint8_t>& chunk,
                                  uint64_t offset, uint64_t totalSize) {
        file.totalSize = totalSize;
        if (file.data.size() < offset + chunk.size()) {
            file.data.resize(offset + chunk.size());
        }
        std::copy(chunk.begin(), chunk.end(), file.data.begin() + offset);
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
        MIDDLEWARE_LOG_INFO("DEBUG sendRequest: magic=0x%08x, type=%u, size=%zu, request_id=%u",
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
        
        MIDDLEWARE_LOG_INFO("DEBUG sendRequest: Empty delimiter sent successfully");
        
        // Send binary struct (second/last frame, no SNDMORE flag)
        zmq::message_t msg(size);
        memcpy(msg.data(), data, size);
        auto result2 = zmqSocket->send(msg, zmq::send_flags::none);
        
        if (!result2 || result2.value() != size) {
            MIDDLEWARE_LOG_ERROR("Failed to send request data (request_id: %u)", requestId);
            connectionStats.failedRequests.fetch_add(1);
            return false;
        }

        MIDDLEWARE_LOG_INFO("DEBUG sendRequest: Request data sent successfully (%zu bytes)", size);
        connectionStats.totalRequestsSent.fetch_add(1);
        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error sending request: %s (errno: %d)", e.what(), e.num());
        connectionStats.failedRequests.fetch_add(1);
        return false;
    }
}

bool AnariUsdClient::receiveResponse(void* buffer, size_t size, int timeoutMs) {
    // First wait for data to be available with cancellation support
    if (!waitForResponse(0, timeoutMs)) { // Use 0 as requestId since we already waited
        return false;
    }
    
    try {
        zmq::message_t msg;
        auto result = zmqSocket->recv(msg, zmq::recv_flags::none);
        
        if (!result || result.value() != size) {
            MIDDLEWARE_LOG_ERROR("Failed to receive response (expected %zu bytes, got %zu)",
                                size, result ? result.value() : 0);
            return false;
        }

        memcpy(buffer, msg.data(), size);
        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error receiving response: %s (errno: %d)", e.what(), e.num());
        return false;
    }
}

uint32_t AnariUsdClient::generateRequestId() {
    return nextRequestId.fetch_add(1);
}

bool AnariUsdClient::waitForResponse(uint32_t requestId, int timeoutMs) {
    // For DEALER socket, we just poll for availability
    // Use polling with smaller intervals to allow cancellation checks
    const int POLL_INTERVAL_MS = 100; // Check every 100ms
    
    int remainingTime = timeoutMs;
    auto startTime = std::chrono::steady_clock::now();
    
    while (remainingTime > 0) {
        // Check if we should cancel (connection status changed or shutdown requested)
        auto status = connectionStatus.load();
        if (status != ConnectionStatus::Connected || shutdownRequested.load()) {
            MIDDLEWARE_LOG_WARNING("Connection status changed during wait (status: %d, shutdown: %d), cancelling",
                                  static_cast<int>(status), shutdownRequested.load());
            return false;
        }
        
        // Poll with smaller interval
        int pollTimeout = std::min(POLL_INTERVAL_MS, remainingTime);
        zmq::pollitem_t items[] = {{ zmqSocket->handle(), 0, ZMQ_POLLIN, 0 }};
        int pollResult = zmq::poll(items, 1, std::chrono::milliseconds(pollTimeout));
        
        if (pollResult > 0) {
            return true; // Data available
        }
        
        // Update remaining time
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
        remainingTime = timeoutMs - static_cast<int>(elapsed.count());
    }
    
    return false; // Timeout
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
        // ZeroMQ automatically adds identity frame for DEALER→ROUTER communication
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

        // Wait for response with cancellation support
        if (!waitForResponse(0, timeoutMs)) {
            MIDDLEWARE_LOG_ERROR("Timeout waiting for worker list response");
            return false;
        }

        // Receive response (2 frames: empty delimiter + data)
        // First frame: empty delimiter (should be 0 bytes)
        zmq::message_t delimiterMsg;
        auto delimResult = zmqSocket->recv(delimiterMsg, zmq::recv_flags::none);
        if (!delimResult) {
            MIDDLEWARE_LOG_ERROR("Failed to receive delimiter frame");
            return false;
        }
        
        // Second frame: string response
        zmq::message_t responseMsg;
        auto recvResult = zmqSocket->recv(responseMsg, zmq::recv_flags::none);
        if (!recvResult) {
            MIDDLEWARE_LOG_ERROR("Failed to receive worker list response");
            return false;
        }

        // Parse response: "WORKER_LIST|rank1:hostname1:ip1;rank2:hostname2:ip2;..."
        std::string responseStr(static_cast<const char*>(responseMsg.data()), responseMsg.size());
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
        MIDDLEWARE_LOG_INFO("=== REQUESTING WORKER COUNT USING ANARI-USD BINARY PROTOCOL ===");
        
        // Create binary property request using ZmqFileRequest struct
        ZmqFileRequest request;
        request.magic = ANARI_USD_MAGIC;           // 0x55534446 ("USDF")
        request.message_type = static_cast<uint32_t>(ZmqMessageType::REQ_GET_PROPERTY);  // 400
        request.request_id = generateRequestId();
        request.target_rank = -1;                  // Request from all ranks
        request.setFilename("totalWorkerCount");   // Use totalWorkerCount (includes rank 0) not workerCount (excludes rank 0)
        request.chunk_size = 0;                    // Not applicable for property requests
        
        MIDDLEWARE_LOG_INFO("DEBUG: Creating binary property request:");
        MIDDLEWARE_LOG_INFO("  magic=0x%08x (ANARI_USD_MAGIC)", request.magic);
        MIDDLEWARE_LOG_INFO("  message_type=%u (REQ_GET_PROPERTY)", request.message_type);
        MIDDLEWARE_LOG_INFO("  request_id=%u", request.request_id);
        MIDDLEWARE_LOG_INFO("  target_rank=%d (broadcast to all ranks)", request.target_rank);
        
        // CRITICAL DEBUG: Log the actual filename buffer content
        std::string actualFilename = request.getFilename();
        MIDDLEWARE_LOG_INFO("  property='%s' (length=%zu)", actualFilename.c_str(), actualFilename.length());
        
        // Also log raw buffer to check for null termination issues
        MIDDLEWARE_LOG_INFO("  raw filename buffer (first 32 chars):");
        for (int i = 0; i < 32 && i < 256; i++) {
            char c = request.filename[i];
            if (c == 0) break;
            MIDDLEWARE_LOG_INFO("    [%d] = '%c' (0x%02x)", i, c, (unsigned char)c);
        }
        
        MIDDLEWARE_LOG_INFO("  chunk_size=%u", request.chunk_size);
        
        std::lock_guard<std::recursive_mutex> lock(requestMutex);
        
        // Send binary struct (276 bytes) using existing sendRequest method
        if (!sendRequest(&request, sizeof(request), request.request_id)) {
            MIDDLEWARE_LOG_ERROR("Failed to send binary property request");
            return false;
        }

        connectionStats.totalRequestsSent.fetch_add(1);

        // Wait for response with cancellation support
        // Use the actual request_id, not 0
        if (!waitForResponse(request.request_id, timeoutMs)) {
            MIDDLEWARE_LOG_ERROR("Timeout waiting for property response");
            return false;
        }

        // Receive binary property response (2 frames: empty delimiter + data)
        // First frame: empty delimiter (should be 0 bytes)
        zmq::message_t delimiterMsg;
        auto delimResult = zmqSocket->recv(delimiterMsg, zmq::recv_flags::none);
        if (!delimResult) {
            MIDDLEWARE_LOG_ERROR("Failed to receive delimiter frame");
            return false;
        }
        
        // Second frame: binary property response
        zmq::message_t responseMsg;
        auto recvResult = zmqSocket->recv(responseMsg, zmq::recv_flags::none);
        if (!recvResult) {
            MIDDLEWARE_LOG_ERROR("Failed to receive property response");
            return false;
        }

        // Validate response size
        size_t responseSize = recvResult.value();
        if (responseSize < sizeof(ZmqPropertyResponse)) {
            MIDDLEWARE_LOG_ERROR("Property response too small: %zu bytes (expected at least %zu)",
                                responseSize, sizeof(ZmqPropertyResponse));
            return false;
        }

        // Parse binary property response
        const ZmqPropertyResponse* response = reinterpret_cast<const ZmqPropertyResponse*>(responseMsg.data());
        
        MIDDLEWARE_LOG_INFO("DEBUG: Received binary property response:");
        MIDDLEWARE_LOG_INFO("  responseSize=%zu bytes", responseSize);
        MIDDLEWARE_LOG_INFO("  response->magic=0x%08x", response->magic);
        MIDDLEWARE_LOG_INFO("  response->message_type=%u", response->message_type);
        MIDDLEWARE_LOG_INFO("  response->request_id=%u", response->request_id);
        MIDDLEWARE_LOG_INFO("  response->property_type=%d", response->property_type);
        MIDDLEWARE_LOG_INFO("  response->int_value=%lld", response->int_value);
        
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
        
        MIDDLEWARE_LOG_INFO("DEBUG: Parsed worker count: totalWorkers=%u", totalWorkers);
        MIDDLEWARE_LOG_INFO("Received property response: worker_count=%u, property_type=%d",
                           totalWorkers, response->property_type);

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

        std::lock_guard<std::recursive_mutex> lock(requestMutex);
        
        // Send request
        if (!sendRequest(&request, sizeof(request), request.request_id)) {
            MIDDLEWARE_LOG_ERROR("Failed to send worker status property request");
            return false;
        }

        connectionStats.totalRequestsSent.fetch_add(1);

        // Wait for response
        if (!waitForResponse(request.request_id, timeoutMs)) {
            MIDDLEWARE_LOG_ERROR("Timeout waiting for worker status property response");
            return false;
        }

        // Receive property response (2 frames: empty delimiter + data)
        // First frame: empty delimiter (should be 0 bytes)
        zmq::message_t delimiterMsg;
        auto delimResult = zmqSocket->recv(delimiterMsg, zmq::recv_flags::none);
        if (!delimResult) {
            MIDDLEWARE_LOG_ERROR("Failed to receive delimiter frame");
            return false;
        }
        
        // Second frame: binary property response
        zmq::message_t responseMsg;
        auto recvResult = zmqSocket->recv(responseMsg, zmq::recv_flags::none);
        if (!recvResult) {
            MIDDLEWARE_LOG_ERROR("Failed to receive property response");
            return false;
        }

        // Validate response size
        size_t responseSize = recvResult.value();
        if (responseSize < sizeof(ZmqPropertyResponse)) {
            MIDDLEWARE_LOG_ERROR("Property response too small: %zu bytes (expected at least %zu)",
                                responseSize, sizeof(ZmqPropertyResponse));
            return false;
        }

        // Parse property response
        const ZmqPropertyResponse* response = reinterpret_cast<const ZmqPropertyResponse*>(responseMsg.data());
        
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
    MIDDLEWARE_LOG_INFO("=== getTotalWorkerCountSync ENTERED (timeout=%d ms) ===", timeoutMs);
    
    // Use binary protocol instead of legacy string protocol
    std::promise<uint32_t> promise;
    std::future<uint32_t> future = promise.get_future();
    
    bool success = requestWorkerCount([&promise](uint32_t count) {
        MIDDLEWARE_LOG_INFO("DEBUG: Worker count callback received: count=%u", count);
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
    MIDDLEWARE_LOG_INFO("=== getTotalWorkerCountSync COMPLETE: totalCount=%u ===", totalCount);
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
        MIDDLEWARE_LOG_INFO("=== ENTERING requestFilesParallel ===");
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
    
    MIDDLEWARE_LOG_INFO("Client is connected");
    
    if (filenames.size() != target_ranks.size()) {
        MIDDLEWARE_LOG_ERROR("Filename count (%zu) doesn't match target_ranks count (%zu)",
                           filenames.size(), target_ranks.size());
        return false;
    }
    
    MIDDLEWARE_LOG_INFO("Input validation passed");
    
    // Create parallel download manager if not already created
    MIDDLEWARE_LOG_INFO("Checking parallelDownloadManager: %p", parallelDownloadManager.get());
    if (!parallelDownloadManager) {
        MIDDLEWARE_LOG_INFO("Creating new ParallelDownloadManager instance");
        try {
            MIDDLEWARE_LOG_INFO("Attempting to create shared_ptr from this: %p", this);
            auto shared_this = std::shared_ptr<AnariUsdClient>(this, [](auto*) {});
            MIDDLEWARE_LOG_INFO("shared_ptr created successfully");
            
            MIDDLEWARE_LOG_INFO("Calling std::make_unique<ParallelDownloadManager>");
            parallelDownloadManager = std::make_unique<ParallelDownloadManager>(
                shared_this, // shared_ptr with no-op deleter
                4 // max parallel downloads
            );
            MIDDLEWARE_LOG_INFO("ParallelDownloadManager created successfully at: %p", parallelDownloadManager.get());
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Failed to create ParallelDownloadManager: %s", e.what());
            if (error_callback) {
                for (const auto& filename : filenames) {
                    error_callback(filename, std::string("Failed to create download manager: ") + e.what());
                }
            }
            return false;
        } catch (...) {
            MIDDLEWARE_LOG_ERROR("Failed to create ParallelDownloadManager: unknown exception");
            if (error_callback) {
                for (const auto& filename : filenames) {
                    error_callback(filename, "Failed to create download manager: unknown exception");
                }
            }
            return false;
        }
    }
    
    MIDDLEWARE_LOG_INFO("parallelDownloadManager after creation: %p", parallelDownloadManager.get());
    
    if (!parallelDownloadManager) {
        MIDDLEWARE_LOG_ERROR("ParallelDownloadManager is null after creation attempt");
        if (error_callback) {
            for (const auto& filename : filenames) {
                error_callback(filename, "Download manager is null");
            }
        }
        return false;
    }
    
    MIDDLEWARE_LOG_INFO("Starting parallel download of %zu files using same logic as async node", filenames.size());
    
    // Create shared state for tracking all downloads
    struct ParallelDownloadState {
        std::atomic<size_t> completed_files{0};
        std::atomic<size_t> successful_files{0};
        std::mutex completion_mutex;
    };
    
    auto state = std::make_shared<ParallelDownloadState>();
    size_t total_files = filenames.size();
    
    // Start a download for each file (same pattern as getFileSync but async)
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
        std::function<void(const std::string&, const std::vector<uint8_t>&, uint64_t, uint64_t)> chunk_callback = 
            [file_state](const std::string& fname, const std::vector<uint8_t>& chunk,
                        uint64_t offset, uint64_t total_size) {
                file_state->total_size = total_size;
                if (file_state->data.size() < offset + chunk.size()) {
                    file_state->data.resize(offset + chunk.size());
                }
                std::copy(chunk.begin(), chunk.end(), file_state->data.begin() + offset);
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
        
        // Start the async download (non-blocking)
        bool started = this->requestFile(filename, target_rank, 
                                        chunk_callback, complete_callback, 
                                        error_callback_wrapper, timeout_ms);
        
        if (!started) {
            MIDDLEWARE_LOG_ERROR("Failed to start parallel download for: %s", filename.c_str());
            
            if (error_callback) {
                error_callback(filename, "Failed to start download");
            }
            
            // Count as completed (with error)
            state->completed_files.fetch_add(1);
        }
    }
    
    // Return true immediately - downloads run asynchronously
    // Callbacks will handle completion and spawning
    return true;
}

void AnariUsdClient::cleanup() {
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
}

} // namespace anari_usd_middleware