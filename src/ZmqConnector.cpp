#include "ZmqConnector.h"
#include "MiddlewareLogging.h"

#include <cstring>
#include <stdexcept>

namespace anari_usd_middleware {

ZmqConnector::ZmqConnector() : connected(false) {
    // Constructor initializes connected flag to false
    lastReceivedMessage.clear();
}

ZmqConnector::~ZmqConnector() {
    // Ensure proper cleanup in destructor
    disconnect();
}

void* ZmqConnector::getSocket() const {
    // Return raw socket handle for polling operations
    return zmqSocket ? zmqSocket->handle() : nullptr;
}

const std::string& ZmqConnector::getLastReceivedMessage() const {
    return lastReceivedMessage;
}

bool ZmqConnector::initialize(const char* endpoint) {
    if (connected) {
        MIDDLEWARE_LOG_WARNING("ZmqConnector already initialized");
        return true; // Already initialized, consider it success
    }

    try {
        // 1. Initialize ZMQ context
        zmqContext = std::make_unique<zmq::context_t>(1); // 1 I/O thread

        // 2. Create ROUTER socket
        zmqSocket = std::make_unique<zmq::socket_t>(*zmqContext, zmq::socket_type::router);

        // 3. Set socket options
        zmqSocket->set(zmq::sockopt::linger, 0);

        // 4. Try binding to the endpoint
        currentEndpoint = endpoint ? endpoint : "tcp://*:5556"; // Default if null

        try {
            zmqSocket->bind(currentEndpoint);
            MIDDLEWARE_LOG_INFO("ZMQ Router bound successfully to %s", currentEndpoint.c_str());
        } catch (const zmq::error_t& e) {
            // If binding fails, try alternative endpoints
            MIDDLEWARE_LOG_WARNING("Failed to bind to %s: %s (errno: %d)",
                currentEndpoint.c_str(), e.what(), e.num());

            // Try specific IPv4 address if wildcard fails
            if (currentEndpoint.find("*") != std::string::npos) {
                std::string altEndpoint = "tcp://127.0.0.1:" +
                    currentEndpoint.substr(currentEndpoint.find_last_of(":") + 1);

                try {
                    zmqSocket->bind(altEndpoint);
                    currentEndpoint = altEndpoint;
                    MIDDLEWARE_LOG_INFO("ZMQ Router bound successfully to %s", currentEndpoint.c_str());
                } catch (const zmq::error_t& e2) {
                    // Try a different port if both previous attempts failed
                    std::string diffPortEndpoint = "tcp://*:15555";
                    try {
                        zmqSocket->bind(diffPortEndpoint);
                        currentEndpoint = diffPortEndpoint;
                        MIDDLEWARE_LOG_INFO("ZMQ Router bound successfully to %s", currentEndpoint.c_str());
                    } catch (const zmq::error_t& e3) {
                        // All binding attempts failed
                        throw;
                    }
                }
            } else {
                // Re-throw if it's not a wildcard endpoint
                throw;
            }
        }

        connected = true;
        return true;
    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error during initialization: %s (errno: %d)",
            e.what(), e.num());
        cleanup();
        return false;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Standard exception during initialization: %s", e.what());
        // Ensure cleanup
        cleanup();
        return false;
    }
}

