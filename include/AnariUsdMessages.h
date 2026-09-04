#pragma once

// Compatibility wrapper.
//
// The canonical protocol definitions live in AnariUsdProtocol.h.
// This header keeps the old anari_usd_middleware:: names working for existing
// middleware code.

#include "AnariUsdProtocol.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace anari_usd_middleware
{

using anari_usd_protocol::ANARI_USD_MAGIC;
using anari_usd_protocol::ANARI_USD_PROTOCOL_VERSION;
using anari_usd_protocol::ANARI_USD_MIN_SUPPORTED_PROTOCOL_VERSION;
using anari_usd_protocol::ANARI_USD_DEFAULT_CHUNK_SIZE;

using anari_usd_protocol::ZmqMessageType;
using anari_usd_protocol::ZmqSceneChangeType;
using anari_usd_protocol::ZmqPropertyValuetype;

using anari_usd_protocol::ZmqFileRequest;
using anari_usd_protocol::ZmqFileChunk;
using anari_usd_protocol::ZmqFileListResponse;
using anari_usd_protocol::ZmqFileComplete;
using anari_usd_protocol::ZmqWorkerStatusRequest;
using anari_usd_protocol::ZmqWorkerStatusResponse;
using anari_usd_protocol::ZmqWorkerListResponse;
using anari_usd_protocol::ZmqWorkerInfo;
using anari_usd_protocol::ZmqErrorResponse;
using anari_usd_protocol::ZmqPropertyResponse;
using anari_usd_protocol::ZmqFileNotification;
using anari_usd_protocol::ZmqWorkerReady;
using anari_usd_protocol::ZmqBrokerAck;
using anari_usd_protocol::ZmqSceneUpdate;

namespace MessageUtils = anari_usd_protocol::MessageUtils;

struct FileInfo
{
    std::string name;
    uint64_t size;
    int32_t source_rank;
    uint64_t hash128[2];

    FileInfo() : size(0), source_rank(-1) { hash128[0] = 0; hash128[1] = 0; }
    FileInfo(const std::string& n, uint64_t s) : name(n), size(s), source_rank(-1) { hash128[0] = 0; hash128[1] = 0; }
    FileInfo(const std::string& n, uint64_t s, int32_t r) : name(n), size(s), source_rank(r) { hash128[0] = 0; hash128[1] = 0; }
};

} // namespace anari_usd_middleware
