#include "ZmqConnector.h"
#include "MiddlewareLogging.h"
#include <algorithm>
#include <regex>
#include <filesystem>
#include <thread>

namespace anari_usd_middleware {

ZmqConnector::ZmqConnector() {
    MIDDLEWARE_LOG_INFO("ZmqConnector created with enhanced safety features");
    messageStats.reset();
    lastHealthCheck = std::chrono::steady_clock::now();
}

ZmqConnector::~ZmqConnector() {
    MIDDLEWARE_LOG_INFO("ZmqConnector destructor called");
    disconnect(500); // Quick shutdown in destructor
}

bool ZmqConnector::initialize(const char* endpoint, int timeoutMs) {
    std::lock_guard<std::mutex> lock(connectionMutex);

    if (connectionStatus.load() == ConnectionStatus::Connected) {
        MIDDLEWARE_LOG_WARNING("ZmqConnector already initialized and connected");
        return true;
    }

    MIDDLEWARE_LOG_INFO("Initializing ZmqConnector with timeout %dms", timeoutMs);
    connectionStatus.store(ConnectionStatus::Connecting);

    try {
        // Validate timeout
        if (timeoutMs <= 0 || timeoutMs > 30000) {
            MIDDLEWARE_LOG_ERROR("Invalid timeout value: %d (must be 1-30000ms)", timeoutMs);
            connectionStatus.store(ConnectionStatus::Error);
            return false;
        }

        // Initialize ZMQ context with enhanced settings
        zmqContext = std::make_unique<zmq::context_t>(1); // 1 I/O thread
        if (!zmqContext) {
            MIDDLEWARE_LOG_ERROR("Failed to create ZMQ context");
            connectionStatus.store(ConnectionStatus::Error);
            return false;
        }

        // Set context options for better performance and safety
        zmqContext->set(zmq::ctxopt::max_sockets, 1024);
        zmqContext->set(zmq::ctxopt::io_threads, 1);

        // Create ROUTER socket with enhanced configuration
        zmqSocket = std::make_unique<zmq::socket_t>(*zmqContext, zmq::socket_type::router);
        if (!zmqSocket) {
            MIDDLEWARE_LOG_ERROR("Failed to create ZMQ ROUTER socket");
            cleanup();
            connectionStatus.store(ConnectionStatus::Error);
            return false;
        }

        // Set comprehensive socket options for safety and performance
        zmqSocket->set(zmq::sockopt::linger, 0);                    // No lingering on close
        zmqSocket->set(zmq::sockopt::sndhwm, 1000);                 // Send high water mark
        zmqSocket->set(zmq::sockopt::rcvhwm, 1000);                 // Receive high water mark
        zmqSocket->set(zmq::sockopt::sndtimeo, timeoutMs);          // Send timeout
        zmqSocket->set(zmq::sockopt::rcvtimeo, timeoutMs);          // Receive timeout
        zmqSocket->set(zmq::sockopt::maxmsgsize, static_cast<int64_t>(maxMessageSize.load())); // Max message size
        zmqSocket->set(zmq::sockopt::router_mandatory, 1);          // Mandatory routing

        // Determine endpoint
        currentEndpoint = endpoint ? endpoint : "tcp://*:5556";

        // Validate endpoint format
        std::regex endpointPattern(R"(^(tcp|ipc|inproc)://[^:]+:\d+$|^(ipc|inproc)://[^:]+$)");
        if (!std::regex_match(currentEndpoint, endpointPattern)) {
            MIDDLEWARE_LOG_ERROR("Invalid endpoint format: %s", currentEndpoint.c_str());
            cleanup();
            connectionStatus.store(ConnectionStatus::Error);
            return false;
        }

        // Try binding to the primary endpoint
        try {
            zmqSocket->bind(currentEndpoint);
            MIDDLEWARE_LOG_INFO("ZMQ Router bound successfully to %s", currentEndpoint.c_str());

        } catch (const zmq::error_t& e) {
            MIDDLEWARE_LOG_WARNING("Failed to bind to primary endpoint %s: %s (errno: %d)",
                                 currentEndpoint.c_str(), e.what(), e.num());

            // Try alternative endpoints
            if (!tryAlternativeEndpoints(currentEndpoint)) {
                MIDDLEWARE_LOG_ERROR("All binding attempts failed");
                cleanup();
                connectionStatus.store(ConnectionStatus::Error);
                return false;
            }
        }

        // Reset statistics
        messageStats.reset();

        // Mark as connected
        connectionStatus.store(ConnectionStatus::Connected);
        shutdownRequested.store(false);

        MIDDLEWARE_LOG_INFO("ZmqConnector initialized successfully on %s", currentEndpoint.c_str());
        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error during initialization: %s (errno: %d)", e.what(), e.num());
        cleanup();
        connectionStatus.store(ConnectionStatus::Error);
        return false;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Standard exception during initialization: %s", e.what());
        cleanup();
        connectionStatus.store(ConnectionStatus::Error);
        return false;
    }
}

bool ZmqConnector::receiveFile(std::string& filename, std::vector<uint8_t>& data,
                               std::string& hash, int timeoutMs) {
    MIDDLEWARE_LOG_DEBUG("=== ZMQ RECEIVE FILE CALLED ===");
    MIDDLEWARE_LOG_DEBUG("Timeout: %d ms", timeoutMs);

    if (connectionStatus.load() != ConnectionStatus::Connected || !zmqSocket) {
        MIDDLEWARE_LOG_ERROR("ZmqConnector not connected or socket invalid");
        return false;
    }

    try {
        // Only poll if timeout > 0 (not already polled by caller)
        if (timeoutMs > 0) {
            zmq::pollitem_t items[] = {{ zmqSocket->handle(), 0, ZMQ_POLLIN, 0 }};
            int pollResult = zmq::poll(items, 1, std::chrono::milliseconds(timeoutMs));

            if (pollResult <= 0) {
                return false; // Timeout or error
            }
        } else {
            MIDDLEWARE_LOG_DEBUG("Skipping poll (timeout=0) - assuming message already available");
        }

        // ROUTER receives: [Identity] [Filename] [Content] [Hash] (from your original)

        // Part 1: Receive client identity (automatic from ROUTER)
        zmq::message_t identityMsg;
        auto identityRes = zmqSocket->recv(identityMsg, zmq::recv_flags::none);
        if (!identityRes || identityRes.value() == 0) {
            return false;
        }

        std::string identityStr = std::string(static_cast<const char*>(identityMsg.data()), identityMsg.size());
        MIDDLEWARE_LOG_INFO("✅ Receiving file from DEALER: %s", identityStr.c_str());

        // Store identity for reply
        zmq::message_t clientIdentity;
        clientIdentity.copy(identityMsg);

        // Part 2: Receive filename
        zmq::message_t filenameMsg;
        auto fileRes = zmqSocket->recv(filenameMsg, zmq::recv_flags::none);
        if (!fileRes || fileRes.value() == 0) {
            MIDDLEWARE_LOG_ERROR("Failed to receive filename");
            sendReply(clientIdentity, "ERROR: Missing filename");
            return false;
        }

        filename = filenameMsg.to_string();
        MIDDLEWARE_LOG_INFO("📁 Filename: %s", filename.c_str());

        // FIXED: Use permissive filename validation
        if (filename.empty() || filename.size() > 255) {
            MIDDLEWARE_LOG_ERROR("Invalid filename: %s", filename.c_str());
            sendReply(clientIdentity, "ERROR: Invalid filename");
            return false;
        }

        // Part 3: Receive content
        zmq::message_t contentMsg;
        auto contentRes = zmqSocket->recv(contentMsg, zmq::recv_flags::none);
        if (!contentRes || contentRes.value() == 0) {
            MIDDLEWARE_LOG_ERROR("Failed to receive content");
            sendReply(clientIdentity, "ERROR: Missing content");
            return false;
        }

        // Copy data efficiently (from your original)
        const uint8_t* dataPtr = static_cast<const uint8_t*>(contentMsg.data());
        data.assign(dataPtr, dataPtr + contentMsg.size());
        MIDDLEWARE_LOG_INFO("📦 Content: %zu bytes", data.size());

        // Part 4: Receive hash (final part)
        zmq::message_t hashMsg;
        auto hashRes = zmqSocket->recv(hashMsg, zmq::recv_flags::none);
        if (!hashRes || hashRes.value() == 0) {
            MIDDLEWARE_LOG_ERROR("Failed to receive hash");
            sendReply(clientIdentity, "ERROR: Missing hash");
            return false;
        }

        hash = hashMsg.to_string();
        MIDDLEWARE_LOG_INFO("🔒 Hash: %s", hash.c_str());

        // Send simple reply (from your original)
        bool replyResult = sendReply(clientIdentity, "RECEIVED");
        if (!replyResult) {
            MIDDLEWARE_LOG_WARNING("Failed to send reply after file reception");
        } else {
            MIDDLEWARE_LOG_INFO("✅ Sent RECEIVED reply to DEALER: %s", identityStr.c_str());
        }

        MIDDLEWARE_LOG_INFO("🎉 Successfully received file: %s (%zu bytes)", filename.c_str(), data.size());
        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error in receiveFile: %s (errno: %d)", e.what(), e.num());
        return false;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in receiveFile: %s", e.what());
        return false;
    }
}

// Add these new helper functions to your ZmqConnector class:
bool ZmqConnector::validateFilename(const std::string& filename) const {
    // Check basic constraints
    if (filename.empty() || filename.size() > 255) {
        MIDDLEWARE_LOG_ERROR("Invalid filename length: %zu", filename.size());
        return false;
    }

    const std::string dangerous = "\\:*?\"<>|"; // Removed '/' to allow directory paths
    if (filename.find_first_of(dangerous) != std::string::npos) {
        MIDDLEWARE_LOG_ERROR("Filename contains dangerous characters: %s", filename.c_str());
        return false;
    }

    // Check for path traversal attempts (but allow single-level directory paths)
    if (filename.find("..") != std::string::npos) {
        MIDDLEWARE_LOG_ERROR("Path traversal attempt detected: %s", filename.c_str());
        return false;
    }

    // Reject paths that start with "./" or contain "~/"
    if (filename.find("./") == 0 || filename.find("~/") != std::string::npos) {
        MIDDLEWARE_LOG_ERROR("Relative path attempt detected: %s", filename.c_str());
        return false;
    }

    // Check for absolute paths (Windows and Unix)
    if (filename.size() >= 2 && filename[1] == ':') { // Windows absolute path like "C:"
        MIDDLEWARE_LOG_ERROR("Absolute path detected: %s", filename.c_str());
        return false;
    }

    if (filename[0] == '/' || filename[0] == '\\') { // Unix/Windows absolute path
        MIDDLEWARE_LOG_ERROR("Absolute path detected: %s", filename.c_str());
        return false;
    }

    // Check for reserved Windows names
    const std::vector<std::string> reserved = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    // Extract just the filename part (after last slash) for reserved name checking
    std::string filenameOnly = filename;
    size_t lastSlash = filename.find_last_of('/');
    if (lastSlash != std::string::npos) {
        filenameOnly = filename.substr(lastSlash + 1);
    }

    // Convert to uppercase for comparison
    std::string upperFilename = filenameOnly;
    std::transform(upperFilename.begin(), upperFilename.end(), upperFilename.begin(), ::toupper);

    // Remove extension for reserved name check
    size_t dotPos = upperFilename.find_last_of('.');
    std::string nameWithoutExt = (dotPos != std::string::npos) ? upperFilename.substr(0, dotPos) : upperFilename;

    for (const auto& reserved_name : reserved) {
        if (nameWithoutExt == reserved_name) {
            MIDDLEWARE_LOG_ERROR("Reserved filename detected: %s", filename.c_str());
            return false;
        }
    }

    // Check for control characters and other problematic characters
    for (char c : filename) {
        if (c >= 0 && c < 32 && c != '\t') { // Allow tab but reject other control chars
            MIDDLEWARE_LOG_ERROR("Control character detected in filename: %s", filename.c_str());
            return false;
        }
    }

    // Additional safety: reject filenames that are just dots
    if (filenameOnly == "." || filenameOnly == "..") {
        MIDDLEWARE_LOG_ERROR("Invalid dot filename: %s", filename.c_str());
        return false;
    }

    MIDDLEWARE_LOG_DEBUG("Filename validation passed: %s", filename.c_str());
    return true;
}


bool ZmqConnector::validateHashFormatPermissive(const std::string& hash) const {
    // More permissive hash validation - allow different hash lengths
    if (hash.empty() || hash.length() > 128) {
        return false;
    }

    // Check if all characters are valid hexadecimal
    return std::all_of(hash.begin(), hash.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c));
    });
}