bool ZmqConnector::receiveAnyMessage() {
  if (!connected || !zmqSocket) {
    MIDDLEWARE_LOG_ERROR("ZmqConnector not connected or socket invalid");
    return false;
  }

  try {
    // Check if there's a message available without blocking
    zmq::pollitem_t items[] = {
      { zmqSocket->handle(), 0, ZMQ_POLLIN, 0 }
    };

    // Poll with a short timeout
    zmq::poll(items, 1, 10);

    if (!(items[0].revents & ZMQ_POLLIN)) {
      return false; // No message available
    }

    // Receive the client's identity
    zmq::message_t identityMsg;
    auto identityRes = zmqSocket->recv(identityMsg, zmq::recv_flags::none);
    if (!identityRes) {
      return false;
    }

    // Store client identity for reply
    zmq::message_t clientIdentity = std::move(identityMsg);
    std::string identityStr = std::string(static_cast<char*>(clientIdentity.data()), clientIdentity.size());
    MIDDLEWARE_LOG_INFO("Received message from client with identity: %s", identityStr.c_str());

    // Check if there are more parts
    if (!zmqSocket->get(zmq::sockopt::rcvmore)) {
      MIDDLEWARE_LOG_WARNING("Received identity but no message content");
      return false;
    }

    // Receive the second part (could be filename or message)
    zmq::message_t secondPartMsg;
    auto secondPartRes = zmqSocket->recv(secondPartMsg, zmq::recv_flags::none);
    if (!secondPartRes) {
      MIDDLEWARE_LOG_ERROR("Failed to receive second message part");
      return false;
    }

    // Check if there are more parts - for the anari-usd via zmq buffer with name, hash and filecontents in our case
    if (zmqSocket->get(zmq::sockopt::rcvmore)) {

      MIDDLEWARE_LOG_INFO("Multi-part message detected, likely a file transfer");

      zmq::message_t drainMsg;
      while (zmqSocket->get(zmq::sockopt::rcvmore)) {
        zmqSocket->recv(drainMsg, zmq::recv_flags::none);
      }

      // Send a reply to avoid leaving the client hanging
      sendReply(clientIdentity, "RETRY");

      return false;
    }

    // This is a simple message with just identity + content
    std::string messageContent = secondPartMsg.to_string();
    MIDDLEWARE_LOG_INFO("Received simple message: %s", messageContent.c_str());

    // Store the message for later retrieval
    lastReceivedMessage = messageContent;

    // Display the message for debugging
    MIDDLEWARE_LOG_INFO("ZMQ Message Received: %s", lastReceivedMessage.c_str());

    // Try to parse as JSON
    try {
      // Add your JSON parsing logic here if needed
      // For now, just log that we received a message
      MIDDLEWARE_LOG_INFO("Successfully processed message");
    } catch (...) {
      MIDDLEWARE_LOG_ERROR("Failed to parse JSON message: %s", lastReceivedMessage.c_str());
    }

    // Send reply back to client
    return sendReply(clientIdentity, "{\"status\": \"ok\", \"message\": \"Received your message\"}");
  }
  catch (const zmq::error_t& e) {
    MIDDLEWARE_LOG_ERROR("ZeroMQ error in receiveAnyMessage: %s (errno: %d)",
      e.what(), e.num());
    return false;
  }
  catch (const std::exception& e) {
    MIDDLEWARE_LOG_ERROR("Exception in receiveAnyMessage: %s", e.what());
    return false;
  }
}


