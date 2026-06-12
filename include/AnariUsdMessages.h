#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace anari_usd_middleware {

/**
 * ANARI USD ZMQ Message Types
 * Based on UsdBridgeZmqBroker.h message protocol
 */
enum class ZmqMessageType : uint32_t {
    // Worker registration/heartbeat
    WORKER_READY = 1,
    WORKER_HEARTBEAT = 2,
    BROKER_ACK = 10,
    
    // Worker status queries
    REQ_WORKER_STATUS = 20,    // Request worker status information
    REQ_WORKER_COUNT = 21,     // Request total number of workers
    REQ_WORKER_LIST = 24,      // Request list of all workers
    RESP_WORKER_STATUS = 22,   // Response with worker status
    RESP_WORKER_COUNT = 23,    // Response with worker count
    RESP_WORKER_LIST = 25,     // Response with worker list
    
    // File request/response (Laptop ↔ Broker ↔ Workers)
    REQ_LIST_FILES = 100,      // Request list of files from a rank
    REQ_GET_FILE = 101,        // Request specific file from a rank
    REQ_GET_FRAME = 102,       // Request all files for a frame number
    REQ_GET_PROPERTY = 400,    // Request property value (e.g., total_workers)
    
    // Responses
    RESP_FILE_LIST = 200,      // Response with list of files
    RESP_FILE_CHUNK = 201,     // File data chunk
    RESP_FILE_COMPLETE = 202,  // File transmission complete
    RESP_NO_FILE = 203,        // File not found
    RESP_ERROR = 204,          // Error occurred
    RESP_PROPERTY = 401,       // Response with property value
    
    // Push notifications (Worker → Broker → Laptop)
    NOTIFY_FILE_UPDATE = 300,  // Notification that a file has been updated
    NOTIFY_COMMIT_COMPLETE = 301,  // Notification that scene commit is complete
    NOTIFY_FILE_UPDATE_V2 = 302  // V2: includes hashPrev128 + hasOldData for diff streaming
};

/**
 * Magic number for ANARI USD messages: "USDF" (0x55534446)
 */
constexpr uint32_t ANARI_USD_MAGIC = 0x55534446;

/**
 * File Request Message Structure
 * Sent from laptop to broker to request files
 */
#pragma pack(push, 1)
struct ZmqFileRequest {
    uint32_t magic;            // 0x55534446 ("USDF")
    uint32_t message_type;     // ZmqMessageType
    uint32_t request_id;       // Unique request ID
    int32_t target_rank;       // Which rank to request from (-1 = all ranks)
    char filename[256];        // Relative path
    uint32_t chunk_size;       // Preferred chunk size (0 = default 4MB)
    
    ZmqFileRequest() {
        memset(this, 0, sizeof(ZmqFileRequest));
        magic = ANARI_USD_MAGIC;
    }
    
    void setFilename(const std::string& fname) {
        memset(filename, 0, sizeof(filename));
        size_t copyLen = std::min(fname.size(), sizeof(filename) - 1);
        memcpy(filename, fname.c_str(), copyLen);
    }
    
    std::string getFilename() const {
        return std::string(filename, strnlen(filename, sizeof(filename)));
    }
};
#pragma pack(pop)

/**
 * File Chunk Response Message Structure
 * Sent from broker to laptop with file data chunks
 */
#pragma pack(push, 1)
struct ZmqFileChunk {
    uint32_t magic;            // 0x55534446 ("USDF")
    uint32_t message_type;     // RESP_FILE_CHUNK
    uint32_t request_id;
    int32_t source_rank;
    char filename[256];
    uint64_t file_size;
    uint64_t chunk_offset;
    uint32_t chunk_size;
    // Followed by chunk_size bytes of data
    
    ZmqFileChunk() {
        memset(this, 0, sizeof(ZmqFileChunk));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK);
    }
    
    void setFilename(const std::string& fname) {
        memset(filename, 0, sizeof(filename));
        size_t copyLen = std::min(fname.size(), sizeof(filename) - 1);
        memcpy(filename, fname.c_str(), copyLen);
    }
    
    std::string getFilename() const {
        return std::string(filename, strnlen(filename, sizeof(filename)));
    }
};
#pragma pack(pop)

/**
 * File List Response Message Structure
 * Sent from broker to laptop with list of available files
 */
#pragma pack(push, 1)
struct ZmqFileListResponse {
    uint32_t magic;            // 0x55534446 ("USDF")
    uint32_t message_type;     // RESP_FILE_LIST
    uint32_t request_id;
    int32_t source_rank;
    uint32_t file_count;       // Number of files in the list
    // Followed by file_count * 256-byte filenames
};
#pragma pack(pop)

