#pragma once

#include <zmq.hpp>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <limits>

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
    constexpr size_t MAX_BUFFER_SIZE = static_cast<size_t>(std::numeric_limits<int64_t>::max() / 2); // Essentially unlimited (4.6EB)
    constexpr size_t MAX_STRING_SIZE = 10 * 1024 * 1024;  // 10MB
    constexpr double EPSILON = 1e-6;
}

namespace anari_usd_middleware {

/**
 * ZeroMQ endpoint validator / lifecycle tracker (DEALER-only mode).
 *
 * In DEALER-only mode no ZMQ socket is created here — all communication is
 * handled by AnariUsdClient (DEALER socket). This class validates the
 * endpoint string and tracks connection lifecycle for the middleware.
 */
class ZmqConnector {
public:
    enum class ConnectionStatus {
        Disconnected,
        Connecting,
        Connected,
        ShuttingDown,
        Error
    };

    ZmqConnector();
    virtual ~ZmqConnector();

    ZmqConnector(const ZmqConnector&) = delete;
    ZmqConnector& operator=(const ZmqConnector&) = delete;

    ZmqConnector(ZmqConnector&&) = default;
    ZmqConnector& operator=(ZmqConnector&&) = default;

    // Core connection management
    bool initialize(const char* endpoint, int timeoutMs = 5000);
    void disconnect(int gracefulTimeoutMs = 1000);

    // Configuration
    void setMaxMessageSize(size_t maxSizeBytes);

private:
    // Endpoint helpers
    std::string getDefaultEndpoint() const;
    bool validateEndpoint(const std::string& endpoint) const;
    bool validateTcpEndpoint(const std::string& endpoint) const;
    bool validateIpcEndpoint(const std::string& endpoint) const;
    bool validateInprocEndpoint(const std::string& endpoint) const;
    void cleanup();

    // Connection state
    std::atomic<ConnectionStatus> connectionStatus{ConnectionStatus::Disconnected};
    std::atomic<bool> shutdownRequested{false};
    std::string currentEndpoint;

    // Thread safety
    mutable std::mutex connectionMutex;

    // ZMQ objects (never created in DEALER-only mode; kept for cleanup symmetry)
    std::unique_ptr<zmq::context_t> zmqContext;
    std::unique_ptr<zmq::socket_t> zmqSocket;

    // Configuration
    std::atomic<size_t> maxMessageSize{static_cast<size_t>(std::numeric_limits<int64_t>::max() / 2)}; // Essentially unlimited
};

} // namespace anari_usd_middleware
