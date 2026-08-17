#include "ZmqConnector.h"
#include "MiddlewareLogging.h"

#include <regex>
#include <filesystem>
#include <stdexcept>

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

// Platform-specific includes
#if PLATFORM_WINDOWS
    #include <winsock2.h>
    #include <windows.h>
#elif PLATFORM_LINUX
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <unistd.h>
#endif

namespace anari_usd_middleware {

ZmqConnector::ZmqConnector() {
    MIDDLEWARE_LOG_INFO("ZmqConnector created with enhanced cross-platform safety features");
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

    MIDDLEWARE_LOG_INFO("Initializing ZmqConnector with cross-platform support (timeout %dms)", timeoutMs);
    connectionStatus.store(ConnectionStatus::Connecting);

    try {
        // Validate timeout
        if (timeoutMs <= 0 || timeoutMs > 30000) {
            MIDDLEWARE_LOG_ERROR("Invalid timeout value: %d (must be 1-30000ms)", timeoutMs);
            connectionStatus.store(ConnectionStatus::Error);
            return false;
        }

        // DEALER-ONLY ARCHITECTURE: No ZMQ context or ROUTER socket is created here.
        // All communication is handled by AnariUsdClient (DEALER socket).
        // ZmqConnector in this mode is a stub for endpoint validation and health tracking.

        // Determine and validate endpoint (but don't bind)
        currentEndpoint = endpoint ? endpoint : getDefaultEndpoint();

        if (!validateEndpoint(currentEndpoint)) {
            MIDDLEWARE_LOG_ERROR("Invalid endpoint format: %s", currentEndpoint.c_str());
            cleanup();
            connectionStatus.store(ConnectionStatus::Error);
            return false;
        }

        MIDDLEWARE_LOG_INFO("ZmqConnector initialized in DEALER-only stub mode");

        // In stub mode, socket is null. This is intentional - use AnariUsdClient
        // for actual communication.
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

void ZmqConnector::disconnect(int gracefulTimeoutMs) {
    MIDDLEWARE_LOG_INFO("Disconnecting ZmqConnector with cross-platform cleanup (timeout: %dms)", gracefulTimeoutMs);
    shutdownRequested.store(true);
    connectionStatus.store(ConnectionStatus::ShuttingDown);

    std::lock_guard<std::mutex> lock(connectionMutex);

    try {
        // Close socket first
        if (zmqSocket) {
            if (!currentEndpoint.empty()) {
                try {
                    zmqSocket->unbind(currentEndpoint);
                    MIDDLEWARE_LOG_DEBUG("Unbound from %s", currentEndpoint.c_str());
                } catch (const zmq::error_t& e) {
                    MIDDLEWARE_LOG_WARNING("Error during unbind: %s (errno: %d)", e.what(), e.num());
                }
            }
            zmqSocket->close();
            zmqSocket.reset();
        }

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

    currentEndpoint.clear();
    connectionStatus.store(ConnectionStatus::Disconnected);
    MIDDLEWARE_LOG_INFO("ZmqConnector disconnected successfully");
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

std::string ZmqConnector::getDefaultEndpoint() const {
    return "tcp://*:5556";
}

bool ZmqConnector::validateEndpoint(const std::string& endpoint) const {
    if (endpoint.empty()) {
        return false;
    }

    // Basic protocol validation
    if (endpoint.find("tcp://") == 0) {
        return validateTcpEndpoint(endpoint);
    } else if (endpoint.find("ipc://") == 0) {
        return validateIpcEndpoint(endpoint);
    } else if (endpoint.find("inproc://") == 0) {
        return validateInprocEndpoint(endpoint);
    }

    return false;
}

bool ZmqConnector::validateTcpEndpoint(const std::string& endpoint) const {
    // TCP endpoint validation: tcp://host:port
    static const std::regex tcpPattern(R"(^tcp://([^:]+|\*):(\d+)$)");
    std::smatch matches;

    if (!std::regex_match(endpoint, matches, tcpPattern)) {
        return false;
    }

    // Validate port range
    int port = std::stoi(matches[2]);
    return (port > 0 && port <= 65535);
}

bool ZmqConnector::validateIpcEndpoint(const std::string& endpoint) const {
#if PLATFORM_WINDOWS
    // Windows named pipes
    std::string path = endpoint.substr(6);  // Remove "ipc://"
    return path.find("//./pipe/") == 0 || path.find("\\\\.\\pipe\\") == 0;
#elif PLATFORM_LINUX
    // Unix domain sockets
    std::string path = endpoint.substr(6);  // Remove "ipc://"
    return !path.empty() && path[0] == '/' && path.find("..") == std::string::npos;
#else
    return false;
#endif
}

bool ZmqConnector::validateInprocEndpoint(const std::string& endpoint) const {
    // In-process endpoint validation: inproc://name
    std::string name = endpoint.substr(9);  // Remove "inproc://"
    return !name.empty() && name.find_first_of(":/\\") == std::string::npos;
}

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
}

} // namespace anari_usd_middleware