bool ZmqConnector::receiveAnyMessage(int timeoutMs) {
    // Validate connection state
    if (connectionStatus.load() != ConnectionStatus::Connected || !zmqSocket) {
        MIDDLEWARE_LOG_ERROR("ZmqConnector not connected for message receive");
        messageStats.failedReceives.fetch_add(1);
        return false;
    }

    if (shutdownRequested.load()) {
        MIDDLEWARE_LOG_DEBUG("Shutdown requested, aborting message receive");
        return false;
    }

    try {
        // Check for available messages
        zmq::pollitem_t items[] = {{ zmqSocket->handle(), 0, ZMQ_POLLIN, 0 }};
        int pollResult = zmq::poll(items, 1, std::chrono::milliseconds(timeoutMs));

        if (pollResult == 0) {
            return false; // Timeout
        }

        if (pollResult < 0 || !(items[0].revents & ZMQ_POLLIN)) {
            MIDDLEWARE_LOG_ERROR("Poll error in receiveAnyMessage");
            messageStats.failedReceives.fetch_add(1);
            return false;
        }

        // Receive identity
        zmq::message_t identityMsg;
        if (!receiveMessagePart(identityMsg, timeoutMs, true)) {
            MIDDLEWARE_LOG_ERROR("Failed to receive identity in receiveAnyMessage");
            messageStats.failedReceives.fetch_add(1);
            return false;
        }

        // Validate identity
        if (identityMsg.size() == 0 || identityMsg.size() > 256) {
            MIDDLEWARE_LOG_ERROR("Invalid identity size in message: %zu", identityMsg.size());
            messageStats.failedReceives.fetch_add(1);
            return false;
        }

        std::string identityStr(static_cast<const char*>(identityMsg.data()), identityMsg.size());
        MIDDLEWARE_LOG_DEBUG("Receiving message from client: %s", identityStr.c_str());

        zmq::message_t clientIdentity = std::move(identityMsg);

        // Receive message content
        zmq::message_t contentMsg;
        if (!receiveMessagePart(contentMsg, timeoutMs, false)) {
            MIDDLEWARE_LOG_ERROR("Failed to receive message content");
            sendReply(clientIdentity, "ERROR: Missing content");
            messageStats.failedReceives.fetch_add(1);
            return false;
        }

        // Validate content size
        if (contentMsg.size() > maxMessageSize.load()) {
            MIDDLEWARE_LOG_ERROR("Message too large: %zu bytes (max: %zu)",
                                contentMsg.size(), maxMessageSize.load());
            sendReply(clientIdentity, "ERROR: Message too large");
            messageStats.failedReceives.fetch_add(1);
            return false;
        }

        // Check for multi-part file message (should be handled by receiveFile)
        if (zmqSocket->get(zmq::sockopt::rcvmore)) {
            MIDDLEWARE_LOG_DEBUG("Multi-part message detected, likely file transfer");
            drainRemainingParts();
            sendReply(clientIdentity, "RETRY_AS_FILE");
            return false;
        }

        // Process as simple message
        std::string messageContent = contentMsg.to_string();

        // Validate message content
        if (messageContent.empty() || messageContent.size() > safety::MAX_STRING_SIZE) {
            MIDDLEWARE_LOG_ERROR("Invalid message content size: %zu", messageContent.size());
            sendReply(clientIdentity, "ERROR: Invalid message");
            messageStats.failedReceives.fetch_add(1);
            return false;
        }

        // Store message safely
        {
            std::lock_guard<std::mutex> lock(messageMutex);
            lastReceivedMessage = messageContent;
        }

        MIDDLEWARE_LOG_DEBUG("Received message: %s", messageContent.c_str());

        // Send success reply
        if (!sendReply(clientIdentity, "{\"status\": \"ok\", \"message\": \"Message received\"}", 1000)) {
            MIDDLEWARE_LOG_WARNING("Failed to send message reply");
        }

        // Update statistics
        messageStats.totalMessagesReceived.fetch_add(1);
        messageStats.lastMessageTime = std::chrono::steady_clock::now();

        return true;

    } catch (const zmq::error_t& e) {
        if (e.num() == ETERM || e.num() == ENOTSOCK) {
            MIDDLEWARE_LOG_INFO("ZMQ context terminated during message receive");
            connectionStatus.store(ConnectionStatus::Disconnected);
        } else if (e.num() == EINTR) {
            MIDDLEWARE_LOG_DEBUG("ZMQ message receive interrupted");
        } else {
            MIDDLEWARE_LOG_ERROR("ZeroMQ error in receiveAnyMessage: %s (errno: %d)", e.what(), e.num());
        }
        messageStats.failedReceives.fetch_add(1);
        return false;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in receiveAnyMessage: %s", e.what());
        messageStats.failedReceives.fetch_add(1);
        return false;
    }
}