bool ZmqConnector::receiveFile(std::string& filename, std::vector<uint8_t>& data, std::string& hash) {
  if (!connected || !zmqSocket) {
    MIDDLEWARE_LOG_ERROR("ZmqConnector not connected or socket invalid");
    return false;
  }

  try {
    // Check if there's a message available without blocking
    zmq::pollitem_t items[] = {
      { zmqSocket->handle(), 0, ZMQ_POLLIN, 0 }
    };

    // Poll with a short timeout
    zmq::poll(items, 1, 10);

    if (!(items[0].revents & ZMQ_POLLIN)) {
      return false; // No message available
    }

    // *** ROUTER RECEIVE LOGIC ***
    // Expecting multi-part message: [Identity] [Filename] [Content] [Hash]

    // Part 1: Receive the client's identity (added automatically by ZMQ)
    zmq::message_t identityMsg;
    auto identityRes = zmqSocket->recv(identityMsg, zmq::recv_flags::none);
    if (!identityRes || identityRes.value() == 0) {
      // to indicate interruption or socket closed
      return false;
    }

    std::string identityStr = std::string(static_cast<char*>(identityMsg.data()), identityMsg.size());
    MIDDLEWARE_LOG_INFO("Received file transfer from client with identity: %s", identityStr.c_str());

    // Check if more parts are coming (mandatory for Router)
    if (!zmqSocket->get(zmq::sockopt::rcvmore)) {
      MIDDLEWARE_LOG_ERROR("Received identity but no more message parts follow");
      return false;
    }

    // Store identity for sending the reply later
    zmq::message_t clientIdentity = std::move(identityMsg);

    // Part 2: Receive filename
    zmq::message_t filenameMsg;
    auto fileRes = zmqSocket->recv(filenameMsg, zmq::recv_flags::none);
    if (!fileRes || fileRes.value() == 0) {
      MIDDLEWARE_LOG_ERROR("Failed to receive filename after identity");
      return false;
    }

    if (!zmqSocket->get(zmq::sockopt::rcvmore)) {
      MIDDLEWARE_LOG_ERROR("Received filename but no more message parts (content expected)");
      return false;
    }

    filename = filenameMsg.to_string();
    MIDDLEWARE_LOG_INFO("Received filename: %s", filename.c_str());

    // Determine file type based on extension
    std::string fileType = "UNKNOWN";
    size_t dotPos = filename.find_last_of('.');
    if (dotPos != std::string::npos) {
      std::string extension = filename.substr(dotPos);
      if (extension == ".usda" || extension == ".usd") {
        fileType = "USD";
      } else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg") {
        fileType = "IMAGE";
      }
    }
    MIDDLEWARE_LOG_INFO("File type detected: %s", fileType.c_str());

    // Part 3: Receive content
    zmq::message_t contentMsg;
    auto contentRes = zmqSocket->recv(contentMsg, zmq::recv_flags::none);
    if (!contentRes || contentRes.value() == 0) {
      MIDDLEWARE_LOG_ERROR("Failed to receive content");
      return false;
    }

    if (!zmqSocket->get(zmq::sockopt::rcvmore)) {
      MIDDLEWARE_LOG_ERROR("Received content but no more message parts (hash expected)");
      return false;
    }

    // Copy data using uint8_t pointer - use efficient move if possible or assign
    const uint8_t* dataPtr = static_cast<const uint8_t*>(contentMsg.data());
    data.assign(dataPtr, dataPtr + contentMsg.size());
    MIDDLEWARE_LOG_INFO("Received file content, size: %zu bytes", data.size());

    // Preview file content for debugging
    if (data.size() > 0 && fileType == "USD") {
      size_t previewSize = std::min(data.size(), size_t(50));
      std::string preview(reinterpret_cast<const char*>(data.data()), previewSize);
      MIDDLEWARE_LOG_INFO("Content preview: %s%s",
        preview.c_str(),
        data.size() > previewSize ? "..." : "");
    }

    // Part 4: Receive hash (last part)
    zmq::message_t hashMsg;
    auto hashRes = zmqSocket->recv(hashMsg, zmq::recv_flags::none);
    if (!hashRes || hashRes.value() == 0) {
      MIDDLEWARE_LOG_ERROR("Failed to receive hash");
      return false;
    }

    // IMPORTANT: Check if *more* parts are coming (should NOT be)
    if (zmqSocket->get(zmq::sockopt::rcvmore)) {
      MIDDLEWARE_LOG_WARNING("Received hash but more message parts follow unexpectedly. Draining extra parts.");
      // Drain remaining parts to avoid state issues
      zmq::message_t drainMsg;
      while (zmqSocket->get(zmq::sockopt::rcvmore)) {
        zmqSocket->recv(drainMsg, zmq::recv_flags::none);
      }
    }

    hash = hashMsg.to_string();
    MIDDLEWARE_LOG_INFO("Received hash: %s", hash.c_str());

    // Send reply back to the client
    bool replyResult = sendReply(clientIdentity, "OK");
    if (!replyResult) {
      MIDDLEWARE_LOG_WARNING("Failed to send reply after file reception");
    }

    return true;
  }
  catch (const zmq::error_t& e) {
    // Handle ZMQ errors (e.g., socket closed, context terminated, interruption)
    if (e.num() == ETERM || e.num() == ENOTSOCK) {
      // Context terminated or socket closed - likely during shutdown
      MIDDLEWARE_LOG_ERROR("ZeroMQ receive error (shutdown/closed): %s", e.what());
      connected = false; // Mark as disconnected on these errors
    } else if (e.num() == EINTR) {
      // Interrupted system call - might happen if signal received
      MIDDLEWARE_LOG_WARNING("ZeroMQ receive interrupted");
    } else if (e.num() == EAGAIN) {
      // Resource temporarily unavailable (using non-blocking flag, which we aren't here)
      MIDDLEWARE_LOG_ERROR("ZeroMQ receive error EAGAIN (unexpected for blocking): %s", e.what());
    }
    else {
      MIDDLEWARE_LOG_ERROR("ZeroMQ error during receive/send: %s (errno: %d)", e.what(), e.num());
      // Consider marking disconnected based on the error type
    }
    return false;
  }
  catch (const std::exception& e) {
    MIDDLEWARE_LOG_ERROR("Standard exception during receive/send: %s", e.what());
    return false;
  }
}

