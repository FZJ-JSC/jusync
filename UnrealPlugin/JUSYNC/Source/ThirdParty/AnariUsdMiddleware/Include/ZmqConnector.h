#pragma once

#include <zmq.hpp>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <limits>

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

namespace safety {
    constexpr size_t MAX_BUFFER_SIZE = static_cast<size_t>(std::numeric_limits<int64_t>::max() / 2); // Essentially unlimited (4.6EB)
    constexpr size_t MAX_STRING_SIZE = 10 * 1024 * 1024;  // 10MB
    constexpr double EPSILON = 1e-6;
}

namespace anari_usd_middleware {

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

    bool initialize(const char* endpoint, int timeoutMs = 5000);
    void disconnect(int gracefulTimeoutMs = 1000);

    void setMaxMessageSize(size_t maxSizeBytes);

private:
    std::string getDefaultEndpoint() const;
    bool validateEndpoint(const std::string& endpoint) const;
    bool validateTcpEndpoint(const std::string& endpoint) const;
    bool validateIpcEndpoint(const std::string& endpoint) const;
    bool validateInprocEndpoint(const std::string& endpoint) const;
    void cleanup();

    std::atomic<ConnectionStatus> connectionStatus{ConnectionStatus::Disconnected};
    std::atomic<bool> shutdownRequested{false};
    std::string currentEndpoint;

    mutable std::mutex connectionMutex;

    std::unique_ptr<zmq::context_t> zmqContext;
    std::unique_ptr<zmq::socket_t> zmqSocket;

    std::atomic<size_t> maxMessageSize{static_cast<size_t>(std::numeric_limits<int64_t>::max() / 2)}; // Essentially unlimited
};

} // namespace anari_usd_middleware