void ZmqConnector::disconnect(int gracefulTimeoutMs) {
    MIDDLEWARE_LOG_INFO("Disconnecting ZmqConnector (timeout: %dms)", gracefulTimeoutMs);

    shutdownRequested.store(true);
    connectionStatus.store(ConnectionStatus::ShuttingDown);

    std::lock_guard<std::mutex> lock(connectionMutex);

    try {
        // Close socket first
        if (zmqSocket) {
            // Try graceful unbind if we have an endpoint
            if (!currentEndpoint.empty()) {
                try {
                    zmqSocket->unbind(currentEndpoint);
                    MIDDLEWARE_LOG_DEBUG("Unbound from %s", currentEndpoint.c_str());
                } catch (const zmq::error_t& e) {
                    MIDDLEWARE_LOG_WARNING("Error during unbind: %s (errno: %d)", e.what(), e.num());
                }
            }

            // Set linger time for graceful shutdown
            try {
                zmqSocket->set(zmq::sockopt::linger, gracefulTimeoutMs);
            } catch (const zmq::error_t& e) {
                MIDDLEWARE_LOG_WARNING("Failed to set linger time: %s", e.what());
            }

            // Close socket
            try {
                zmqSocket->close();
                MIDDLEWARE_LOG_DEBUG("ZMQ socket closed");
            } catch (const zmq::error_t& e) {
                MIDDLEWARE_LOG_WARNING("Error closing socket: %s (errno: %d)", e.what(), e.num());
            }

            zmqSocket.reset();
        }

        // Close context
        if (zmqContext) {
            try {
                zmqContext->close();
                MIDDLEWARE_LOG_DEBUG("ZMQ context closed");
            } catch (const zmq::error_t& e) {
                MIDDLEWARE_LOG_WARNING("Error closing context: %s (errno: %d)", e.what(), e.num());
            }

            zmqContext.reset();
        }

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception during disconnect: %s", e.what());
    }

    // Clear state
    currentEndpoint.clear();
    {
        std::lock_guard<std::mutex> msgLock(messageMutex);
        lastReceivedMessage.clear();
    }

    connectionStatus.store(ConnectionStatus::Disconnected);
    MIDDLEWARE_LOG_INFO("ZmqConnector disconnected successfully");
}