/**
 * File Complete Message Structure
 * Sent from broker to laptop to signal end of file transfer
 */
#pragma pack(push, 1)
struct ZmqFileComplete {
    uint32_t magic;            // 0x55534446 ("USDF")
    uint32_t message_type;     // RESP_FILE_COMPLETE
    uint32_t request_id;
    int32_t source_rank;
    char filename[256];
    uint64_t total_size;       // Total file size transferred
    
    ZmqFileComplete() {
        memset(this, 0, sizeof(ZmqFileComplete));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE);
    }
    
    void setFilename(const std::string& fname) {
        memset(filename, 0, sizeof(filename));
        size_t copyLen = std::min(fname.size(), sizeof(filename) - 1);
        memcpy(filename, fname.c_str(), copyLen);
    }
    
    std::string getFilename() const {
        return std::string(filename, strnlen(filename, sizeof(filename)));
    }
};
#pragma pack(pop)

/**
 * Worker Status Request Structure
 * Sent from laptop to broker to query worker status
 */
#pragma pack(push, 1)
struct ZmqWorkerStatusRequest {
    uint32_t magic;            // 0x55534446 ("USDF")
    uint32_t message_type;     // REQ_WORKER_STATUS or REQ_WORKER_COUNT
    uint32_t request_id;
    int32_t target_rank;       // -1 for all workers, specific rank for individual
    
    ZmqWorkerStatusRequest() {
        memset(this, 0, sizeof(ZmqWorkerStatusRequest));
        magic = ANARI_USD_MAGIC;
    }
};
#pragma pack(pop)

/**
 * Worker Status Response Structure
 * Sent from broker to laptop with worker status information
 */
#pragma pack(push, 1)
struct ZmqWorkerStatusResponse {
    uint32_t magic;            // 0x55534446 ("USDF")
    uint32_t message_type;     // RESP_WORKER_STATUS or RESP_WORKER_COUNT
    uint32_t request_id;
    int32_t source_rank;       // -1 for broker, specific rank for worker
    uint32_t worker_count;     // Total number of workers (for RESP_WORKER_COUNT)
    uint32_t worker_status;    // 0=offline, 1=idle, 2=busy, 3=error
    uint64_t last_heartbeat;   // Unix timestamp of last heartbeat
    char hostname[64];         // Worker hostname
    char gpu_info[128];        // GPU information if available
    
    ZmqWorkerStatusResponse() {
        memset(this, 0, sizeof(ZmqWorkerStatusResponse));
        magic = ANARI_USD_MAGIC;
    }
    
    void setHostname(const std::string& host) {
        memset(hostname, 0, sizeof(hostname));
        size_t copyLen = std::min(host.size(), sizeof(hostname) - 1);
        memcpy(hostname, host.c_str(), copyLen);
    }
    
    void setGpuInfo(const std::string& gpu) {
        memset(gpu_info, 0, sizeof(gpu_info));
        size_t copyLen = std::min(gpu.size(), sizeof(gpu_info) - 1);
        memcpy(gpu_info, gpu.c_str(), copyLen);
    }
    
    std::string getHostname() const {
        return std::string(hostname, strnlen(hostname, sizeof(hostname)));
    }
    
    std::string getGpuInfo() const {
        return std::string(gpu_info, strnlen(gpu_info, sizeof(gpu_info)));
    }
};
#pragma pack(pop)

/**
 * Worker List Response Structure
 * Sent from broker to laptop with list of all workers
 */
#pragma pack(push, 1)
struct ZmqWorkerListResponse {
    uint32_t magic;            // 0x55534446 ("USDF")
    uint32_t message_type;     // RESP_WORKER_LIST
    uint32_t request_id;
    int32_t source_rank;       // -1 for broker
    uint32_t worker_count;     // Total number of workers
    // Followed by worker_count * ZmqWorkerInfo structures
    // Each ZmqWorkerInfo is: int32_t rank, char hostname[64], char ip_address[64]
    
    ZmqWorkerListResponse() {
        memset(this, 0, sizeof(ZmqWorkerListResponse));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::RESP_WORKER_LIST);
    }
};
#pragma pack(pop)

/**
 * Worker Info Structure (used in worker list response)
 */
#pragma pack(push, 1)
struct ZmqWorkerInfo {
    int32_t rank;
    char hostname[64];
    char ip_address[64];
    
    ZmqWorkerInfo() {
        memset(this, 0, sizeof(ZmqWorkerInfo));
    }
    
