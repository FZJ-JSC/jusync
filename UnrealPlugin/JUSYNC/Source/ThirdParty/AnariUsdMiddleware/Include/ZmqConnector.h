#pragma once

#include <zmq.hpp>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <map>

// Platform detection
#if defined(_WIN32)
    #define PLATFORM_WINDOWS 1
    #define PLATFORM_LINUX 0
#elif defined(__linux__)
    #define PLATFORM_WINDOWS 0
    #define PLATFORM_LINUX 1
#else
    #define PLATFORM_WINDOWS 0
    #define PLATFORM_LINUX 0
#endif

// Safety constants
namespace safety {
    constexpr size_t MAX_BUFFER_SIZE = 100 * 1024 * 1024; // 100MB
    constexpr size_t MAX_STRING_SIZE = 10 * 1024 * 1024;  // 10MB
    constexpr double EPSILON = 1e-6;
}

namespace anari_usd_middleware {

/**
 * Enhanced cross-platform ZeroMQ connector with comprehensive safety features
 * Supports both Windows and Linux platforms with optimized configurations
 */
class ZmqConnector {
public:
    // Connection status enumeration
    enum class ConnectionStatus {
        Disconnected,
        Connecting,
        Connected,
        ShuttingDown,
        Error
    };

    // Message statistics structure with thread-safe snapshot capability
    struct MessageStats {
        std::atomic<uint64_t> totalMessagesReceived{0};
        std::atomic<uint64_t> totalFilesReceived{0};
        std::atomic<uint64_t> totalBytesReceived{0};
        std::atomic<uint64_t> failedReceives{0};
        std::chrono::steady_clock::time_point lastMessageTime;

        // Thread-safe copyable snapshot
        struct Snapshot {
            uint64_t totalMessagesReceived;
            uint64_t totalFilesReceived;
            uint64_t totalBytesReceived;
            uint64_t failedReceives;
            std::chrono::steady_clock::time_point lastMessageTime;
        };

        Snapshot getSnapshot() const {
            return {
                totalMessagesReceived.load(),
                totalFilesReceived.load(),
                totalBytesReceived.load(),
                failedReceives.load(),
                lastMessageTime
            };
        }

        void reset() {
            totalMessagesReceived.store(0);
            totalFilesReceived.store(0);
            totalBytesReceived.store(0);
            failedReceives.store(0);
            lastMessageTime = std::chrono::steady_clock::now();
        }
    };

public:
    // Constructor and destructor
    ZmqConnector();
    virtual ~ZmqConnector();

    // Disable copy constructor and assignment operator for safety
    ZmqConnector(const ZmqConnector&) = delete;
    ZmqConnector& operator=(const ZmqConnector&) = delete;

    // Enable move constructor and assignment operator
    ZmqConnector(ZmqConnector&&) = default;
    ZmqConnector& operator=(ZmqConnector&&) = default;

    // Core connection management
    bool initialize(const char* endpoint, int timeoutMs = 5000);
    void disconnect(int gracefulTimeoutMs = 1000);
    bool isConnected() const;
    ConnectionStatus getConnectionStatus() const;

    // Message handling
    bool receiveFile(std::string& filename, std::vector<uint8_t>& data,
                     std::string& hash, int timeoutMs = 5000);
    bool receiveAnyMessage(int timeoutMs = 1000);

    // Getters with thread safety
    void* getSocket() const;
    std::string getLastReceivedMessage() const;
    std::string getCurrentEndpoint() const;

    // Statistics and monitoring
    MessageStats::Snapshot getMessageStats() const;
    void resetMessageStats();
    void setMaxMessageSize(size_t maxSizeBytes);
    size_t getMaxMessageSize() const;

    // Connection testing and health monitoring
    bool testConnection();
    void updateHealthStatus();

private:
    // Cross-platform initialization methods
    bool configurePlatformSpecificSocket(int timeoutMs);
    std::string getDefaultEndpoint() const;

#if PLATFORM_WINDOWS
    bool configureWindowsSpecific();
    bool validateWindowsReservedNames(const std::string& filename) const;
#endif

#if PLATFORM_LINUX
    bool configureLinuxSpecific();
#endif

    // Enhanced validation methods
    bool validateEndpoint(const std::string& endpoint) const;
    bool validateTcpEndpoint(const std::string& endpoint) const;
    bool validateIpcEndpoint(const std::string& endpoint) const;
    bool validateInprocEndpoint(const std::string& endpoint) const;
    bool validateFilename(const std::string& filename) const;
    bool validateHashFormatPermissive(const std::string& hash) const;

    // Message handling helpers
    bool receiveMessagePart(zmq::message_t& message, int timeoutMs, bool expectMore);
    int drainRemainingParts();
    bool sendReply(zmq::message_t& identity, const std::string& response, int timeoutMs = 1000);

    // Connection management helpers
    bool tryAlternativeEndpoints(const std::string& primaryEndpoint);
    void cleanup();

    // Member variables
    std::unique_ptr<zmq::context_t> zmqContext;
    std::unique_ptr<zmq::socket_t> zmqSocket;

    // Connection state
    std::atomic<ConnectionStatus> connectionStatus{ConnectionStatus::Disconnected};
    std::atomic<bool> shutdownRequested{false};
    std::string currentEndpoint;

    // Thread safety
    mutable std::mutex connectionMutex;
    mutable std::mutex messageMutex;

    // Message storage
    std::string lastReceivedMessage;

    // Statistics and monitoring
    MessageStats messageStats;
    std::atomic<size_t> maxMessageSize{10485760}; // 10MB default
    std::chrono::steady_clock::time_point lastHealthCheck;

    // Cross-platform library handles (if needed for future extensions)
    std::map<std::string, void*> loadedLibraryHandles;
};

} // namespace anari_usd_middleware
