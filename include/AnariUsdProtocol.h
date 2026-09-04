#pragma once

// Canonical ANARI-USD / JUSYNC ZMQ wire protocol.
//
// IMPORTANT:
// - This header is the single source of truth for packed ZMQ message layout.
// - Do not change existing field order, field size, or message IDs.
// - Add new messages as new IDs and new structs.
// - Consumers must safely ignore unknown future message types.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace anari_usd_protocol
{

inline constexpr uint32_t ANARI_USD_MAGIC = 0x55534446;             // "USDF"
inline constexpr uint32_t ANARI_USD_PROTOCOL_VERSION = 3;
inline constexpr uint32_t ANARI_USD_MIN_SUPPORTED_PROTOCOL_VERSION = 1;
inline constexpr uint32_t ANARI_USD_DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024;

enum class ZmqMessageType : uint32_t
{
    // Worker registration / heartbeat
    WORKER_READY = 1,
    WORKER_HEARTBEAT = 2,
    BROKER_ACK = 10,

    // Worker status queries
    REQ_WORKER_STATUS = 20,
    REQ_WORKER_COUNT = 21,
    RESP_WORKER_COUNT = 23,
    REQ_WORKER_LIST = 24,
    RESP_WORKER_STATUS = 22,
    RESP_WORKER_LIST = 25,

    // File request / response
    REQ_LIST_FILES = 100,
    REQ_GET_FILE = 101,
    REQ_GET_FRAME = 102,

    RESP_FILE_LIST = 200,
    RESP_FILE_CHUNK = 201,
    RESP_FILE_COMPLETE = 202,
    RESP_NO_FILE = 203,
    RESP_ERROR = 204,

    // Push notifications
    NOTIFY_FILE_UPDATE = 300,
    NOTIFY_COMMIT_COMPLETE = 301,
    NOTIFY_FILE_UPDATE_V2 = 302,
    NOTIFY_SCENE_UPDATE = 303,
    NOTIFY_PROPERTY_UPDATE = 304,

    // Property query
    REQ_GET_PROPERTY = 400,
    RESP_PROPERTY = 401,

    // Scene snapshot
    REQ_SCENE_SNAPSHOT = 500,
    RESP_SCENE_SNAPSHOT = 501
};

enum class ZmqSceneChangeType : int32_t
{
    None = 0,
    Created = 1,
    Removed = 2,
    Visibility = 3,
    Transform = 4,
    Material = 5,
    Attribute = 6,
    Commit = 7,
    Property = 8
};

enum class ZmqPropertyValuetype : int32_t
{
    None = 0,
    Int = 1,
    Bool = 2,
    Float = 3,
    Float2 = 4,
    Float3 = 5,
    Float4 = 6,
    String = 7,
    Path = 8,
    ArrayRef = 9
};

#pragma pack(push, 1)

struct ZmqFileRequest
{
    uint32_t magic;
    uint32_t message_type;
    uint32_t request_id;
    int32_t target_rank;
    char filename[256];
    uint32_t chunk_size;

    ZmqFileRequest()
    {
        memset(this, 0, sizeof(ZmqFileRequest));
        magic = ANARI_USD_MAGIC;
    }

    void setFilename(const std::string& fname)
    {
        memset(filename, 0, sizeof(filename));
        const size_t copyLen = std::min(fname.size(), sizeof(filename) - 1);
        memcpy(filename, fname.c_str(), copyLen);
    }

    std::string getFilename() const
    {
        return std::string(filename, strnlen(filename, sizeof(filename)));
    }
};

struct ZmqFileChunk
{
    uint32_t magic;
    uint32_t message_type;
    uint32_t request_id;
    int32_t source_rank;
    char filename[256];
    uint64_t file_size;
    uint64_t chunk_offset;
    uint32_t chunk_size;

    ZmqFileChunk()
    {
        memset(this, 0, sizeof(ZmqFileChunk));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK);
    }

    void setFilename(const std::string& fname)
    {
        memset(filename, 0, sizeof(filename));
        const size_t copyLen = std::min(fname.size(), sizeof(filename) - 1);
        memcpy(filename, fname.c_str(), copyLen);
    }

    std::string getFilename() const
    {
        return std::string(filename, strnlen(filename, sizeof(filename)));
    }
};