// Getter methods with thread safety
bool ZmqConnector::isConnected() const {
    return connectionStatus.load() == ConnectionStatus::Connected &&
           zmqSocket != nullptr &&
           !shutdownRequested.load();
}

ZmqConnector::ConnectionStatus ZmqConnector::getConnectionStatus() const {
    return connectionStatus.load();
}

void* ZmqConnector::getSocket() const {
    std::lock_guard<std::mutex> lock(connectionMutex);
    return zmqSocket ? zmqSocket->handle() : nullptr;
}

std::string ZmqConnector::getLastReceivedMessage() const {
    std::lock_guard<std::mutex> lock(messageMutex);
    return lastReceivedMessage; // Return copy for thread safety
}

std::string ZmqConnector::getCurrentEndpoint() const {
    std::lock_guard<std::mutex> lock(connectionMutex);
    return currentEndpoint;
}


    ZmqConnector::MessageStats::Snapshot ZmqConnector::getMessageStats() const {
    return messageStats.getSnapshot(); // Return the copyable snapshot
}

void ZmqConnector::resetMessageStats() {
    messageStats.reset();
    MIDDLEWARE_LOG_INFO("Message statistics reset");
}

void ZmqConnector::setMaxMessageSize(size_t maxSizeBytes) {
    if (maxSizeBytes > 0 && maxSizeBytes <= safety::MAX_BUFFER_SIZE) {
        maxMessageSize.store(maxSizeBytes);
        MIDDLEWARE_LOG_INFO("Max message size set to %zu bytes", maxSizeBytes);
    } else {
        MIDDLEWARE_LOG_ERROR("Invalid max message size: %zu (must be 1-%zu)",
                            maxSizeBytes, safety::MAX_BUFFER_SIZE);
    }
}

