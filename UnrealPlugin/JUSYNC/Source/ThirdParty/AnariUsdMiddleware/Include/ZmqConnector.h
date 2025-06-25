#pragma once

#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <zmq.hpp>
#include "MiddlewareLogging.h"

namespace anari_usd_middleware {

/**
 * Thread-safe ZeroMQ communication handler for the ANARI-USD middleware with comprehensive
 * error handling and memory safety features for Unreal Engine 5.5 compatibility.
 * Implements a ROUTER socket pattern for receiving data from DEALER clients.
 */
class ZmqConnector {
public:
    /**
     * Connection status enumeration for detailed state tracking
     */
    enum class ConnectionStatus {
        Disconnected,
        Connecting,
        Connected,
        Error,
        ShuttingDown
    };

    /**
     * Message statistics for monitoring and debugging - FIXED VERSION
     */
    struct MessageStats {
        std::atomic<uint64_t> totalMessagesReceived{0};
        std::atomic<uint64_t> totalFilesReceived{0};
        std::atomic<uint64_t> totalBytesReceived{0};
        std::atomic<uint64_t> failedReceives{0};
        std::atomic<uint64_t> hashMismatches{0};
        std::chrono::steady_clock::time_point lastMessageTime;

        // Delete copy constructor and assignment operator since atomics can't be copied
        MessageStats() = default;
        MessageStats(const MessageStats&) = delete;
        MessageStats& operator=(const MessageStats&) = delete;

        // Move constructor and assignment
        MessageStats(MessageStats&& other) noexcept {
            totalMessagesReceived.store(other.totalMessagesReceived.load());
            totalFilesReceived.store(other.totalFilesReceived.load());
            totalBytesReceived.store(other.totalBytesReceived.load());
            failedReceives.store(other.failedReceives.load());
            hashMismatches.store(other.hashMismatches.load());
            lastMessageTime = other.lastMessageTime;
        }

        MessageStats& operator=(MessageStats&& other) noexcept {
            if (this != &other) {
                totalMessagesReceived.store(other.totalMessagesReceived.load());
                totalFilesReceived.store(other.totalFilesReceived.load());
                totalBytesReceived.store(other.totalBytesReceived.load());
                failedReceives.store(other.failedReceives.load());
                hashMismatches.store(other.hashMismatches.load());
                lastMessageTime = other.lastMessageTime;
            }
            return *this;
        }

        void reset() {
            totalMessagesReceived.store(0);
            totalFilesReceived.store(0);
            totalBytesReceived.store(0);
            failedReceives.store(0);
            hashMismatches.store(0);
            lastMessageTime = std::chrono::steady_clock::now();
        }

        // Create a copyable snapshot for returning from functions
        struct Snapshot {
            uint64_t totalMessagesReceived;
            uint64_t totalFilesReceived;
            uint64_t totalBytesReceived;
            uint64_t failedReceives;
            uint64_t hashMismatches;
            std::chrono::steady_clock::time_point lastMessageTime;
        };

        Snapshot getSnapshot() const {
            return {
                totalMessagesReceived.load(),
                totalFilesReceived.load(),
                totalBytesReceived.load(),
                failedReceives.load(),
                hashMismatches.load(),
                lastMessageTime
            };
        }
    };

    ZmqConnector();
    ~ZmqConnector();

    // Disable copy constructor and assignment operator for safety
    ZmqConnector(const ZmqConnector&) = delete;
    ZmqConnector& operator=(const ZmqConnector&) = delete;

    /**
     * Initializes the ZeroMQ connection with enhanced error handling and fallback endpoints.
     * @param endpoint The ZeroMQ endpoint to bind to (e.g., "tcp://*:13456").
     *                 If null, defaults to "tcp://*:5556".
     * @param timeoutMs Connection timeout in milliseconds (default: 5000ms)
     * @return True if initialization was successful, false otherwise.
     */
    bool initialize(const char* endpoint = nullptr, int timeoutMs = 5000);

    /**
     * Disconnects and cleans up the ZeroMQ connection safely.
     * @param gracefulTimeoutMs Time to wait for graceful shutdown (default: 1000ms)
     */
    void disconnect(int gracefulTimeoutMs = 1000);

    /**
     * Checks if the ZeroMQ connection is active and healthy.
     * @return True if connected and operational, false otherwise.
     */
    bool isConnected() const;

    /**
     * Get detailed connection status
     * @return Current connection status
     */
    ConnectionStatus getConnectionStatus() const;