struct ZmqFileListResponse
{
    uint32_t magic;
    uint32_t message_type;
    uint32_t request_id;
    int32_t source_rank;
    uint32_t file_count;
};

struct ZmqFileComplete
{
    uint32_t magic;
    uint32_t message_type;
    uint32_t request_id;
    int32_t source_rank;
    char filename[256];
    uint64_t total_size;

    ZmqFileComplete()
    {
        memset(this, 0, sizeof(ZmqFileComplete));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE);
    }

    void setFilename(const std::string& fname)
    {
        memset(filename, 0, sizeof(filename));
        const size_t copyLen = std::min(fname.size(), sizeof(filename) - 1);
        memcpy(filename, fname.c_str(), copyLen);
    }

    std::string getFilename() const
    {
        return std::string(filename, strnlen(filename, sizeof(filename)));
    }
};

struct ZmqWorkerStatusRequest
{
    uint32_t magic;
    uint32_t message_type;
    uint32_t request_id;
    int32_t target_rank;

    ZmqWorkerStatusRequest()
    {
        memset(this, 0, sizeof(ZmqWorkerStatusRequest));
        magic = ANARI_USD_MAGIC;
    }
};

struct ZmqWorkerStatusResponse
{
    uint32_t magic;
    uint32_t message_type;
    uint32_t request_id;
    int32_t source_rank;
    uint32_t worker_count;
    uint32_t worker_status;
    uint64_t last_heartbeat;
    char hostname[64];
    char gpu_info[128];

    ZmqWorkerStatusResponse()
    {
        memset(this, 0, sizeof(ZmqWorkerStatusResponse));
        magic = ANARI_USD_MAGIC;
    }

    void setHostname(const std::string& host)
    {
        memset(hostname, 0, sizeof(hostname));
        const size_t copyLen = std::min(host.size(), sizeof(hostname) - 1);
        memcpy(hostname, host.c_str(), copyLen);
    }

    void setGpuInfo(const std::string& gpu)
    {
        memset(gpu_info, 0, sizeof(gpu_info));
        const size_t copyLen = std::min(gpu.size(), sizeof(gpu_info) - 1);
        memcpy(gpu_info, gpu.c_str(), copyLen);
    }

    std::string getHostname() const
    {
        return std::string(hostname, strnlen(hostname, sizeof(hostname)));
    }

    std::string getGpuInfo() const
    {
        return std::string(gpu_info, strnlen(gpu_info, sizeof(gpu_info)));
    }
};

struct ZmqWorkerListResponse
{
    uint32_t magic;
    uint32_t message_type;
    uint32_t request_id;
    int32_t source_rank;
    uint32_t worker_count;

    ZmqWorkerListResponse()
    {
        memset(this, 0, sizeof(ZmqWorkerListResponse));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::RESP_WORKER_LIST);
    }
};

struct ZmqWorkerInfo
{
    int32_t rank;
    char hostname[64];
    char ip_address[64];

    ZmqWorkerInfo()
    {
        memset(this, 0, sizeof(ZmqWorkerInfo));
    }

    void setHostname(const std::string& host)
    {
        memset(hostname, 0, sizeof(hostname));
        const size_t copyLen = std::min(host.size(), sizeof(hostname) - 1);
        memcpy(hostname, host.c_str(), copyLen);
    }

    void setIpAddress(const std::string& ip)
    {
        memset(ip_address, 0, sizeof(ip_address));
        const size_t copyLen = std::min(ip.size(), sizeof(ip_address) - 1);
        memcpy(ip_address, ip.c_str(), copyLen);
    }

    std::string getHostname() const
    {
        return std::string(hostname, strnlen(hostname, sizeof(hostname)));
    }

    std::string getIpAddress() const
    {
        return std::string(ip_address, strnlen(ip_address, sizeof(ip_address)));
    }
};

struct ZmqErrorResponse
{
    uint32_t magic;
    uint32_t message_type;
    uint32_t request_id;
    int32_t error_code;
    char error_message[256];