size_t ZmqConnector::getMaxMessageSize() const {
    return maxMessageSize.load();
}

// Private helper methods

void ZmqConnector::cleanup() {
    try {
        if (zmqSocket) {
            zmqSocket->close();
            zmqSocket.reset();
        }
        if (zmqContext) {
            zmqContext->close();
            zmqContext.reset();
        }
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception during cleanup: %s", e.what());
    }

    currentEndpoint.clear();
    {
        std::lock_guard<std::mutex> lock(messageMutex);
        lastReceivedMessage.clear();
    }
}

bool ZmqConnector::sendReply(zmq::message_t& identity, const std::string& response, int timeoutMs) {
    if (!zmqSocket || shutdownRequested.load()) {
        return false;
    }

    try {
        // Validate response size
        if (response.size() > maxMessageSize.load()) {
            MIDDLEWARE_LOG_ERROR("Reply too large: %zu bytes", response.size());
            return false;
        }

        // Send identity frame
        auto sendIdRes = zmqSocket->send(identity, zmq::send_flags::sndmore);
        if (!sendIdRes) {
            MIDDLEWARE_LOG_ERROR("Failed to send identity in reply");
            return false;
        }

        // Send response frame
        zmq::message_t replyMsg(response.data(), response.size());
        auto sendReplyRes = zmqSocket->send(replyMsg, zmq::send_flags::none);
        if (!sendReplyRes) {
            MIDDLEWARE_LOG_ERROR("Failed to send reply message");
            return false;
        }

        MIDDLEWARE_LOG_DEBUG("Reply sent successfully: %s", response.c_str());
        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZMQ error in sendReply: %s (errno: %d)", e.what(), e.num());
        return false;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in sendReply: %s", e.what());
        return false;
    }
}