bool ZmqConnector::sendReply(zmq::message_t& identity, const std::string& response) {
    try {
        auto sendIdRes = zmqSocket->send(identity, zmq::send_flags::sndmore);
        if (!sendIdRes) {
            MIDDLEWARE_LOG_ERROR("Failed to send identity in reply");
            return false;
        }

        zmq::message_t replyMsg(response.data(), response.size());
        auto sendReplyRes = zmqSocket->send(replyMsg, zmq::send_flags::none);
        if (!sendReplyRes) {
            MIDDLEWARE_LOG_ERROR("Failed to send reply message");
            return false;
        }

        MIDDLEWARE_LOG_INFO("Reply sent successfully: %s", response.c_str());
        return true;
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in sendReply: %s", e.what());
        return false;
    }
}

void ZmqConnector::disconnect() {
    if (!connected) {
        return;
    }

    MIDDLEWARE_LOG_INFO("Disconnecting ZMQ Router from %s", currentEndpoint.c_str());
    connected = false; // Set flag early to prevent concurrent use attempts
    try {
        if (zmqSocket) {
            // Unbind the socket if it was bound
            if (!currentEndpoint.empty()) {
                try {
                    zmqSocket->unbind(currentEndpoint);
                    MIDDLEWARE_LOG_INFO("Unbound from %s", currentEndpoint.c_str());
                } catch (const zmq::error_t& e) {
                    // Log error during unbind but continue cleanup
                    MIDDLEWARE_LOG_ERROR("ZeroMQ error during unbind: %s (errno: %d)", e.what(), e.num());
                }
            }
            // Close the socket
            zmqSocket->close();
            zmqSocket.reset(); // Release ownership
            MIDDLEWARE_LOG_INFO("ZMQ socket closed");
        }
    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error during socket close: %s (errno: %d)", e.what(), e.num());
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Standard exception during socket disconnect: %s", e.what());
    }

    // Terminate the context (must happen after sockets using it are closed)
    try {
        if (zmqContext) {
            zmqContext->close();
            zmqContext.reset();
            MIDDLEWARE_LOG_INFO("ZMQ context terminated");
        }
    } catch (const zmq::error_t& e) {
        MIDDLEWARE_LOG_ERROR("ZeroMQ error during context close/termination: %s (errno: %d)", e.what(), e.num());
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Standard exception during context disconnect: %s", e.what());
    }

    currentEndpoint.clear();
    lastReceivedMessage.clear();
    MIDDLEWARE_LOG_INFO("ZMQ disconnected");
}

bool ZmqConnector::isConnected() const {
    // Check both the flag and the validity of the socket pointer
    return connected && (zmqSocket != nullptr);
}

// Helper method for cleanup during initialization errors
void ZmqConnector::cleanup() {
    zmqSocket.reset();
    zmqContext.reset();
    currentEndpoint.clear();
    lastReceivedMessage.clear();
    connected = false;
}

}