    ZmqErrorResponse()
    {
        memset(this, 0, sizeof(ZmqErrorResponse));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::RESP_ERROR);
    }

    void setErrorMessage(const std::string& msg)
    {
        memset(error_message, 0, sizeof(error_message));
        const size_t copyLen = std::min(msg.size(), sizeof(error_message) - 1);
        memcpy(error_message, msg.c_str(), copyLen);
    }

    std::string getErrorMessage() const
    {
        return std::string(error_message, strnlen(error_message, sizeof(error_message)));
    }
};

struct ZmqPropertyResponse
{
    uint32_t magic;
    uint32_t message_type;
    uint32_t request_id;
    int32_t property_type;
    int32_t int_value;
    char string_value[256];

    ZmqPropertyResponse()
    {
        memset(this, 0, sizeof(ZmqPropertyResponse));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::RESP_PROPERTY);
    }

    void setStringValue(const std::string& str)
    {
        memset(string_value, 0, sizeof(string_value));
        const size_t copyLen = std::min(str.size(), sizeof(string_value) - 1);
        memcpy(string_value, str.c_str(), copyLen);
    }

    std::string getStringValue() const
    {
        return std::string(string_value, strnlen(string_value, sizeof(string_value)));
    }
};

struct ZmqFileNotification
{
    uint32_t magic;
    uint32_t message_type;
    int32_t source_rank;
    char filename[256];
    uint64_t file_size;
    uint64_t timestamp;
    uint64_t hash128[2];
    uint64_t hashPrev128[2];
    bool hasOldData;

    ZmqFileNotification()
    {
        memset(this, 0, sizeof(ZmqFileNotification));
        magic = ANARI_USD_MAGIC;
    }

    std::string getFilename() const
    {
        return std::string(filename, strnlen(filename, sizeof(filename)));
    }
};

struct ZmqWorkerReady
{
    uint32_t magic;
    uint32_t message_type;
    int32_t rank;
    char hostname[64];
    char ip_address[64];

    ZmqWorkerReady()
    {
        memset(this, 0, sizeof(ZmqWorkerReady));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::WORKER_READY);
    }

    void setHostname(const std::string& host)
    {
        memset(hostname, 0, sizeof(hostname));
        const size_t copyLen = std::min(host.size(), sizeof(hostname) - 1);
        memcpy(hostname, host.c_str(), copyLen);
    }

    void setIpAddress(const std::string& ip)
    {
        memset(ip_address, 0, sizeof(ip_address));
        const size_t copyLen = std::min(ip.size(), sizeof(ip_address) - 1);
        memcpy(ip_address, ip.c_str(), copyLen);
    }
};

struct ZmqBrokerAck
{
    uint32_t magic;
    uint32_t message_type;
    int32_t broker_rank;
    char broker_address[64];

    ZmqBrokerAck()
    {
        memset(this, 0, sizeof(ZmqBrokerAck));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::BROKER_ACK);
    }

    void setBrokerAddress(const std::string& addr)
    {
        memset(broker_address, 0, sizeof(broker_address));
        const size_t copyLen = std::min(addr.size(), sizeof(broker_address) - 1);
        memcpy(broker_address, addr.c_str(), copyLen);
    }
};

// Typed scene / property update.
//
// Used for NOTIFY_SCENE_UPDATE (303) and NOTIFY_PROPERTY_UPDATE (304).
// The same struct is intentionally used for both so old/new clients can share
// one parser. `change_type` disambiguates the semantic meaning.
struct ZmqSceneUpdate
{
    uint32_t magic;
    uint32_t message_type;
    int32_t source_rank;
    uint64_t timestamp;
    uint64_t commit_id;
    uint64_t revision;
    char prim_path[256];
    char property_name[64];
    int32_t change_type;     // ZmqSceneChangeType
    int32_t value_type;      // ZmqPropertyValuetype
    int64_t int_value;
    float float_value;
    float vec4[4];
    char string_value[128];
    uint32_t payload_size;   // Reserved for future inline payloads / array refs.
    uint32_t reserved;

    ZmqSceneUpdate()
    {
        memset(this, 0, sizeof(ZmqSceneUpdate));
        magic = ANARI_USD_MAGIC;
        message_type = static_cast<uint32_t>(ZmqMessageType::NOTIFY_SCENE_UPDATE);
    }