bool ZmqConnector::validateMessageContent(const std::vector<uint8_t>& content, size_t maxSize) const {
    if (content.empty() || content.size() > maxSize) {
        return false;
    }

    // Additional content validation can be added here
    // For now, just check size constraints
    return true;
}

bool ZmqConnector::tryAlternativeEndpoints(const std::string& primaryEndpoint) {
    std::vector<std::string> alternatives;

    // Extract port from primary endpoint
    size_t colonPos = primaryEndpoint.find_last_of(':');
    if (colonPos != std::string::npos) {
        std::string port = primaryEndpoint.substr(colonPos + 1);

        // Try localhost instead of wildcard
        if (primaryEndpoint.find("*") != std::string::npos) {
            alternatives.push_back("tcp://127.0.0.1:" + port);
            alternatives.push_back("tcp://localhost:" + port);
        }

        // Try different ports
        int basePort = std::stoi(port);
        for (int offset : {1, -1, 1000, -1000}) {
            int newPort = basePort + offset;
            if (newPort > 1024 && newPort < 65536) {
                alternatives.push_back("tcp://*:" + std::to_string(newPort));
            }
        }
    }

    // Try each alternative
    for (const auto& alt : alternatives) {
        try {
            MIDDLEWARE_LOG_INFO("Trying alternative endpoint: %s", alt.c_str());
            zmqSocket->bind(alt);
            currentEndpoint = alt;
            MIDDLEWARE_LOG_INFO("Successfully bound to alternative endpoint: %s", alt.c_str());
            return true;

        } catch (const zmq::error_t& e) {
            MIDDLEWARE_LOG_DEBUG("Alternative endpoint %s failed: %s", alt.c_str(), e.what());
            continue;
        }
    }

    return false;
}