    void setHostname(const std::string& host) {
        memset(hostname, 0, sizeof(hostname));
        size_t copyLen = std::min(host.size(), sizeof(hostname) - 1);
        memcpy(hostname, host.c_str(), copyLen);
    }
    
    void setIpAddress(const std::string& ip) {
        memset(ip_address, 0, sizeof(ip_address));
        size_t copyLen = std::min(ip.size(), sizeof(ip_address) - 1);
        memcpy(ip_address, ip.c_str(), copyLen);
    }
    
    std::string getHostname() const {
        return std::string(hostname, strnlen(hostname, sizeof(hostname)));
    }
    
    std::string getIpAddress() const {
        return std::string(ip_address, strnlen(ip_address, sizeof(ip_address)));
    }
};
#pragma pack(pop)

/**
 * Error Response Message Structure
 * Sent from broker to laptop when an error occurs
 */
#pragma pack(push, 1)
struct ZmqErrorResponse {
    uint32_t magic;            // 0x55534446 ("USDF")
    uint32_t message_type;     // RESP_ERROR
    uint32_t request_id;
    int32_t error_code;
    char error_message[256];
    
    ZmqErrorResponse() {
        memset(this, 0, sizeof(ZmqErrorResponse));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::RESP_ERROR);
    }
    
    void setErrorMessage(const std::string& msg) {
        memset(error_message, 0, sizeof(error_message));
        size_t copyLen = std::min(msg.size(), sizeof(error_message) - 1);
        memcpy(error_message, msg.c_str(), copyLen);
    }
    
    std::string getErrorMessage() const {
        return std::string(error_message, strnlen(error_message, sizeof(error_message)));
    }
};
#pragma pack(pop)

/**
 * Property Response Message Structure
 * Sent from broker to laptop with property value
 */
#pragma pack(push, 1)
struct ZmqPropertyResponse {
    uint32_t magic;            // 0x55534446 ("USDF")
    uint32_t message_type;     // RESP_PROPERTY (401)
    uint32_t request_id;       // Match with request
    int32_t property_type;     // 0=int32, 1=string, -1=error
    int32_t int_value;         // For integer properties
    char string_value[256];    // For string properties
    
    ZmqPropertyResponse() {
        memset(this, 0, sizeof(ZmqPropertyResponse));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::RESP_PROPERTY);
    }
    
    void setStringValue(const std::string& str) {
        memset(string_value, 0, sizeof(string_value));
        size_t copyLen = std::min(str.size(), sizeof(string_value) - 1);
        memcpy(string_value, str.c_str(), copyLen);
    }
    
    std::string getStringValue() const {
        return std::string(string_value, strnlen(string_value, sizeof(string_value)));
    }
};
#pragma pack(pop)

/**
 * File Notification Message Structure
 * Sent from broker to client as push notification.
 * V1 (type 300/301): fields above hashPrev128 are valid.
 * V2 (type 302): hashPrev128[2] + hasOldData also populated for diff streaming.
 */
#pragma pack(push, 1)
struct ZmqFileNotification {
    uint32_t magic;            // 0x55534446 ("USDF")
    uint32_t message_type;     // NOTIFY_FILE_UPDATE or NOTIFY_COMMIT_COMPLETE
    int32_t source_rank;       // Which rank sent this
    char filename[256];        // Filename that was updated
    uint64_t file_size;        // Current file size
    uint64_t timestamp;        // Unix timestamp of update
    uint64_t hash128[2];       // XXH3-128 hash of new file data (from broker)
    uint64_t hashPrev128[2];   // XXH3-128 hash of old file data (0 if first time)
    bool hasOldData;           // true if hashPrev128 is valid (for delta streaming)

    ZmqFileNotification() {
        memset(this, 0, sizeof(ZmqFileNotification));
        magic = ANARI_USD_MAGIC;
    }

    std::string getFilename() const {
        return std::string(filename, strnlen(filename, sizeof(filename)));
    }
};
#pragma pack(pop)

/**
 * Worker Registration Message Structure
 * Sent from worker to broker during registration
 */
#pragma pack(push, 1)
struct ZmqWorkerReady {
    uint32_t magic;            // 0x55534446 ("USDF")
    uint32_t message_type;     // WORKER_READY
    int32_t rank;
    char hostname[64];
    char ip_address[64];
    
    ZmqWorkerReady() {
        memset(this, 0, sizeof(ZmqWorkerReady));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::WORKER_READY);
    }
    
    void setHostname(const std::string& host) {
        memset(hostname, 0, sizeof(hostname));
        size_t copyLen = std::min(host.size(), sizeof(hostname) - 1);
        memcpy(hostname, host.c_str(), copyLen);
    }
    
    void setIpAddress(const std::string& ip) {
        memset(ip_address, 0, sizeof(ip_address));
        size_t copyLen = std::min(ip.size(), sizeof(ip_address) - 1);
        memcpy(ip_address, ip.c_str(), copyLen);
    }
};
#pragma pack(pop)