    void setPrimPath(const std::string& path)
    {
        memset(prim_path, 0, sizeof(prim_path));
        const size_t copyLen = std::min(path.size(), sizeof(prim_path) - 1);
        memcpy(prim_path, path.c_str(), copyLen);
    }

    void setPropertyName(const std::string& name)
    {
        memset(property_name, 0, sizeof(property_name));
        const size_t copyLen = std::min(name.size(), sizeof(property_name) - 1);
        memcpy(property_name, name.c_str(), copyLen);
    }

    void setStringValue(const std::string& value)
    {
        memset(string_value, 0, sizeof(string_value));
        const size_t copyLen = std::min(value.size(), sizeof(string_value) - 1);
        memcpy(string_value, value.c_str(), copyLen);
    }

    std::string getPrimPath() const
    {
        return std::string(prim_path, strnlen(prim_path, sizeof(prim_path)));
    }

    std::string getPropertyName() const
    {
        return std::string(property_name, strnlen(property_name, sizeof(property_name)));
    }

    std::string getStringValue() const
    {
        return std::string(string_value, strnlen(string_value, sizeof(string_value)));
    }
};

#pragma pack(pop)

namespace MessageUtils
{
    inline bool isValidMagic(uint32_t magic)
    {
        return magic == ANARI_USD_MAGIC;
    }

    inline bool isValidMessageType(uint32_t type)
    {
        switch (static_cast<ZmqMessageType>(type))
        {
            case ZmqMessageType::WORKER_READY:
            case ZmqMessageType::WORKER_HEARTBEAT:
            case ZmqMessageType::BROKER_ACK:
            case ZmqMessageType::REQ_WORKER_STATUS:
            case ZmqMessageType::REQ_WORKER_COUNT:
            case ZmqMessageType::RESP_WORKER_COUNT:
            case ZmqMessageType::REQ_WORKER_LIST:
            case ZmqMessageType::RESP_WORKER_STATUS:
            case ZmqMessageType::RESP_WORKER_LIST:
            case ZmqMessageType::REQ_LIST_FILES:
            case ZmqMessageType::REQ_GET_FILE:
            case ZmqMessageType::REQ_GET_FRAME:
            case ZmqMessageType::RESP_FILE_LIST:
            case ZmqMessageType::RESP_FILE_CHUNK:
            case ZmqMessageType::RESP_FILE_COMPLETE:
            case ZmqMessageType::RESP_NO_FILE:
            case ZmqMessageType::RESP_ERROR:
            case ZmqMessageType::NOTIFY_FILE_UPDATE:
            case ZmqMessageType::NOTIFY_COMMIT_COMPLETE:
            case ZmqMessageType::NOTIFY_FILE_UPDATE_V2:
            case ZmqMessageType::NOTIFY_SCENE_UPDATE:
            case ZmqMessageType::NOTIFY_PROPERTY_UPDATE:
            case ZmqMessageType::REQ_GET_PROPERTY:
            case ZmqMessageType::RESP_PROPERTY:
            case ZmqMessageType::REQ_SCENE_SNAPSHOT:
            case ZmqMessageType::RESP_SCENE_SNAPSHOT:
                return true;
            default:
                return false;
        }
    }