    /**
     * Receives a file from a client with comprehensive validation.
     * Expects a multi-part message: [Identity] [Filename] [Content] [Hash]
     * @param filename Output parameter for the received filename (validated for safety)
     * @param data Output parameter for the received file data (size-limited)
     * @param hash Output parameter for the received file hash (format-validated)
     * @param timeoutMs Receive timeout in milliseconds (default: 100ms)
     * @return True if a file was successfully received and validated, false otherwise.
     */
    bool receiveFile(std::string& filename, std::vector<uint8_t>& data,
                     std::string& hash, int timeoutMs = 100);

    bool validateFilenamePermissive(const std::string &filename) const;

    bool validateHashFormatPermissive(const std::string &hash) const;

    /**
     * Receives any message format and handles simple test messages with validation.
     * @param timeoutMs Receive timeout in milliseconds (default: 100ms)
     * @return True if a message was successfully processed and validated.
     */
    bool receiveAnyMessage(int timeoutMs = 100);

    /**
     * Gets the raw socket handle for polling operations (thread-safe).
     * @return The raw ZeroMQ socket handle, or nullptr if not connected.
     */
    void* getSocket() const;

    /**
     * Gets the last received message as a string (thread-safe).
     * @return The last received message (copy for thread safety).
     */
    std::string getLastReceivedMessage() const;

    /**
     * Get current endpoint information
     * @return Current bound endpoint string
     */
    std::string getCurrentEndpoint() const;

    /**
     * Get message statistics for monitoring - FIXED VERSION
     * @return Snapshot of current message statistics (copyable)
     */
    MessageStats::Snapshot getMessageStats() const;

    /**
     * Reset message statistics
     */
    void resetMessageStats();

    /**
     * Test connection health by sending a ping
     * @return True if connection is healthy, false otherwise
     */
    bool testConnection();

    /**
     * Set maximum message size limit for safety
     * @param maxSizeBytes Maximum allowed message size in bytes
     */
    void setMaxMessageSize(size_t maxSizeBytes);

    /**
     * Get maximum message size limit
     * @return Current maximum message size in bytes
     */
    size_t getMaxMessageSize() const;

private:
    // ZeroMQ components with RAII wrappers
    std::unique_ptr<zmq::context_t> zmqContext;
    std::unique_ptr<zmq::socket_t> zmqSocket;

    // Connection state management
    std::string currentEndpoint;
    mutable std::mutex connectionMutex;
    std::atomic<ConnectionStatus> connectionStatus{ConnectionStatus::Disconnected};
    std::atomic<bool> shutdownRequested{false};

    // Message handling
    std::string lastReceivedMessage;
    mutable std::mutex messageMutex;
    MessageStats messageStats;
    std::atomic<size_t> maxMessageSize{safety::MAX_BUFFER_SIZE};

    // Timing and health monitoring
    std::chrono::steady_clock::time_point lastHealthCheck;
    std::atomic<bool> healthCheckEnabled{true};

    /**
     * Helper method for safe cleanup during initialization errors.
     */
    void cleanup();

    /**
     * Helper method to send a reply to a client with validation.
     * @param identity The client's identity frame (validated)
     * @param response The response message to send (size-limited)
     * @param timeoutMs Send timeout in milliseconds
     * @return True if the reply was sent successfully, false otherwise.
     */
    bool sendReply(zmq::message_t& identity, const std::string& response, int timeoutMs = 1000);

    /**
     * Validate incoming filename for security
     * @param filename Filename to validate
     * @return True if filename is safe, false otherwise
     */
    bool validateFilename(const std::string& filename) const;

    /**
     * Validate message content for safety
     * @param content Message content to validate
     * @param maxSize Maximum allowed size
     * @return True if content is safe, false otherwise
     */
    bool validateMessageContent(const std::vector<uint8_t>& content, size_t maxSize) const;

    /**
     * Try binding to alternative endpoints if primary fails
     * @param primaryEndpoint Primary endpoint that failed
     * @return True if successfully bound to alternative, false otherwise
     */
    bool tryAlternativeEndpoints(const std::string& primaryEndpoint);

    /**
     * Drain any remaining message parts to prevent state corruption
     * @return Number of parts drained
     */
    int drainRemainingParts();

    /**
     * Update connection health status
     */
    void updateHealthStatus();

    /**
     * Safe message part reception with timeout and validation
     * @param message Output message
     * @param timeoutMs Timeout in milliseconds
     * @param expectMore Whether to expect more parts
     * @return True if received successfully, false otherwise
     */
    bool receiveMessagePart(zmq::message_t& message, int timeoutMs, bool expectMore = false);

    /**
     * Validate hash format and content
     * @param hash Hash string to validate
     * @return True if hash is valid format, false otherwise
     */
    bool validateHashFormat(const std::string& hash) const;
};

} // namespace anari_usd_middleware
