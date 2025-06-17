#pragma once

#include <memory>
#include <string>
#include <vector>
#include <zmq.hpp>
#include "MiddlewareLogging.h"

namespace anari_usd_middleware {

/**
 * Handles ZeroMQ communication for the ANARI-USD middleware.
 * Implements a ROUTER socket for receiving data from DEALER clients.
 */
class ZmqConnector {
public:
    ZmqConnector();
    ~ZmqConnector();

    /**
     * Initializes the ZeroMQ connection.
     * @param endpoint The ZeroMQ endpoint to bind to (e.g., "tcp://*:13456").
     *                 If null, defaults to "tcp://*:5556".
     * @return True if initialization was successful, false otherwise.
     */
    bool initialize(const char* endpoint = nullptr);

    /**
     * Disconnects and cleans up the ZeroMQ connection.
     */
    void disconnect();

    /**
     * Checks if the ZeroMQ connection is active.
     * @return True if connected, false otherwise.
     */
    bool isConnected() const;

    /**
     * Receives a file from a client.
     * Expects a multi-part message: [Identity] [Filename] [Content] [Hash]
     * @param filename Output parameter for the received filename.
     * @param data Output parameter for the received file data.
     * @param hash Output parameter for the received file hash.
     * @return True if a file was successfully received, false otherwise.
     */
    bool receiveFile(std::string& filename, std::vector<uint8_t>& data, std::string& hash);

    /**
     * Receives any message format and handles simple test messages.
     * @return True if a message was successfully processed.
     */
    bool receiveAnyMessage();

    /**
     * Gets the raw socket handle for polling operations.
     * @return The raw ZeroMQ socket handle, or nullptr if not connected.
     */
    void* getSocket() const;

    /**
     * Gets the last received message as a string.
     * @return The last received message.
     */
    const std::string& getLastReceivedMessage() const;

private:
    std::unique_ptr<zmq::context_t> zmqContext;
    std::unique_ptr<zmq::socket_t> zmqSocket;
    std::string currentEndpoint;
    std::string lastReceivedMessage;
    bool connected;

    /**
     * Helper method for cleanup during initialization errors.
     */
    void cleanup();

    /**
     * Helper method to send a reply to a client.
     * @param identity The client's identity frame.
     * @param response The response message to send.
     * @return True if the reply was sent successfully, false otherwise.
     */
    bool sendReply(zmq::message_t& identity, const std::string& response);
};

} 