    inline bool isNotificationType(uint32_t type)
    {
        return type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_FILE_UPDATE) ||
               type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_COMMIT_COMPLETE) ||
               type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_FILE_UPDATE_V2) ||
               type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_SCENE_UPDATE) ||
               type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_PROPERTY_UPDATE);
    }

    inline bool isSceneUpdateType(uint32_t type)
    {
        return type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_SCENE_UPDATE) ||
               type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_PROPERTY_UPDATE);
    }

    inline const char* getMessageTypeName(uint32_t type)
    {
        switch (static_cast<ZmqMessageType>(type))
        {
            case ZmqMessageType::WORKER_READY: return "WORKER_READY";
            case ZmqMessageType::WORKER_HEARTBEAT: return "WORKER_HEARTBEAT";
            case ZmqMessageType::BROKER_ACK: return "BROKER_ACK";
            case ZmqMessageType::REQ_WORKER_STATUS: return "REQ_WORKER_STATUS";
            case ZmqMessageType::REQ_WORKER_COUNT: return "REQ_WORKER_COUNT";
            case ZmqMessageType::RESP_WORKER_COUNT: return "RESP_WORKER_COUNT";
            case ZmqMessageType::REQ_WORKER_LIST: return "REQ_WORKER_LIST";
            case ZmqMessageType::RESP_WORKER_STATUS: return "RESP_WORKER_STATUS";
            case ZmqMessageType::RESP_WORKER_LIST: return "RESP_WORKER_LIST";
            case ZmqMessageType::REQ_LIST_FILES: return "REQ_LIST_FILES";
            case ZmqMessageType::REQ_GET_FILE: return "REQ_GET_FILE";
            case ZmqMessageType::REQ_GET_FRAME: return "REQ_GET_FRAME";
            case ZmqMessageType::RESP_FILE_LIST: return "RESP_FILE_LIST";
            case ZmqMessageType::RESP_FILE_CHUNK: return "RESP_FILE_CHUNK";
            case ZmqMessageType::RESP_FILE_COMPLETE: return "RESP_FILE_COMPLETE";
            case ZmqMessageType::RESP_NO_FILE: return "RESP_NO_FILE";
            case ZmqMessageType::RESP_ERROR: return "RESP_ERROR";
            case ZmqMessageType::NOTIFY_FILE_UPDATE: return "NOTIFY_FILE_UPDATE";
            case ZmqMessageType::NOTIFY_COMMIT_COMPLETE: return "NOTIFY_COMMIT_COMPLETE";
            case ZmqMessageType::NOTIFY_FILE_UPDATE_V2: return "NOTIFY_FILE_UPDATE_V2";
            case ZmqMessageType::NOTIFY_SCENE_UPDATE: return "NOTIFY_SCENE_UPDATE";
            case ZmqMessageType::NOTIFY_PROPERTY_UPDATE: return "NOTIFY_PROPERTY_UPDATE";
            case ZmqMessageType::REQ_GET_PROPERTY: return "REQ_GET_PROPERTY";
            case ZmqMessageType::RESP_PROPERTY: return "RESP_PROPERTY";
            case ZmqMessageType::REQ_SCENE_SNAPSHOT: return "REQ_SCENE_SNAPSHOT";
            case ZmqMessageType::RESP_SCENE_SNAPSHOT: return "RESP_SCENE_SNAPSHOT";
            default: return "UNKNOWN";
        }
    }
}

static_assert(sizeof(ZmqFileRequest) == 276, "ZmqFileRequest wire layout changed");
static_assert(sizeof(ZmqFileChunk) == 292, "ZmqFileChunk wire layout changed");
static_assert(sizeof(ZmqFileListResponse) == 20, "ZmqFileListResponse wire layout changed");
static_assert(sizeof(ZmqFileComplete) == 280, "ZmqFileComplete wire layout changed");
static_assert(sizeof(ZmqWorkerStatusRequest) == 16, "ZmqWorkerStatusRequest wire layout changed");
static_assert(sizeof(ZmqWorkerStatusResponse) == 224, "ZmqWorkerStatusResponse wire layout changed");
static_assert(sizeof(ZmqWorkerListResponse) == 20, "ZmqWorkerListResponse wire layout changed");
static_assert(sizeof(ZmqWorkerInfo) == 132, "ZmqWorkerInfo wire layout changed");
static_assert(sizeof(ZmqErrorResponse) == 272, "ZmqErrorResponse wire layout changed");
static_assert(sizeof(ZmqPropertyResponse) == 276, "ZmqPropertyResponse wire layout changed");
static_assert(sizeof(ZmqFileNotification) == 317, "ZmqFileNotification wire layout changed");
static_assert(sizeof(ZmqWorkerReady) == 140, "ZmqWorkerReady wire layout changed");
static_assert(sizeof(ZmqBrokerAck) == 76, "ZmqBrokerAck wire layout changed");
static_assert(sizeof(ZmqSceneUpdate) == 528, "ZmqSceneUpdate wire layout changed");

} // namespace anari_usd_protocol