int ZmqConnector::drainRemainingParts() {
    int drainedCount = 0;

    try {
        zmq::message_t drainMsg;
        while (zmqSocket && zmqSocket->get(zmq::sockopt::rcvmore)) {
            if (zmqSocket->recv(drainMsg, zmq::recv_flags::dontwait)) {
                drainedCount++;
                if (drainedCount > 10) { // Prevent infinite loop
                    MIDDLEWARE_LOG_ERROR("Too many message parts to drain, stopping");
                    break;
                }
            } else {
                break;
            }
        }
    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_WARNING("Error draining message parts: %s", e.what());
    }

    return drainedCount;
}

bool ZmqConnector::receiveMessagePart(zmq::message_t& message, int timeoutMs, bool expectMore) {
    if (!zmqSocket) {
        return false;
    }

    try {
        auto result = zmqSocket->recv(message, zmq::recv_flags::none);
        if (!result || result.value() == 0) {
            return false;
        }

        // Check if more parts are expected
        bool hasMore = zmqSocket->get(zmq::sockopt::rcvmore);
        if (expectMore && !hasMore) {
            MIDDLEWARE_LOG_ERROR("Expected more message parts but none available");
            return false;
        }

        if (!expectMore && hasMore) {
            MIDDLEWARE_LOG_WARNING("Unexpected additional message parts available");
        }

        return true;

    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("Error receiving message part: %s (errno: %d)", e.what(), e.num());
        return false;
    }
}

bool ZmqConnector::validateHashFormat(const std::string& hash) const {
    // SHA256 hash should be exactly 64 hexadecimal characters
    if (hash.length() != 64) {
        return false;
    }

    // Check if all characters are valid hexadecimal
    return std::all_of(hash.begin(), hash.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c));
    });
}

bool ZmqConnector::testConnection() {
    if (!isConnected()) {
        return false;
    }

    try {
        // Simple connection health check
        updateHealthStatus();
        return connectionStatus.load() == ConnectionStatus::Connected;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Connection test failed: %s", e.what());
        return false;
    }
}

void ZmqConnector::updateHealthStatus() {
    auto now = std::chrono::steady_clock::now();
    lastHealthCheck = now;

    // Additional health checks can be implemented here
    // For now, just update the timestamp
}

} // namespace anari_usd_middleware