/**
 * Broker Acknowledgment Message Structure
 * Sent from broker to worker after registration
 */
#pragma pack(push, 1)
struct ZmqBrokerAck {
    uint32_t magic;            // 0x55534446 ("USDF")
    uint32_t message_type;     // BROKER_ACK
    int32_t broker_rank;
    char broker_address[64];
    
    ZmqBrokerAck() {
        memset(this, 0, sizeof(ZmqBrokerAck));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::BROKER_ACK);
    }
    
    void setBrokerAddress(const std::string& addr) {
        memset(broker_address, 0, sizeof(broker_address));
        size_t copyLen = std::min(addr.size(), sizeof(broker_address) - 1);
        memcpy(broker_address, addr.c_str(), copyLen);
    }
};
#pragma pack(pop)

/**
 * Utility functions for message validation
 */
namespace MessageUtils {
    inline bool isValidMagic(uint32_t magic) {
        return magic == ANARI_USD_MAGIC;
    }
    
    inline bool isValidMessageType(uint32_t type) {
        return type >= 1 && type <= 401;
    }

    inline bool isNotificationType(uint32_t type) {
        return type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_FILE_UPDATE) ||
               type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_FILE_UPDATE_V2) ||
               type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_COMMIT_COMPLETE);
    }
    
    inline std::string getMessageTypeName(uint32_t type) {
        switch (static_cast<ZmqMessageType>(type)) {
            case ZmqMessageType::WORKER_READY: return "WORKER_READY";
            case ZmqMessageType::WORKER_HEARTBEAT: return "WORKER_HEARTBEAT";
            case ZmqMessageType::BROKER_ACK: return "BROKER_ACK";
            case ZmqMessageType::REQ_WORKER_STATUS: return "REQ_WORKER_STATUS";
            case ZmqMessageType::REQ_WORKER_COUNT: return "REQ_WORKER_COUNT";
            case ZmqMessageType::REQ_WORKER_LIST: return "REQ_WORKER_LIST";
            case ZmqMessageType::RESP_WORKER_STATUS: return "RESP_WORKER_STATUS";
            case ZmqMessageType::RESP_WORKER_COUNT: return "RESP_WORKER_COUNT";
            case ZmqMessageType::RESP_WORKER_LIST: return "RESP_WORKER_LIST";
            case ZmqMessageType::REQ_LIST_FILES: return "REQ_LIST_FILES";
            case ZmqMessageType::REQ_GET_FILE: return "REQ_GET_FILE";
            case ZmqMessageType::REQ_GET_FRAME: return "REQ_GET_FRAME";
            case ZmqMessageType::REQ_GET_PROPERTY: return "REQ_GET_PROPERTY";
            case ZmqMessageType::RESP_FILE_LIST: return "RESP_FILE_LIST";
            case ZmqMessageType::RESP_FILE_CHUNK: return "RESP_FILE_CHUNK";
            case ZmqMessageType::RESP_FILE_COMPLETE: return "RESP_FILE_COMPLETE";
            case ZmqMessageType::RESP_NO_FILE: return "RESP_NO_FILE";
            case ZmqMessageType::RESP_ERROR: return "RESP_ERROR";
            case ZmqMessageType::RESP_PROPERTY: return "RESP_PROPERTY";
            case ZmqMessageType::NOTIFY_FILE_UPDATE: return "NOTIFY_FILE_UPDATE";
            case ZmqMessageType::NOTIFY_FILE_UPDATE_V2: return "NOTIFY_FILE_UPDATE_V2";
            case ZmqMessageType::NOTIFY_COMMIT_COMPLETE: return "NOTIFY_COMMIT_COMPLETE";
            default: return "UNKNOWN";
        }
    }
}

/**
 * File information (name + size + source rank)
 */
struct FileInfo {
    std::string name;
    uint64_t size;
    int32_t source_rank;
    uint64_t hash128[2];           // XXH3-128 hash

    FileInfo() : size(0), source_rank(-1) { hash128[0] = 0; hash128[1] = 0; }
    FileInfo(const std::string& n, uint64_t s) : name(n), size(s), source_rank(-1) { hash128[0] = 0; hash128[1] = 0; }
    FileInfo(const std::string& n, uint64_t s, int32_t r) : name(n), size(s), source_rank(r) { hash128[0] = 0; hash128[1] = 0; }
};

} // namespace anari_usd_middleware