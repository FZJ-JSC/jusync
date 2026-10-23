#ifndef ANARI_USD_MIDDLEWARE_EXPORTS
#define ANARI_USD_MIDDLEWARE_EXPORTS
#endif

#include "AnariUsdMiddleware_C.h"
#include "AnariUsdMiddleware.h"
#include "AnariUsdClient.h"
#include "CollisionProcessor.h"
#include "UsdProcessor.h"
#include "AnariUsdMessages.h"
#include "StreamDiagnostics.h"

#include <memory>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <map>
#include <string>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif


static std::unique_ptr<anari_usd_middleware::AnariUsdMiddleware> g_middleware;

static std::unique_ptr<anari_usd_middleware::CollisionProcessor> g_collision_processor;

static std::atomic<FileReceivedCallback_C> g_file_callback = nullptr;
static std::atomic<FileReceivedSpanCallback_C> g_file_span_callback = nullptr;
static std::atomic<MessageReceivedCallback_C> g_message_callback = nullptr;

static int g_default_collision_complexity = COLLISION_COMPLEX;

static std::mutex g_middleware_mutex;

struct CountingSemaphore {
    std::mutex mtx;
    std::condition_variable cv;
    int count;
    explicit CountingSemaphore(int initial) : count(initial) {}
    void acquire() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return count > 0; });
        count--;
    }
    void release() {
        std::unique_lock<std::mutex> lock(mtx);
        count++;
        cv.notify_one();
    }
};
static CountingSemaphore g_parse_semaphore{4}; // allow 4 concurrent USD parses

struct ParseGuard {
    ~ParseGuard() { g_parse_semaphore.release(); }
};

static anari_usd_middleware::UsdProcessor& GetThreadLocalUsdProcessor() {
    // One processor per thread (pImpl-allocated on first use per thread).

    static thread_local anari_usd_middleware::UsdProcessor processor;
    return processor;
}


extern "C" {

int InitializeMiddleware_C(const char* endpoint) {
    try {
        #ifdef ENABLE_CUDA_ACCELERATION
        MIDDLEWARE_LOG_INFO("🔥🔥🔥 GPU ACCELERATION ENABLED IN DLL 🔥🔥🔥");
        #else
        MIDDLEWARE_LOG_INFO("⚠️⚠️⚠️ GPU ACCELERATION NOT COMPILED IN DLL ⚠️⚠️⚠️");
        #endif

        if (!g_middleware) {
            g_middleware = std::make_unique<anari_usd_middleware::AnariUsdMiddleware>();
        }

        if (!g_collision_processor) {
            g_collision_processor = std::make_unique<anari_usd_middleware::CollisionProcessor>();
        }

        std::string endpoint_str = endpoint ? endpoint : "tcp://*:5556";
        bool result = g_middleware->initialize(endpoint_str.c_str());

        if (result) {
            if (g_file_callback || g_file_span_callback) {
                g_middleware->registerUpdateCallback([](const anari_usd_middleware::FileData& file_data) {
                    FileReceivedSpanCallback_C span_callback = g_file_span_callback.load(std::memory_order_acquire);
                    if (span_callback) {
                        span_callback(
                            file_data.filename.c_str(),
                            file_data.data.empty() ? nullptr : file_data.data.data(),
                            file_data.data.size(),
                            file_data.hash.c_str(),
                            file_data.fileType.c_str());
                        return;
                    }

                    FileReceivedCallback_C file_callback = g_file_callback.load(std::memory_order_acquire);
                    if (!file_callback) {
                        return;
                    }

                    CFileData c_data = {};

#ifdef _WIN32
                    strncpy_s(c_data.filename, sizeof(c_data.filename), file_data.filename.c_str(), _TRUNCATE);
                    strncpy_s(c_data.hash, sizeof(c_data.hash), file_data.hash.c_str(), _TRUNCATE);
                    strncpy_s(c_data.file_type, sizeof(c_data.file_type), file_data.fileType.c_str(), _TRUNCATE);
#else
                    snprintf(c_data.filename, sizeof(c_data.filename), "%s", file_data.filename.c_str());
                    snprintf(c_data.hash, sizeof(c_data.hash), "%s", file_data.hash.c_str());
                    snprintf(c_data.file_type, sizeof(c_data.file_type), "%s", file_data.fileType.c_str());
#endif

                    c_data.data_size = file_data.data.size();
                    if (c_data.data_size > 0) {
                        c_data.data = new unsigned char[c_data.data_size];
                        std::memcpy(c_data.data, file_data.data.data(), c_data.data_size);
                    } else {
                        c_data.data = nullptr;
                    }

                    file_callback(&c_data);

                    if (c_data.data) {
                        delete[] c_data.data;
                        c_data.data = nullptr;
                        c_data.data_size = 0;
                    }
                });
            }

            MessageReceivedCallback_C message_callback = g_message_callback.load(std::memory_order_acquire);
            if (message_callback) {
                g_middleware->registerMessageCallback([message_callback](const std::string& message) {
                    if (message_callback) {
                        message_callback(message.c_str());
                    }
                });
            }
        }

        return result ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

void ShutdownMiddleware_C() {
    if (g_middleware) {
        g_middleware->shutdown();
        g_middleware.reset();
    }

    if (g_collision_processor) {
        g_collision_processor.reset();
    }

    g_file_callback = nullptr;
    g_file_span_callback = nullptr;
    g_message_callback = nullptr;
}

int IsConnected_C() {
    return (g_middleware && g_middleware->isConnected()) ? 1 : 0;
}

const char* GetStatusInfo_C() {
    static std::string status;
    if (g_middleware) {
        status = g_middleware->getStatusInfo();
        return status.c_str();
    }
    return "Middleware not initialized";
}

int StartReceiving_C() {
    return (g_middleware && g_middleware->startReceiving()) ? 1 : 0;
}

void StopReceiving_C() {
    if (g_middleware) {
        g_middleware->stopReceiving();
    }
}


int ConnectToBroker_C(const char* broker_endpoint, int timeout_ms) {
    if (!g_middleware) {
        return 0;
    }

    try {
        std::string endpoint = broker_endpoint ? broker_endpoint : "tcp://localhost:5556";
        return g_middleware->connectToBroker(endpoint.c_str(), timeout_ms) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

void DisconnectFromBroker_C() {
    if (g_middleware) {
        g_middleware->disconnectFromBroker();
    }
}

int IsBrokerConnected_C() {
    return (g_middleware && g_middleware->isBrokerConnected()) ? 1 : 0;
}

int RequestTotalWorkerCount_C(uint32_t* out_total_count, int timeout_ms) {
    if (!g_middleware || !out_total_count) {
        return 0;
    }

    try {
        uint32_t count = 0;
        bool success = g_middleware->requestTotalWorkerCount(count, timeout_ms);
        *out_total_count = count;
        return success ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int RequestWorkerListStringCallback_C(WorkerListCallback_C callback, int timeout_ms) {
    if (!g_middleware || !callback) {
        return 0;
    }

    try {
        std::vector<std::tuple<int32_t, std::string, std::string>> workerList;
        bool success = g_middleware->requestWorkerListString(workerList, timeout_ms);
        if (!success) {
            return 0;
        }

        uint32_t count = static_cast<uint32_t>(workerList.size());
        if (count > 1024) count = 1024; // Safety limit

        std::vector<int32_t> ranks(count);
        std::vector<const char*> hostnames(count);
        std::vector<const char*> ips(count);

        for (uint32_t i = 0; i < count; i++) {
            ranks[i] = std::get<0>(workerList[i]);
            hostnames[i] = std::get<1>(workerList[i]).c_str();
            ips[i] = std::get<2>(workerList[i]).c_str();
        }

        callback(count, ranks.data(), hostnames.data(), ips.data());
        return 1;
    } catch (...) {
        return 0;
    }
}

int RequestWorkerListString_C(uint32_t* out_worker_count, unsigned char** out_data, size_t* out_size, int timeout_ms) {
    if (!g_middleware || !out_worker_count || !out_data || !out_size) {
        return 0;
    }

    *out_worker_count = 0;
    *out_data = nullptr;
    *out_size = 0;

    try {
        std::vector<std::tuple<int32_t, std::string, std::string>> workerList;
        bool success = g_middleware->requestWorkerListString(workerList, timeout_ms);
        if (!success) {
            return 0;
        }

        *out_worker_count = static_cast<uint32_t>(workerList.size());

        if (workerList.empty()) {
            return 1;
        }

        std::string result;
        for (auto& worker : workerList) {
            if (result.empty() == false) {
                result += ';';
            }
            result += std::to_string(std::get<0>(worker));
            result += ':';
            result += std::get<1>(worker);
            result += ':';
            result += std::get<2>(worker);
        }

        *out_size = result.size();
        *out_data = static_cast<unsigned char*>(std::malloc(*out_size));
        if (*out_data) {
            std::memcpy(*out_data, result.data(), *out_size);
        }
        return (*out_data) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int RequestWorkerCountExcludingRank0_C(uint32_t* out_count, int timeout_ms) {
    if (!g_middleware || !out_count) {
        return 0;
    }

    try {
        uint32_t count = 0;
        bool success = g_middleware->requestWorkerCount(count, timeout_ms);
        *out_count = count;
        return success ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

static int32_t usd_format_priority(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return 0;
    if (strcmp(dot, ".usda") == 0) return 4;
    if (strcmp(dot, ".usdc") == 0) return 3;
    if (strcmp(dot, ".usdz") == 0) return 2;
    if (strcmp(dot, ".usd")  == 0) return 1;
    return 99; // non-USD — never deduped
}

static void dedup_usd_filenames_strings(std::vector<std::string>& files) {
    std::map<std::string, size_t> bestIdx;
    for (size_t i = 0; i < files.size(); ++i) {
        int32_t pri = usd_format_priority(files[i].c_str());
        if (pri == 99) continue;
        size_t dotPos = files[i].find_last_of('.');
        if (dotPos == std::string::npos) continue;
        std::string stem = files[i].substr(0, dotPos);
        auto it = bestIdx.find(stem);
        if (it == bestIdx.end()) bestIdx[stem] = i;
        else if (pri > usd_format_priority(files[it->second].c_str())) it->second = i;
    }
    size_t cur = 0;
    for (const auto& kv : bestIdx) {
        if (kv.second > cur) std::swap(files[cur], files[kv.second]);
        cur++;
    }
    std::fill(files.begin() + cur, files.end(), std::string());
    files.erase(std::remove(files.begin(), files.end(), std::string()), files.end());
}

static void dedup_usd_filenames_info(std::vector<anari_usd_middleware::FileInfo>& files) {
    std::vector<size_t> usdIndices;
    std::vector<size_t> nonUsdIndices;
    for (size_t i = 0; i < files.size(); ++i) {
        int32_t pri = usd_format_priority(files[i].name.c_str());
        if (pri == 99) {
            nonUsdIndices.push_back(i);
        } else {
            usdIndices.push_back(i);
        }
    }

    std::map<std::string, size_t> bestIdx;
    for (size_t idx : usdIndices) {
        int32_t pri = usd_format_priority(files[idx].name.c_str());
        size_t dotPos = files[idx].name.find_last_of('.');
        if (dotPos == std::string::npos) continue;
        std::string stem = files[idx].name.substr(0, dotPos);
        auto it = bestIdx.find(stem);
        if (it == bestIdx.end()) {
            bestIdx[stem] = idx;
        } else if (pri > usd_format_priority(files[it->second].name.c_str())) {
            it->second = idx;
        }
    }

    std::vector<anari_usd_middleware::FileInfo> result;
    result.reserve(bestIdx.size() + nonUsdIndices.size());
    for (const auto& kv : bestIdx) {
        result.push_back(files[kv.second]);
    }
    for (size_t idx : nonUsdIndices) {
        result.push_back(files[idx]);
    }
    files = std::move(result);
}

int RequestFileList_C(int32_t target_rank, char*** out_files, size_t* out_count, int timeout_ms) {
    if (!g_middleware || !out_files || !out_count) {
        return 0;
    }

    try {
        std::vector<std::string> files;
        if (!g_middleware->requestFileList(target_rank, files, timeout_ms)) {
            *out_count = 0;
            *out_files = nullptr;
            return 0;
        }
        dedup_usd_filenames_strings(files);

        *out_count = files.size();
        *out_files = new char*[*out_count];

        for (size_t i = 0; i < files.size(); ++i) {
            size_t len = files[i].length() + 1;
            (*out_files)[i] = new char[len];
#ifdef _WIN32
            strncpy_s((*out_files)[i], len, files[i].c_str(), _TRUNCATE);
#else
            snprintf((*out_files)[i], len, "%s", files[i].c_str());
#endif
        }

        return 1;
    } catch (...) {
        *out_count = 0;
        *out_files = nullptr;
        return 0;
    }
}

int RequestFileListWithSizes_C(int32_t target_rank, char*** out_names, uint64_t** out_sizes, size_t* out_count, int timeout_ms) {
    if (!g_middleware || !out_names || !out_sizes || !out_count) {
        return 0;
    }

    try {
        std::vector<anari_usd_middleware::FileInfo> files;
        if (!g_middleware->requestFileListWithSizes(target_rank, files, timeout_ms)) {
            *out_count = 0;
            *out_names = nullptr;
            *out_sizes = nullptr;
            return 0;
        }
        dedup_usd_filenames_info(files);

        *out_count = files.size();
        *out_names = new char*[*out_count];
        *out_sizes = new uint64_t[*out_count];

        for (size_t i = 0; i < files.size(); ++i) {
            size_t len = files[i].name.length() + 1;
            (*out_names)[i] = new char[len];
#ifdef _WIN32
            strncpy_s((*out_names)[i], len, files[i].name.c_str(), _TRUNCATE);
#else
            snprintf((*out_names)[i], len, "%s", files[i].name.c_str());
#endif
            (*out_sizes)[i] = files[i].size;
        }

        return 1;
    } catch (...) {
        *out_count = 0;
        *out_names = nullptr;
        *out_sizes = nullptr;
        return 0;
    }
}

int RequestFileListWithSizesAndRanks_C(int32_t target_rank, char*** out_names, uint64_t** out_sizes, int32_t** out_ranks, uint64_t** out_hash_lo, uint64_t** out_hash_hi, size_t* out_count, int timeout_ms) {
    if (!g_middleware || !out_names || !out_sizes || !out_ranks || !out_hash_lo || !out_hash_hi || !out_count) {
        return 0;
    }

    try {
        std::vector<anari_usd_middleware::FileInfo> files;
        if (!g_middleware->requestFileListWithSizes(target_rank, files, timeout_ms)) {
            *out_count = 0;
            *out_names = nullptr;
            *out_sizes = nullptr;
            *out_ranks = nullptr;
            *out_hash_lo = nullptr;
            *out_hash_hi = nullptr;
            return 0;
        }
        dedup_usd_filenames_info(files);

        *out_count = files.size();
        *out_names = new char*[*out_count];
        *out_sizes = new uint64_t[*out_count];
        *out_ranks = new int32_t[*out_count];
        *out_hash_lo = new uint64_t[*out_count];
        *out_hash_hi = new uint64_t[*out_count];

        for (size_t i = 0; i < files.size(); ++i) {
            size_t len = files[i].name.length() + 1;
            (*out_names)[i] = new char[len];
#ifdef _WIN32
            strncpy_s((*out_names)[i], len, files[i].name.c_str(), _TRUNCATE);
#else
            snprintf((*out_names)[i], len, "%s", files[i].name.c_str());
#endif
            (*out_sizes)[i] = files[i].size;
            (*out_ranks)[i] = files[i].source_rank;
            (*out_hash_lo)[i] = files[i].hash128[0];
            (*out_hash_hi)[i] = files[i].hash128[1];
        }

        return 1;
    } catch (...) {
        *out_count = 0;
        *out_names = nullptr;
        *out_sizes = nullptr;
        *out_ranks = nullptr;
        *out_hash_lo = nullptr;
        *out_hash_hi = nullptr;
        return 0;
    }
}

int RequestFile_C(const char* filename, int32_t target_rank,
                   unsigned char** out_data, size_t* out_size, int timeout_ms) {
    if (!g_middleware || !filename || !out_data || !out_size) {
        return 0;
    }

    try {
        std::vector<uint8_t> fileData;
        MIDDLEWARE_LOG_INFO("RequestFile_C: Calling g_middleware->requestFile('%s', %d, timeout=%d)",
                           filename, target_rank, timeout_ms);
        bool requestResult = g_middleware->requestFile(filename, target_rank, fileData, timeout_ms);
        if (!requestResult) {
            MIDDLEWARE_LOG_ERROR("RequestFile_C: g_middleware->requestFile failed for '%s'", filename);
            *out_size = 0;
            *out_data = nullptr;
            return 0;
        }
        MIDDLEWARE_LOG_INFO("RequestFile_C: g_middleware->requestFile succeeded, file size: %zu", fileData.size());

        *out_size = fileData.size();
        if (*out_size > 0) {
            const size_t MAX_REASONABLE_FILE_SIZE = 100ULL * 1024 * 1024 * 1024; // 100GB
            if (*out_size > MAX_REASONABLE_FILE_SIZE) {
                MIDDLEWARE_LOG_ERROR("File size suspiciously large: %zu bytes (max 100GB)", *out_size);
                *out_size = 0;
                *out_data = nullptr;
                return 0;
            }

            *out_data = new unsigned char[*out_size];
            if (!*out_data) {
                MIDDLEWARE_LOG_ERROR("Memory allocation failed for %zu bytes", *out_size);
                *out_size = 0;
                return 0;
            }
            std::memcpy(*out_data, fileData.data(), *out_size);
            MIDDLEWARE_LOG_DEBUG("Allocated %zu bytes at %p using new[]", *out_size, (void*)*out_data);
        } else {
            *out_data = nullptr;
            MIDDLEWARE_LOG_WARNING("File size is 0 bytes");
        }

        MIDDLEWARE_LOG_INFO("RequestFile_C succeeded: %zu bytes for '%s'", *out_size, filename);
        return 1;
    } catch (...) {
        *out_size = 0;
        *out_data = nullptr;
        return 0;
    }
}


int64_t RequestFileIntoBuffer_C(const char* filename,
                                int32_t target_rank,
                                int timeout_ms,
                                unsigned char* out_buffer,
                                size_t out_capacity,
                                size_t* out_size) {
    if (out_size) *out_size = 0;
    if (!g_middleware || !filename) {
        return -1;
    }
    try {
        size_t size = 0;
        int status = 0;
        if (!g_middleware->requestFileIntoBuffer(filename, target_rank,
                                                 out_buffer, out_capacity,
                                                 size, status, timeout_ms)) {
            return -1; // failure / timeout / not connected
        }
        if (out_size) *out_size = size;
        if (status == -1) return -2; // overflow
        return static_cast<int64_t>(size);
    } catch (...) {
        MIDDLEWARE_LOG_ERROR("RequestFileIntoBuffer_C: exception for '%s'", filename);
        return -1;
    }
}

int64_t RequestFileIntoBufferAbort_C(const char* filename,
                                     int32_t target_rank,
                                     int timeout_ms,
                                     unsigned char* out_buffer,
                                     size_t out_capacity,
                                     size_t* out_size,
                                     uint64_t abort_handle) {
    if (out_size) *out_size = 0;
    if (!g_middleware || !filename) {
        return -1;
    }
    try {
        size_t size = 0;
        int status = 0;
        if (!g_middleware->requestFileIntoBuffer(filename, target_rank,
                                                 out_buffer, out_capacity,
                                                 size, status, timeout_ms,
                                                 abort_handle)) {
            return -1; // failure / timeout / aborted
        }
        if (out_size) *out_size = size;
        if (status == -1) return -2; // overflow
        return static_cast<int64_t>(size);
    } catch (...) {
        MIDDLEWARE_LOG_ERROR("RequestFileIntoBufferAbort_C: exception for '%s'", filename);
        return -1;
    }
}

uint64_t JusyncAbortCreate_C(void) {
    if (!g_middleware) {
        return 0;
    }
    return g_middleware->createAbortToken();
}

int32_t JusyncAbortTrigger_C(uint64_t handle) {
    if (!g_middleware || handle == 0) {
        return 0;
    }
    return g_middleware->triggerAbortToken(handle) ? 1 : 0;
}

void JusyncAbortRelease_C(uint64_t handle) {
    if (!g_middleware || handle == 0) {
        return;
    }
    g_middleware->releaseAbortToken(handle);
}


namespace {

struct ParsedUSDHandle {
    std::vector<anari_usd_middleware::UsdProcessor::MeshData> meshes;
    std::vector<anari_usd_middleware::UsdProcessor::PointCloudData> clouds;
    std::string filename;
};

static inline uint8_t ScaleColorToFColor(float v) {
    float c = v * 255.0f;
    if (c < 0.0f) c = 0.0f;
    if (c > 255.0f) c = 255.0f;
    return static_cast<uint8_t>(c);
}

static inline uint8_t ScaleColorRaw(float v) {
    return static_cast<uint8_t>(v * 255.0f);
}

static bool FillMeshIntoBuffer(const anari_usd_middleware::UsdProcessor::MeshData& src,
                                CMeshDataFill& dst) {
    const size_t nVerts = src.points.size();

    if (dst.points && nVerts > 0) {
        for (size_t i = 0; i < nVerts; ++i) {
            dst.points[i * 3 + 0] = static_cast<double>(src.points[i].x);
            dst.points[i * 3 + 1] = static_cast<double>(-src.points[i].y);
            dst.points[i * 3 + 2] = static_cast<double>(src.points[i].z);
        }
    }

    if (dst.indices && !src.indices.empty()) {
        for (size_t i = 0; i < src.indices.size(); ++i) {
            dst.indices[i] = static_cast<int32_t>(src.indices[i]);
        }
    }

    const size_t nNormals = src.normals.size();
    if (dst.normals && nNormals > 0) {
        for (size_t i = 0; i < nNormals; ++i) {
            dst.normals[i * 3 + 0] = static_cast<double>(src.normals[i].x);
            dst.normals[i * 3 + 1] = static_cast<double>(-src.normals[i].y);
            dst.normals[i * 3 + 2] = static_cast<double>(src.normals[i].z);
        }
    }

    const size_t nUv = src.uvs.size();
    if (dst.uvs && nUv > 0) {
        for (size_t i = 0; i < nUv; ++i) {
            dst.uvs[i * 2 + 0] = static_cast<double>(src.uvs[i].x);
            dst.uvs[i * 2 + 1] = static_cast<double>(src.uvs[i].y);
        }
    }

    if (src.vertex_colors.size() == nVerts && nVerts > 0 && dst.vertex_colors8) {
        for (size_t i = 0; i < nVerts; ++i) {
            const auto& c = src.vertex_colors[i];
            dst.vertex_colors8[i * 4 + 0] = ScaleColorToFColor(c.r);
            dst.vertex_colors8[i * 4 + 1] = ScaleColorToFColor(c.g);
            dst.vertex_colors8[i * 4 + 2] = ScaleColorToFColor(c.b);
            dst.vertex_colors8[i * 4 + 3] = ScaleColorToFColor(c.a);
        }
    }

    return true;
}

static bool FillCloudIntoBuffer(const anari_usd_middleware::UsdProcessor::PointCloudData& src,
                                CPointCloudDataFill& dst) {
    const size_t n = src.positions.size();

    const bool bDirectLidarFill =
        dst.lidar_points != nullptr &&
        dst.lidar_point_version == 1u &&
        dst.lidar_point_stride == static_cast<int32_t>(sizeof(CPointCloudLidarPoint_v1));

    if (bDirectLidarFill && n > 0) {
        auto* outPoints = static_cast<CPointCloudLidarPoint_v1*>(dst.lidar_points);
        const bool bHasVertexColors = !src.vertex_colors.empty();

        for (size_t i = 0; i < n; ++i) {
            CPointCloudLidarPoint_v1& out = outPoints[i];

            out.location[0] = src.positions[i].x;
            out.location[1] = src.positions[i].z;
            out.location[2] = -src.positions[i].y;

            if (bHasVertexColors && i < src.vertex_colors.size()) {
                const auto& col = src.vertex_colors[i];
                out.color[0] = ScaleColorRaw(col.r);
                out.color[1] = ScaleColorRaw(col.g);
                out.color[2] = ScaleColorRaw(col.b);
                out.color[3] = ScaleColorRaw(col.a);
            } else {
                out.color[0] = 255;
                out.color[1] = 255;
                out.color[2] = 255;
                out.color[3] = 255;
            }

            out.normal[0] = 127;
            out.normal[1] = 127;
            out.normal[2] = 127;
            out.flags = 1u;
        }
    }

    if (dst.positions && n > 0) {
        for (size_t i = 0; i < n; ++i) {
            dst.positions[i * 3 + 0] = static_cast<double>(src.positions[i].x);
            dst.positions[i * 3 + 1] = static_cast<double>(src.positions[i].z);
            dst.positions[i * 3 + 2] = static_cast<double>(-src.positions[i].y);
        }
    }

    if (!src.vertex_colors.empty() && dst.colors8) {
        const size_t c = src.vertex_colors.size();
        for (size_t i = 0; i < c; ++i) {
            const auto& col = src.vertex_colors[i];
            dst.colors8[i * 4 + 0] = ScaleColorRaw(col.r);
            dst.colors8[i * 4 + 1] = ScaleColorRaw(col.g);
            dst.colors8[i * 4 + 2] = ScaleColorRaw(col.b);
            dst.colors8[i * 4 + 3] = ScaleColorRaw(col.a);
        }
    }

    if (!src.widths.empty() && dst.widths) {
        std::memcpy(dst.widths, src.widths.data(), src.widths.size() * sizeof(float));
    } else if (!src.scalarAttributes.empty() && n > 0 && dst.widths) {
        const size_t wcount = std::min(src.scalarAttributes.size(), n);
        for (size_t i = 0; i < wcount; ++i) {
            dst.widths[i] = src.scalarAttributes[i].x;
        }
    }

    for (int i = 0; i < 3; ++i) {
        dst.bounding_box_min[i] = 0.0f;
        dst.bounding_box_max[i] = 0.0f;
    }
    if (n > 0) {
        for (size_t i = 0; i < n; ++i) {
            dst.bounding_box_min[0] = fminf(dst.bounding_box_min[0], src.positions[i].x);
            dst.bounding_box_min[1] = fminf(dst.bounding_box_min[1], src.positions[i].y);
            dst.bounding_box_min[2] = fminf(dst.bounding_box_min[2], src.positions[i].z);
            dst.bounding_box_max[0] = fmaxf(dst.bounding_box_max[0], src.positions[i].x);
            dst.bounding_box_max[1] = fmaxf(dst.bounding_box_max[1], src.positions[i].y);
            dst.bounding_box_max[2] = fmaxf(dst.bounding_box_max[2], src.positions[i].z);
        }
    }
    return true;
}

} // namespace

int QueryUSDFullLayout_C(const unsigned char* buffer,
                         size_t buffer_size,
                         const char* filename,
                         void** out_handle,
                         CMeshLayout** out_mesh_layouts,
                         size_t* out_mesh_count,
                         CPointCloudLayout** out_cloud_layouts,
                         size_t* out_cloud_count) {
    if (!buffer || !filename || !out_handle || !out_mesh_layouts || !out_mesh_count ||
        !out_cloud_layouts || !out_cloud_count) {
        return 0;
    }
    *out_handle = nullptr;
    *out_mesh_layouts = nullptr;
    *out_mesh_count = 0;
    *out_cloud_layouts = nullptr;
    *out_cloud_count = 0;

    try {
        jusync_diag::Diagnostics::instance().mark(filename, jusync_diag::STAGE_PARSE_START);
        anari_usd_middleware::UsdProcessor& processor = GetThreadLocalUsdProcessor();
        std::vector<anari_usd_middleware::UsdProcessor::MeshData> mesh_data;
        std::vector<anari_usd_middleware::UsdProcessor::PointCloudData> pc_data;

        g_parse_semaphore.acquire();
        {
            ParseGuard guard;
            bool result = processor.LoadUSDBufferFromRaw(
                reinterpret_cast<const uint8_t*>(buffer), buffer_size,
                std::string(filename), mesh_data, &pc_data);
            if (!result) {
                MIDDLEWARE_LOG_ERROR("QueryUSDFullLayout_C: parse failed for '%s' (size=%zu bytes)",
                                     filename, buffer_size);
                return 0;
            }
        } // semaphore released here

        ParsedUSDHandle* handle = new ParsedUSDHandle{std::move(mesh_data), std::move(pc_data)};
        handle->filename = filename;
        *out_handle = handle;

        CMeshLayout* ml = nullptr;
        if (!handle->meshes.empty()) {
            ml = static_cast<CMeshLayout*>(std::calloc(handle->meshes.size(), sizeof(CMeshLayout)));
            for (size_t i = 0; i < handle->meshes.size(); ++i) {
                const auto& m = handle->meshes[i];
                snprintf(ml[i].element_name, sizeof(ml[i].element_name), "%s", m.elementName.c_str());
                snprintf(ml[i].type_name, sizeof(ml[i].type_name), "%s", m.typeName.c_str());
                ml[i].points_count = m.points.size();
                ml[i].indices_count = m.indices.size();
                ml[i].normals_count = m.normals.size();
                ml[i].uvs_count = m.uvs.size();
                ml[i].vertex_colors_count = m.vertex_colors.empty() ? 0 : m.points.size();
            }
        }
        *out_mesh_layouts = ml;
        *out_mesh_count = handle->meshes.size();

        CPointCloudLayout* cl = nullptr;
        if (!handle->clouds.empty()) {
            cl = static_cast<CPointCloudLayout*>(std::calloc(handle->clouds.size(), sizeof(CPointCloudLayout)));
            for (size_t i = 0; i < handle->clouds.size(); ++i) {
                const auto& c = handle->clouds[i];
                snprintf(cl[i].element_name, sizeof(cl[i].element_name), "%s", c.elementName.c_str());
                cl[i].point_count = c.positions.size();
                cl[i].has_colors = c.vertex_colors.empty() ? 0 : 1;
                cl[i].has_normals = c.normals.empty() ? 0 : 1;
                cl[i].has_widths = (!c.widths.empty() || !c.scalarAttributes.empty()) ? 1 : 0;
            }
        }
        *out_cloud_layouts = cl;
        *out_cloud_count = handle->clouds.size();

        MIDDLEWARE_LOG_INFO("QueryUSDFullLayout_C: %zu meshes + %zu point clouds for '%s'",
                            *out_mesh_count, *out_cloud_count, filename);
        jusync_diag::Diagnostics::instance().mark(filename, jusync_diag::STAGE_PARSE_DONE);
        return 1;
    } catch (...) {
        MIDDLEWARE_LOG_ERROR("QueryUSDFullLayout_C: exception for '%s'", filename);
        return 0;
    }
}

int FillUSDFull_C(void* handle,
                  CMeshDataFill* meshes,
                  size_t mesh_count,
                  CPointCloudDataFill* clouds,
                  size_t cloud_count) {
    auto* h = static_cast<ParsedUSDHandle*>(handle);
    if (!h) return 0;
    if (mesh_count > h->meshes.size() || cloud_count > h->clouds.size()) {
        MIDDLEWARE_LOG_ERROR("FillUSDFull_C: requested more elements than parsed (%zu meshes, %zu clouds)",
                             h->meshes.size(), h->clouds.size());
        return 0;
    }
    try {
        for (size_t i = 0; i < mesh_count; ++i) {
            if (!FillMeshIntoBuffer(h->meshes[i], meshes[i])) return 0;
        }
        for (size_t i = 0; i < cloud_count; ++i) {
            if (!FillCloudIntoBuffer(h->clouds[i], clouds[i])) return 0;
        }
        if (!h->filename.empty()) {
            jusync_diag::Diagnostics::instance().mark(h->filename, jusync_diag::STAGE_FILL_DONE);
        }
        return 1;
    } catch (...) {
        MIDDLEWARE_LOG_ERROR("FillUSDFull_C: exception");
        return 0;
    }
}

static bool FillMeshIntoBufferCompact(const anari_usd_middleware::UsdProcessor::MeshData& src,
                                      CMeshDataFillCompact& dst) {
    const size_t nVerts = src.points.size();

    if (dst.points && nVerts > 0) {
        for (size_t i = 0; i < nVerts; ++i) {
            dst.points[i * 3 + 0] = src.points[i].x;
            dst.points[i * 3 + 1] = -src.points[i].y;
            dst.points[i * 3 + 2] = src.points[i].z;
        }
    }

    if (dst.indices && !src.indices.empty()) {
        for (size_t i = 0; i < src.indices.size(); ++i) {
            dst.indices[i] = static_cast<int32_t>(src.indices[i]);
        }
    }

    const size_t nNormals = src.normals.size();
    if (dst.normals && nNormals > 0) {
        for (size_t i = 0; i < nNormals; ++i) {
            dst.normals[i * 3 + 0] = src.normals[i].x;
            dst.normals[i * 3 + 1] = -src.normals[i].y;
            dst.normals[i * 3 + 2] = src.normals[i].z;
        }
    }

    const size_t nUv = src.uvs.size();
    if (dst.uvs && nUv > 0) {
        for (size_t i = 0; i < nUv; ++i) {
            dst.uvs[i * 2 + 0] = src.uvs[i].x;
            dst.uvs[i * 2 + 1] = src.uvs[i].y;
        }
    }

    if (src.vertex_colors.size() == nVerts && nVerts > 0 && dst.vertex_colors8) {
        for (size_t i = 0; i < nVerts; ++i) {
            const auto& c = src.vertex_colors[i];
            dst.vertex_colors8[i * 4 + 0] = ScaleColorToFColor(c.r);
            dst.vertex_colors8[i * 4 + 1] = ScaleColorToFColor(c.g);
            dst.vertex_colors8[i * 4 + 2] = ScaleColorToFColor(c.b);
            dst.vertex_colors8[i * 4 + 3] = ScaleColorToFColor(c.a);
        }
    }

    return true;
}

int FillUSDFullCompact_C(void* handle,
                         CMeshDataFillCompact* meshes,
                         size_t mesh_count,
                         CPointCloudDataFill* clouds,
                         size_t cloud_count) {
    auto* h = static_cast<ParsedUSDHandle*>(handle);
    if (!h) return 0;
    if (mesh_count > h->meshes.size() || cloud_count > h->clouds.size()) {
        MIDDLEWARE_LOG_ERROR("FillUSDFullCompact_C: requested more elements than parsed (%zu meshes, %zu clouds)",
                             h->meshes.size(), h->clouds.size());
        return 0;
    }
    try {
        for (size_t i = 0; i < mesh_count; ++i) {
            if (!FillMeshIntoBufferCompact(h->meshes[i], meshes[i])) return 0;
        }
        for (size_t i = 0; i < cloud_count; ++i) {
            if (!FillCloudIntoBuffer(h->clouds[i], clouds[i])) return 0;
        }
        if (!h->filename.empty()) {
            jusync_diag::Diagnostics::instance().mark(h->filename, jusync_diag::STAGE_FILL_DONE);
        }
        return 1;
    } catch (...) {
        MIDDLEWARE_LOG_ERROR("FillUSDFullCompact_C: exception");
        return 0;
    }
}

void FreeParsedUSD_C(void* handle) {
    delete static_cast<ParsedUSDHandle*>(handle);
}

void FreeUSDFullLayouts_C(CMeshLayout* mesh_layouts,
                          size_t mesh_count,
                          CPointCloudLayout* cloud_layouts,
                          size_t cloud_count) {
    (void)mesh_count;
    (void)cloud_count;
    std::free(mesh_layouts);
    std::free(cloud_layouts);
}

int RequestFrame_C(int32_t frame_number, int32_t target_rank,
                   CFileData** out_files, size_t* out_count, int timeout_ms) {
    if (!g_middleware || !out_files || !out_count) {
        return 0;
    }

    try {
        std::vector<std::pair<std::string, std::vector<uint8_t>>> frameFiles;
        if (!g_middleware->requestFrame(frame_number, target_rank, frameFiles, timeout_ms)) {
            *out_count = 0;
            *out_files = nullptr;
            return 0;
        }

        *out_count = frameFiles.size();
        *out_files = new CFileData[*out_count];

        for (size_t i = 0; i < frameFiles.size(); ++i) {
            CFileData& c_file = (*out_files)[i];

#ifdef _WIN32
            strncpy_s(c_file.filename, sizeof(c_file.filename), frameFiles[i].first.c_str(), _TRUNCATE);
#else
            snprintf(c_file.filename, sizeof(c_file.filename), "%s", frameFiles[i].first.c_str());
#endif

            c_file.data_size = frameFiles[i].second.size();
            if (c_file.data_size > 0) {
                c_file.data = new unsigned char[c_file.data_size];
                std::memcpy(c_file.data, frameFiles[i].second.data(), c_file.data_size);
            } else {
                c_file.data = nullptr;
            }

            c_file.hash[0] = '\0';
            c_file.file_type[0] = '\0';
        }

        return 1;
    } catch (...) {
        *out_count = 0;
        *out_files = nullptr;
        return 0;
    }
}

void FreeFileList_C(char** files, size_t count) {
    if (!files) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        if (files[i]) {
            delete[] files[i];
        }
    }
    delete[] files;
}

void FreeFileListWithSizes_C(char** names, uint64_t* sizes, size_t count) {
    if (!names && !sizes) {
        return;
    }
    if (names) {
        for (size_t i = 0; i < count; ++i) {
            if (names[i]) {
                delete[] names[i];
            }
        }
        delete[] names;
    }
    if (sizes) {
        delete[] sizes;
    }
}

void FreeFileListWithSizesAndRanks_C(char** names, uint64_t* sizes, int32_t* ranks, uint64_t* hash_lo, uint64_t* hash_hi, size_t count) {
    if (!names && !sizes && !ranks && !hash_lo && !hash_hi) {
        return;
    }
    if (names) {
        for (size_t i = 0; i < count; ++i) {
            if (names[i]) {
                delete[] names[i];
            }
        }
        delete[] names;
    }
    if (sizes) {
        delete[] sizes;
    }
    if (ranks) {
        delete[] ranks;
    }
    if (hash_lo) {
        delete[] hash_lo;
    }
    if (hash_hi) {
        delete[] hash_hi;
    }
}


static thread_local std::vector<std::string> g_subdivision_scheme_storage;
static thread_local std::vector<std::string> g_uv_name_storage;

static void ConvertMeshDataToCFormat(const anari_usd_middleware::UsdProcessor::MeshData& src,
                                     CMeshData& dst) {
    dst.points = nullptr;
    dst.indices = nullptr;
    dst.normals = nullptr;
    dst.uvs = nullptr;
    dst.vertex_colors = nullptr;


    #ifdef _WIN32
    strncpy_s(dst.element_name, sizeof(dst.element_name), src.elementName.c_str(), _TRUNCATE);
    strncpy_s(dst.type_name, sizeof(dst.type_name), src.typeName.c_str(), _TRUNCATE);
    #else
    snprintf(dst.element_name, sizeof(dst.element_name), "%s", src.elementName.c_str());
    snprintf(dst.type_name, sizeof(dst.type_name), "%s", src.typeName.c_str());
    #endif


    size_t numVertices = src.points.size();
    dst.points_count = numVertices * 3;  // ✅ FIXED: Each vec3 = 3 floats
    if (dst.points_count > 0) {
        dst.points = new float[dst.points_count];
        for (size_t i = 0; i < numVertices; ++i) {
            dst.points[i * 3 + 0] = src.points[i].x;
            dst.points[i * 3 + 1] = src.points[i].y;
            dst.points[i * 3 + 2] = src.points[i].z;
        }
    }

    dst.indices_count = src.indices.size();
    if (dst.indices_count > 0) {
        dst.indices = new unsigned int[dst.indices_count];
        std::memcpy(dst.indices, src.indices.data(), dst.indices_count * sizeof(unsigned int));
    }

    size_t numNormals = src.normals.size();
    dst.normals_count = numNormals * 3;  // ✅ FIXED: Each vec3 = 3 floats
    if (dst.normals_count > 0) {
        dst.normals = new float[dst.normals_count];
        for (size_t i = 0; i < numNormals; ++i) {
            dst.normals[i * 3 + 0] = src.normals[i].x;
            dst.normals[i * 3 + 1] = src.normals[i].y;
            dst.normals[i * 3 + 2] = src.normals[i].z;
        }
    }

    size_t numUVs = src.uvs.size();
    dst.uvs_count = numUVs * 2;  // ✅ FIXED: Each vec2 = 2 floats
    if (dst.uvs_count > 0) {
        dst.uvs = new float[dst.uvs_count];
        for (size_t i = 0; i < numUVs; ++i) {
            dst.uvs[i * 2 + 0] = src.uvs[i].x;
            dst.uvs[i * 2 + 1] = src.uvs[i].y;
        }
    }

    size_t numColorFloats = src.vertex_colors.size();
    dst.vertex_colors_count = numColorFloats;  // Already flat float array (4 floats per color)
    if (dst.vertex_colors_count > 0) {
        dst.vertex_colors = new float[dst.vertex_colors_count];
        std::memcpy(dst.vertex_colors, src.vertex_colors.data(), dst.vertex_colors_count * sizeof(float));
    }

}


int LoadUSDBuffer_C(const unsigned char* buffer, size_t buffer_size, const char* filename,
                    CMeshData** out_meshes, size_t* out_count) {
    if (!buffer || !filename || !out_meshes || !out_count) {
        return 0;
    }

    try {
        std::vector<uint8_t> std_buffer(buffer, buffer + buffer_size);
        std::string std_filename(filename);
        std::vector<anari_usd_middleware::UsdProcessor::MeshData> mesh_data;

        anari_usd_middleware::UsdProcessor& processor = GetThreadLocalUsdProcessor();
        jusync_diag::Diagnostics::instance().mark(std_filename, jusync_diag::STAGE_PARSE_START);
        g_parse_semaphore.acquire();
        {
            ParseGuard guard;
            bool result = processor.LoadUSDBuffer(std_buffer, std_filename, mesh_data);


            if (!result || mesh_data.empty()) {
                *out_count = 0;
                *out_meshes = nullptr;
            return 0;
        }
        } // semaphore released here

        jusync_diag::Diagnostics::instance().mark(std_filename, jusync_diag::STAGE_PARSE_DONE);

        *out_count = mesh_data.size();
        *out_meshes = new CMeshData[*out_count];

        for (size_t i = 0; i < mesh_data.size(); ++i) {
            ConvertMeshDataToCFormat(mesh_data[i], (*out_meshes)[i]);

            (*out_meshes)[i].collision_type = COLLISION_NONE;
            (*out_meshes)[i].collision_vertices = nullptr;
            (*out_meshes)[i].collision_indices = nullptr;
            (*out_meshes)[i].collision_vertices_count = 0;
            (*out_meshes)[i].collision_indices_count = 0;

            for (int j = 0; j < 3; j++) {
                (*out_meshes)[i].bounding_box_min[j] = 0.0f;
                (*out_meshes)[i].bounding_box_max[j] = 0.0f;
                (*out_meshes)[i].sphere_center[j] = 0.0f;
            }
            (*out_meshes)[i].sphere_radius = 0.0f;
        }

        return 1;

    } catch (...) {
        *out_count = 0;
        *out_meshes = nullptr;
        return 0;
    }
}

int LoadUSDFromDisk_C(const char* filepath, CMeshData** out_meshes, size_t* out_count) {
    if (!filepath || !out_meshes || !out_count) {
        return 0;
    }

    try {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            *out_count = 0;
            *out_meshes = nullptr;
            return 0;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> buffer(size);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            *out_count = 0;
            *out_meshes = nullptr;
            return 0;
        }

        std::string std_filepath(filepath);
        std::vector<anari_usd_middleware::UsdProcessor::MeshData> mesh_data;

        anari_usd_middleware::UsdProcessor& processor = GetThreadLocalUsdProcessor();
        bool result = processor.LoadUSDFromDisk(std_filepath, mesh_data);


        if (!result || mesh_data.empty()) {
            *out_count = 0;
            *out_meshes = nullptr;
            return 0;
        }

        *out_count = mesh_data.size();
        *out_meshes = new CMeshData[*out_count];

        for (size_t i = 0; i < mesh_data.size(); ++i) {
            ConvertMeshDataToCFormat(mesh_data[i], (*out_meshes)[i]);

            (*out_meshes)[i].collision_type = COLLISION_NONE;
            (*out_meshes)[i].collision_vertices = nullptr;
            (*out_meshes)[i].collision_indices = nullptr;
            (*out_meshes)[i].collision_vertices_count = 0;
            (*out_meshes)[i].collision_indices_count = 0;

            for (int j = 0; j < 3; j++) {
                (*out_meshes)[i].bounding_box_min[j] = 0.0f;
                (*out_meshes)[i].bounding_box_max[j] = 0.0f;
                (*out_meshes)[i].sphere_center[j] = 0.0f;
            }
            (*out_meshes)[i].sphere_radius = 0.0f;
        }

        return 1;

    } catch (...) {
        *out_count = 0;
        *out_meshes = nullptr;
        return 0;
    }
}


int LoadUSDBufferWithCollision_C(const unsigned char* buffer,
                                  size_t buffer_size,
                                  const char* filename,
                                  int collision_complexity,
                                  CMeshData** out_meshes,
                                  size_t* out_count) {
    if (!g_middleware || !buffer || !filename || !out_meshes || !out_count) {
        return 0;
    }

    if (collision_complexity == -1) {
        collision_complexity = g_default_collision_complexity;
    }

    if (collision_complexity < COLLISION_NONE || collision_complexity > COLLISION_CONVEX_DECOMP) {
        return 0;
    }

    try {
        if (!g_collision_processor) {
            g_collision_processor = std::make_unique<anari_usd_middleware::CollisionProcessor>();
        }

        std::vector<uint8_t> std_buffer(buffer, buffer + buffer_size);
        std::string std_filename(filename);
        std::vector<anari_usd_middleware::UsdProcessor::MeshData> mesh_data;

        anari_usd_middleware::UsdProcessor& processor = GetThreadLocalUsdProcessor();
        bool result = processor.LoadUSDBuffer(std_buffer, std_filename, mesh_data);

        if (!result || mesh_data.empty()) {
            *out_count = 0;
            *out_meshes = nullptr;
            return 0;
        }

        *out_count = mesh_data.size();
        *out_meshes = new CMeshData[*out_count];

        for (size_t i = 0; i < mesh_data.size(); ++i) {
            const auto& src = mesh_data[i];
            CMeshData& dst = (*out_meshes)[i];

            ConvertMeshDataToCFormat(src, dst);

            dst.collision_type = collision_complexity;

            if (collision_complexity != COLLISION_NONE) {
                anari_usd_middleware::CollisionData collisionData;

                anari_usd_middleware::ECollisionComplexity complexity =
                    static_cast<anari_usd_middleware::ECollisionComplexity>(collision_complexity);

                std::vector<float> flatPoints;
                flatPoints.reserve(src.points.size() * 3);
                for (const auto& p : src.points) {
                    flatPoints.push_back(p.x);
                    flatPoints.push_back(p.y);
                    flatPoints.push_back(p.z);
                }

                bool collisionResult = g_collision_processor->generateCollision(
                    flatPoints, src.indices, complexity, collisionData);

                if (collisionResult && collisionData.isValid()) {
                    dst.collision_vertices_count = collisionData.vertices.size();
                    if (dst.collision_vertices_count > 0) {
                        dst.collision_vertices = new float[dst.collision_vertices_count];
                        std::memcpy(dst.collision_vertices, collisionData.vertices.data(),
                                   dst.collision_vertices_count * sizeof(float));
                    }

                    dst.collision_indices_count = collisionData.indices.size();
                    if (dst.collision_indices_count > 0) {
                        dst.collision_indices = new unsigned int[dst.collision_indices_count];
                        std::memcpy(dst.collision_indices, collisionData.indices.data(),
                                   dst.collision_indices_count * sizeof(unsigned int));

                        std::cout << "🔍 COLLISION COPY: vertices=" << dst.collision_vertices_count
                                  << " indices=" << dst.collision_indices_count
                                  << " ptr=" << (void*)dst.collision_vertices << std::endl;
                    }

                    dst.bounding_box_min[0] = collisionData.boundingBoxMin.x;
                    dst.bounding_box_min[1] = collisionData.boundingBoxMin.y;
                    dst.bounding_box_min[2] = collisionData.boundingBoxMin.z;

                    dst.bounding_box_max[0] = collisionData.boundingBoxMax.x;
                    dst.bounding_box_max[1] = collisionData.boundingBoxMax.y;
                    dst.bounding_box_max[2] = collisionData.boundingBoxMax.z;

                    dst.sphere_center[0] = collisionData.sphereCenter.x;
                    dst.sphere_center[1] = collisionData.sphereCenter.y;
                    dst.sphere_center[2] = collisionData.sphereCenter.z;
                    dst.sphere_radius = collisionData.sphereRadius;

                } else {
                    dst.collision_vertices = nullptr;
                    dst.collision_indices = nullptr;
                    dst.collision_vertices_count = 0;
                    dst.collision_indices_count = 0;

                    for (int j = 0; j < 3; j++) {
                        dst.bounding_box_min[j] = 0.0f;
                        dst.bounding_box_max[j] = 0.0f;
                        dst.sphere_center[j] = 0.0f;
                    }
                    dst.sphere_radius = 0.0f;
                }
            } else {
                dst.collision_vertices = nullptr;
                dst.collision_indices = nullptr;
                dst.collision_vertices_count = 0;
                dst.collision_indices_count = 0;

                for (int j = 0; j < 3; j++) {
                    dst.bounding_box_min[j] = 0.0f;
                    dst.bounding_box_max[j] = 0.0f;
                    dst.sphere_center[j] = 0.0f;
                }
                dst.sphere_radius = 0.0f;
            }
        }

        return 1;

    } catch (...) {
        *out_count = 0;
        *out_meshes = nullptr;
        return 0;
    }
}

int LoadUSDFromDiskWithCollision_C(const char* filepath,
                                   int collision_complexity,
                                   CMeshData** out_meshes,
                                   size_t* out_count) {
    if (!g_middleware || !filepath || !out_meshes || !out_count) {
        return 0;
    }

    try {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            *out_count = 0;
            *out_meshes = nullptr;
            return 0;
        }

        auto fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<unsigned char> buffer(fileSize);
        file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
        file.close();

        std::filesystem::path path(filepath);
        std::string filename = path.filename().string();

        return LoadUSDBufferWithCollision_C(buffer.data(), buffer.size(),
                                           filename.c_str(), collision_complexity,
                                           out_meshes, out_count);
    } catch (...) {
        *out_count = 0;
        *out_meshes = nullptr;
        return 0;
    }
}


int SetDefaultCollisionComplexity_C(int collision_complexity) {
    if (collision_complexity >= COLLISION_NONE && collision_complexity <= COLLISION_CONVEX_DECOMP) {
        g_default_collision_complexity = collision_complexity;
        return 1;
    }
    return 0;
}

const char* GetCollisionComplexityName_C(int collision_complexity) {
    static std::string name;

    if (!g_collision_processor) {
        g_collision_processor = std::make_unique<anari_usd_middleware::CollisionProcessor>();
    }

    auto complexity = static_cast<anari_usd_middleware::ECollisionComplexity>(collision_complexity);
    name = anari_usd_middleware::CollisionProcessor::getComplexityName(complexity);
    return name.c_str();
}

int SetCollisionParameters_C(float simplification_ratio,
                            float convex_hull_precision,
                            int max_convex_hulls) {
    if (!g_collision_processor) {
        g_collision_processor = std::make_unique<anari_usd_middleware::CollisionProcessor>();
    }

    try {
        if (simplification_ratio > 0.0f && simplification_ratio < 1.0f) {
            g_collision_processor->setSimplificationRatio(simplification_ratio);
        }

        if (convex_hull_precision > 0.0f && convex_hull_precision < 1.0f) {
            g_collision_processor->setConvexHullPrecision(convex_hull_precision);
        }

        if (max_convex_hulls > 0 && max_convex_hulls <= 64) {
            g_collision_processor->setMaxConvexHulls(max_convex_hulls);
        }

        return 1;
    } catch (...) {
        return 0;
    }
}


CTextureData CreateTextureFromBuffer_C(const unsigned char* buffer, size_t buffer_size) {
    CTextureData result = {};

    if (!g_middleware || !buffer) {
        return result;
    }

    try {
        std::vector<unsigned char> std_buffer(buffer, buffer + buffer_size);

        anari_usd_middleware::TextureData tex_data =
            g_middleware->CreateTextureFromBuffer(std_buffer);

        result.width = tex_data.width;
        result.height = tex_data.height;
        result.channels = tex_data.channels;
        result.data_size = tex_data.data.size();

        if (result.data_size > 0) {
            result.data = new unsigned char[result.data_size];
            std::memcpy(result.data, tex_data.data.data(), result.data_size);
        }

    } catch (...) {
    }

    return result;
}

int WriteGradientLineAsPNG_C(const unsigned char* buffer, size_t buffer_size, const char* output_path) {
    if (!g_middleware || !buffer || !output_path) {
        return 0;
    }

    try {
        std::vector<unsigned char> std_buffer(buffer, buffer + buffer_size);
        std::string std_path(output_path);

        return g_middleware->WriteGradientLineAsPNG(std_buffer, std_path) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int GetGradientLineAsPNGBuffer_C(const unsigned char* buffer, size_t buffer_size,
                                 unsigned char** out_buffer, size_t* out_size) {
    if (!g_middleware || !buffer || !out_buffer || !out_size || buffer_size == 0) {
        if (out_buffer) *out_buffer = nullptr;
        if (out_size) *out_size = 0;
        return 0;
    }

    *out_buffer = nullptr;
    *out_size = 0;

    try {
        std::vector<unsigned char> std_buffer(buffer, buffer + buffer_size);
        std::vector<unsigned char> png_buffer;

        bool result = g_middleware->GetGradientLineAsPNGBuffer(std_buffer, png_buffer);
        if (result && !png_buffer.empty()) {
            *out_size = png_buffer.size();
            *out_buffer = new unsigned char[*out_size];
            std::memcpy(*out_buffer, png_buffer.data(), *out_size);
            return 1;
        }
        else {
            return 0;
        }
    }
    catch (const std::exception& e) {
        return 0;
    }
    catch (...) {
        return 0;
    }
}

int GetImageRowAsPNGBuffer_C(const unsigned char* buffer, size_t buffer_size,
                             int row_index, unsigned char** out_buffer, size_t* out_size) {
    if (!buffer || !out_buffer || !out_size || row_index < 0 || buffer_size < 30) {
        if (out_buffer) *out_buffer = nullptr;
        if (out_size) *out_size = 0;
        return 0;
    }

    *out_buffer = nullptr;
    *out_size = 0;

    int width = 0, height = 0, channels = 0;
    int dim_result = GetPNGDimensions_C(buffer, buffer_size, &width, &height, &channels);

    if (dim_result == 0 || width <= 0 || height <= 0 || channels <= 0) {
        return 0;
    }

    if (row_index >= height) {
        return 0;
    }


    if (row_index == 0) {
        return GetGradientLineAsPNGBuffer_C(buffer, buffer_size, out_buffer, out_size);
    }
    else {
        return 0;
    }
}

int GetPNGDimensions_C(const unsigned char* buffer, size_t buffer_size,
                       int* out_width, int* out_height, int* out_channels) {
    if (!buffer || !out_width || !out_height || !out_channels || buffer_size < 30) {
        *out_width = 0;
        *out_height = 0;
        *out_channels = 0;
        return 0;
    }

    const unsigned char png_signature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(buffer, png_signature, 8) != 0) {
        *out_width = 0;
        *out_height = 0;
        *out_channels = 0;
        return 0;
    }

    if (std::memcmp(buffer + 12, "IHDR", 4) != 0) {
        *out_width = 0;
        *out_height = 0;
        *out_channels = 0;
        return 0;
    }

    try {
        // Read width (bytes 16-19, big-endian)

        *out_width = (buffer[16] << 24) | (buffer[17] << 16) | (buffer[18] << 8) | buffer[19];

        // Read height (bytes 20-23, big-endian)

        *out_height = (buffer[20] << 24) | (buffer[21] << 16) | (buffer[22] << 8) | buffer[23];

        unsigned char color_type = buffer[25];
        switch (color_type) {
            case 0:  // Grayscale
                *out_channels = 1;
                break;
            case 2:  // RGB
                *out_channels = 3;
                break;
            case 3:  // Palette
                *out_channels = 1;  // Indexed, but we'll treat as 1 channel
                break;
            case 4:  // Grayscale + Alpha
                *out_channels = 2;
                break;
            case 6:  // RGBA
                *out_channels = 4;
                break;
            default:
                *out_channels = 0;
                return 0;
        }

        if (*out_width <= 0 || *out_height <= 0 || *out_channels <= 0) {
            *out_width = 0;
            *out_height = 0;
            *out_channels = 0;
            return 0;
        }

        return 1;
    } catch (...) {
        *out_width = 0;
        *out_height = 0;
        *out_channels = 0;
        return 0;
    }
}


const char* GetMiddlewareVersion_C() {
    static const char* version = "AnariUsdMiddleware v2.0.0 with Collision Support";
    return version;
}

int ValidateUSDFormat_C(const unsigned char* buffer, size_t buffer_size, const char* filename) {
    if (!buffer || !filename || buffer_size == 0) {
        return 0;
    }

    try {
        std::string content(reinterpret_cast<const char*>(buffer),
                           (std::min)(buffer_size, static_cast<size_t>(1000)));

        return (content.find("#usda") != std::string::npos ||
                content.find("PXR-USDC") != std::string::npos ||
                content.find("def ") != std::string::npos ||
                content.find("over ") != std::string::npos) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

const char* GetSupportedUSDExtensions_C() {
    static const char* extensions = ".usd,.usda,.usdc,.usdz";
    return extensions;
}


void FreeMeshData_C(CMeshData* meshes, size_t count) {
    if (!meshes) return;

    for (size_t i = 0; i < count; ++i) {
        if (meshes[i].points) delete[] meshes[i].points;
        if (meshes[i].indices) delete[] meshes[i].indices;
        if (meshes[i].normals) delete[] meshes[i].normals;
        if (meshes[i].uvs) delete[] meshes[i].uvs;
        if (meshes[i].vertex_colors) delete[] meshes[i].vertex_colors;

        if (meshes[i].collision_vertices) delete[] meshes[i].collision_vertices;
        if (meshes[i].collision_indices) delete[] meshes[i].collision_indices;
    }

    delete[] meshes;
}

void FreeTextureData_C(CTextureData* texture) {
    if (texture && texture->data) {
        delete[] texture->data;
        texture->data = nullptr;
        texture->data_size = 0;
    }
}

void FreeBuffer_C(unsigned char* buffer) {
    delete[] buffer;
}

void FreeFileData_C(CFileData* file_data) {
    if (file_data && file_data->data) {
        delete[] file_data->data;
        file_data->data = nullptr;
        file_data->data_size = 0;
    }
}

void FreeFrameFiles_C(CFileData* files, size_t count) {
    if (!files) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        if (files[i].data) {
            delete[] files[i].data;
            files[i].data = nullptr;
            files[i].data_size = 0;
        }
    }
    delete[] files;
}


void RegisterUpdateCallback_C(FileReceivedCallback_C callback) {
    g_file_callback.store(callback, std::memory_order_release);
}

void RegisterUpdateCallbackSpan_C(FileReceivedSpanCallback_C callback) {
    g_file_span_callback.store(callback, std::memory_order_release);
}

void RegisterMessageCallback_C(MessageReceivedCallback_C callback) {
    g_message_callback.store(callback, std::memory_order_release);
}

void RegisterNotificationCallback_C(NotificationCallback_C callback) {
    if (!g_middleware) {
        MIDDLEWARE_LOG_WARNING("RegisterNotificationCallback_C: g_middleware is NULL!");
        return;
    }
    if (callback) {
        MIDDLEWARE_LOG_INFO("RegisterNotificationCallback_C: registering notification callback (V2-aware)");
        g_middleware->setNotificationCallback([callback](uint32_t messageType, int32_t sourceRank,
                                                             const std::string& filename,
                                                             uint64_t fileSize, uint64_t timestamp,
                                                             uint64_t hashLo, uint64_t hashHi,
                                                             uint64_t hashPrevLo, uint64_t hashPrevHi,
                                                             bool hasOldData) {
            callback(messageType, sourceRank, filename.c_str(), fileSize, timestamp,
                     hashLo, hashHi, hashPrevLo, hashPrevHi, hasOldData);
        });
    } else {
        MIDDLEWARE_LOG_INFO("RegisterNotificationCallback_C: clearing notification callback");
        g_middleware->setNotificationCallback(nullptr);
    }
}

void RegisterSceneUpdateCallback_C(SceneUpdateCallback_C callback) {
    if (!g_middleware) {
        MIDDLEWARE_LOG_WARNING("RegisterSceneUpdateCallback_C: g_middleware is NULL!");
        return;
    }
    if (callback) {
        MIDDLEWARE_LOG_INFO("RegisterSceneUpdateCallback_C: registering typed scene/property callback");
        g_middleware->setSceneUpdateCallback([callback](uint32_t messageType,
                                                         int32_t sourceRank,
                                                         uint64_t timestamp,
                                                         uint64_t commitId,
                                                         uint64_t revision,
                                                         const std::string& primPath,
                                                         const std::string& propertyName,
                                                         int32_t changeType,
                                                         int32_t valueType,
                                                         int64_t intValue,
                                                         float floatValue,
                                                         const float* vec4,
                                                         const std::string& stringValue,
                                                         uint32_t payloadSize) {
            callback(messageType,
                     sourceRank,
                     timestamp,
                     commitId,
                     revision,
                     primPath.c_str(),
                     propertyName.c_str(),
                     changeType,
                     valueType,
                     intValue,
                     floatValue,
                     vec4,
                     stringValue.c_str(),
                     payloadSize);
        });
    } else {
        MIDDLEWARE_LOG_INFO("RegisterSceneUpdateCallback_C: clearing typed scene/property callback");
        g_middleware->setSceneUpdateCallback(nullptr);
    }
}

void RegisterProtocolDiagnosticsCallback_C(ProtocolDiagnosticsCallback_C callback) {
    if (!g_middleware) {
        MIDDLEWARE_LOG_WARNING("RegisterProtocolDiagnosticsCallback_C: g_middleware is NULL!");
        return;
    }
    if (callback) {
        MIDDLEWARE_LOG_INFO("RegisterProtocolDiagnosticsCallback_C: registering protocol diagnostics callback");
        g_middleware->setProtocolDiagnosticsCallback([callback](const std::string& event,
                                                                 const std::string& message,
                                                                 uint64_t value0,
                                                                 uint64_t value1) {
            callback(event.c_str(), message.c_str(), value0, value1);
        });
    } else {
        MIDDLEWARE_LOG_INFO("RegisterProtocolDiagnosticsCallback_C: clearing protocol diagnostics callback");
        g_middleware->setProtocolDiagnosticsCallback(nullptr);
    }
}

uint32_t GetProtocolVersion_C(void) {
    if (!g_middleware) {
        return anari_usd_protocol::ANARI_USD_PROTOCOL_VERSION;
    }
    return g_middleware->getProtocolVersion();
}


void RequestTotalWorkerCountAsync_C(
    WorkerCountCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms) {

    if (!g_middleware || !g_middleware->isBrokerConnected()) {
        if (error_callback) {
            error_callback("Broker not connected");
        }
        return;
    }

    std::thread([callback, error_callback, timeout_ms]() {
        if (!g_middleware) {
            if (error_callback) error_callback("Middleware shut down");
            return;
        }
        uint32_t total_count = 0;
        bool success = g_middleware->requestTotalWorkerCount(total_count, timeout_ms);

        if (success && callback) {
            callback(total_count);
        } else if (error_callback) {
            error_callback(success ? "Unknown error" : "Failed to retrieve total worker count");
        }
    }).detach();
}

void RequestWorkerCountAsync_C(
    WorkerCountCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms) {

    if (!g_middleware || !g_middleware->isBrokerConnected()) {
        if (error_callback) {
            error_callback("Broker not connected");
        }
        return;
    }

    std::thread([callback, error_callback, timeout_ms]() {
        if (!g_middleware) {
            if (error_callback) error_callback("Middleware shut down");
            return;
        }
        uint32_t worker_count = 0;
        bool success = g_middleware->requestWorkerCount(worker_count, timeout_ms);

        if (success && callback) {
            callback(worker_count);
        } else if (error_callback) {
            error_callback(success ? "Unknown error" : "Failed to retrieve worker count");
        }
    }).detach();
}

void RequestWorkerStatusAsync_C(
    int32_t target_rank,
    WorkerStatusCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms) {

    if (!g_middleware || !g_middleware->isBrokerConnected()) {
        if (error_callback) {
            error_callback("Broker not connected");
        }
        return;
    }

    std::thread([target_rank, callback, error_callback, timeout_ms]() {
        if (!g_middleware) {
            if (error_callback) error_callback("Middleware shut down");
            return;
        }
        std::vector<std::tuple<int32_t, uint32_t, std::string, std::string, uint64_t>> worker_status;
        bool success = g_middleware->requestWorkerStatus(target_rank, worker_status, timeout_ms);

        if (success && callback) {
            std::string status_data;
            for (const auto& status : worker_status) {
                status_data += std::to_string(std::get<0>(status)) + ":";
                status_data += std::to_string(std::get<1>(status)) + ":";
                status_data += std::get<2>(status) + ":";
                status_data += std::get<3>(status) + ":";
                status_data += std::to_string(std::get<4>(status)) + ";";
            }
            callback(target_rank, status_data.c_str(), status_data.size());
        } else if (error_callback) {
            error_callback(success ? "Unknown error" : "Failed to retrieve worker status");
        }
    }).detach();
}

void RequestFileListAsync_C(
    int32_t target_rank,
    FileListCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms) {

    if (!g_middleware || !g_middleware->isBrokerConnected()) {
        if (error_callback) {
            error_callback("Broker not connected");
        }
        return;
    }

    std::thread([target_rank, callback, error_callback, timeout_ms]() {
        if (!g_middleware) {
            if (error_callback) error_callback("Middleware shut down");
            return;
        }
        std::vector<std::string> files;
        bool success = g_middleware->requestFileList(target_rank, files, timeout_ms);
        if (success) dedup_usd_filenames_strings(files);

        if (success && callback) {
            char** file_array = new char*[files.size()];
            for (size_t i = 0; i < files.size(); i++) {
                file_array[i] = strdup(files[i].c_str());
            }
            callback(target_rank, file_array, files.size());

            for (size_t i = 0; i < files.size(); i++) {
                free(file_array[i]);
            }
            delete[] file_array;
        } else if (error_callback) {
            error_callback(success ? "Unknown error" : "Failed to retrieve file list");
        }
    }).detach();
}

void RequestFilesParallelAsync_C(
    const char** filenames,
    size_t filename_count,
    const int32_t* target_ranks,
    ParallelFileReceivedCallback_C file_received_callback,
    ParallelDownloadCompleteCallback_C completion_callback,
    ParallelDownloadErrorCallback_C error_callback,
    int timeout_ms) {

    #ifdef _WIN32
    OutputDebugStringA("=== JUSYNC DEBUG: RequestFilesParallelAsync_C ENTER ===\n");

    HMODULE hModule = GetModuleHandle(TEXT("anari_usd_middleware.dll"));
    if (hModule) {
        char path[MAX_PATH];
        GetModuleFileNameA(hModule, path, MAX_PATH);
        char debugMsg[512];
        sprintf(debugMsg, "=== JUSYNC DEBUG: DLL LOADED FROM: %s ===\n", path);
        OutputDebugStringA(debugMsg);
        MIDDLEWARE_LOG_INFO("=== DLL LOADED FROM: %s ===", path);
    } else {
        OutputDebugStringA("=== JUSYNC DEBUG: DLL NOT LOADED ===\n");
        MIDDLEWARE_LOG_ERROR("=== DLL NOT LOADED ===");
    }
    #endif

    char countMsg[256];
    sprintf(countMsg, "=== JUSYNC DEBUG: Filename count: %zu ===\n", filename_count);
    #ifdef _WIN32
    OutputDebugStringA(countMsg);
    #endif

    MIDDLEWARE_LOG_INFO("=== RequestFilesParallelAsync_C ENTER ===");
    MIDDLEWARE_LOG_INFO("Filename count: %zu", filename_count);

    if (!g_middleware) {
        MIDDLEWARE_LOG_ERROR("g_middleware is NULL!");
        if (error_callback) {
            error_callback("", "Middleware not initialized");
        }
        return;
    }

    if (!g_middleware->isBrokerConnected()) {
        MIDDLEWARE_LOG_ERROR("Broker not connected");
        if (error_callback) {
            error_callback("", "Broker not connected");
        }
        return;
    }

    if (filename_count == 0) {
        if (error_callback) {
            error_callback("", "Empty filename list");
        }
        return;
    }

    std::map<std::string, int32_t> uniqueFiles;
    std::vector<std::string> filename_vec;
    std::vector<int32_t> target_ranks_vec;

    for (size_t i = 0; i < filename_count; i++) {
        std::string fname = filenames[i] ? filenames[i] : "";
        if (fname.empty()) continue;
        if (uniqueFiles.find(fname) == uniqueFiles.end()) {
            uniqueFiles[fname] = target_ranks[i];
            filename_vec.push_back(fname);
            target_ranks_vec.push_back(target_ranks[i]);
        }
    }
    if (filename_vec.size() < filename_count) {
        MIDDLEWARE_LOG_INFO("Parallel download deduplication: %zu → %zu files (removed %zu duplicates)",
            filename_count, filename_vec.size(), filename_count - filename_vec.size());
    }

    std::function<void(const std::string&, const std::vector<uint8_t>&)> cpp_file_callback = nullptr;
    if (file_received_callback) {
        cpp_file_callback = [file_received_callback](const std::string& filename, const std::vector<uint8_t>& data) {
            file_received_callback(filename.c_str(), data.data(), data.size());
        };
    }

    std::function<void()> cpp_completion_callback = nullptr;
    if (completion_callback) {
        cpp_completion_callback = [completion_callback]() {
            completion_callback();
        };
    }

    std::function<void(const std::string&, const std::string&)> cpp_error_callback = nullptr;
    if (error_callback) {
        cpp_error_callback = [error_callback](const std::string& filename, const std::string& error_msg) {
            error_callback(filename.c_str(), error_msg.c_str());
        };
    }

    try {
        if (!g_middleware) {
            MIDDLEWARE_LOG_ERROR("RequestFilesParallelAsync_C: g_middleware is NULL!");
            if (error_callback) {
                error_callback("", "Middleware not initialized");
            }
            return;
        }

        MIDDLEWARE_LOG_INFO("RequestFilesParallelAsync_C: Calling requestFilesParallelAsync with %zu files", filename_count);

        #ifdef _WIN32
        OutputDebugStringA("[ANARI] RequestFilesParallelAsync_C: About to call C++ API\n");
        #endif

        g_middleware->requestFilesParallelAsync(
            filename_vec,
            target_ranks_vec,
            timeout_ms,
            cpp_file_callback,
            cpp_completion_callback,
            cpp_error_callback);

        MIDDLEWARE_LOG_INFO("RequestFilesParallelAsync_C: Successfully called requestFilesParallelAsync");

        #ifdef _WIN32
        OutputDebugStringA("[ANARI] RequestFilesParallelAsync_C: C++ API call completed\n");
        #endif
    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("RequestFilesParallelAsync_C: Exception: %s", e.what());
        if (error_callback) {
            std::string error_msg = std::string("Exception: ") + e.what();
            error_callback("", error_msg.c_str());
        }
    } catch (...) {
        MIDDLEWARE_LOG_ERROR("RequestFilesParallelAsync_C: Unknown exception");
        if (error_callback) {
            error_callback("", "Unknown exception");
        }
    }
}

ANARI_USD_MIDDLEWARE_C_API int VerifyParallelDownloadDLL_C() {
    #ifdef _WIN32
    OutputDebugStringA("=== JUSYNC DEBUG: VerifyParallelDownloadDLL_C called ===\n");
    #endif

    MIDDLEWARE_LOG_INFO("=== VerifyParallelDownloadDLL_C ===");

    if (!g_middleware) {
        MIDDLEWARE_LOG_ERROR("g_middleware is NULL");
        return 0;
    }

    if (!g_middleware->isBrokerConnected()) {
        MIDDLEWARE_LOG_ERROR("Broker not connected");
        return 0;
    }

    MIDDLEWARE_LOG_INFO("DLL verification PASSED");
    return 1;
}

ANARI_USD_MIDDLEWARE_C_API int RequestFilesParallelDirect_C(
    const char** filenames,
    size_t filename_count,
    const int32_t* target_ranks,
    ParallelFileReceivedCallback_C file_received_callback,
    ParallelDownloadCompleteCallback_C completion_callback,
    ParallelDownloadErrorCallback_C error_callback,
    int timeout_ms) {

    #ifdef _WIN32
    OutputDebugStringA("[ANARI] RequestFilesParallelDirect_C: Entering direct C API\n");
    #endif

    if (!g_middleware) {
        #ifdef _WIN32
        OutputDebugStringA("[ANARI] RequestFilesParallelDirect_C: g_middleware is NULL!\n");
        #endif
        return 0;
    }

    if (!filenames || filename_count == 0) {
        #ifdef _WIN32
        OutputDebugStringA("[ANARI] RequestFilesParallelDirect_C: Invalid parameters\n");
        #endif
        return 0;
    }

    auto client = g_middleware->getClient();
    if (!client) {
        #ifdef _WIN32
        OutputDebugStringA("[ANARI] RequestFilesParallelDirect_C: Client is NULL!\n");
        #endif
        return 0;
    }

    std::vector<std::string> filename_vec;
    std::vector<int32_t> target_ranks_vec;

    filename_vec.reserve(filename_count);
    target_ranks_vec.reserve(filename_count);

    for (size_t i = 0; i < filename_count; i++) {
        filename_vec.push_back(filenames[i] ? filenames[i] : "");
        target_ranks_vec.push_back(target_ranks ? target_ranks[i] : -1);
    }

    std::function<void(const std::string&, const std::vector<uint8_t>&)> cpp_file_callback = nullptr;
    if (file_received_callback) {
        cpp_file_callback = [file_received_callback](const std::string& filename, const std::vector<uint8_t>& data) {
            file_received_callback(filename.c_str(), data.data(), data.size());
        };
    }

    std::function<void()> cpp_completion_callback = nullptr;
    if (completion_callback) {
        cpp_completion_callback = [completion_callback]() {
            completion_callback();
        };
    }

    std::function<void(const std::string&, const std::string&)> cpp_error_callback = nullptr;
    if (error_callback) {
        cpp_error_callback = [error_callback](const std::string& filename, const std::string& error_msg) {
            error_callback(filename.c_str(), error_msg.c_str());
        };
    }

    #ifdef _WIN32
    char debug_msg[256];
    snprintf(debug_msg, sizeof(debug_msg), "[ANARI] RequestFilesParallelDirect_C: Calling client->requestFilesParallel with %zu files\n",
             filename_count);
    OutputDebugStringA(debug_msg);
    #endif

    // Call the client directly (synchronous within this thread)

    try {
        bool success = client->requestFilesParallel(
            filename_vec,
            target_ranks_vec,
            cpp_file_callback,
            cpp_completion_callback,
            cpp_error_callback,
            timeout_ms);

        #ifdef _WIN32
        OutputDebugStringA(success ?
            "[ANARI] RequestFilesParallelDirect_C: Success!\n" :
            "[ANARI] RequestFilesParallelDirect_C: Failed!\n");
        #endif

        return success ? 1 : 0;
    } catch (const std::exception& e) {
        #ifdef _WIN32
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg), "[ANARI] RequestFilesParallelDirect_C: Exception: %s\n", e.what());
        OutputDebugStringA(error_msg);
        #endif
        return 0;
    } catch (...) {
        #ifdef _WIN32
        OutputDebugStringA("[ANARI] RequestFilesParallelDirect_C: Unknown exception\n");
        #endif
        return 0;
    }
}


static void ConvertPointCloudDataToCFormat(const anari_usd_middleware::UsdProcessor::PointCloudData& src,
                                           CPointCloudData& dst) {
    memset(&dst, 0, sizeof(CPointCloudData));

    #ifdef _WIN32
    strncpy_s(dst.element_name, sizeof(dst.element_name), src.elementName.c_str(), _TRUNCATE);
    strncpy_s(dst.type_name, sizeof(dst.type_name), src.typeName.c_str(), _TRUNCATE);
    #else
    snprintf(dst.element_name, sizeof(dst.element_name), "%s", src.elementName.c_str());
    snprintf(dst.type_name, sizeof(dst.type_name), "%s", src.typeName.c_str());
    #endif

    dst.points_count = src.positions.size();

    if (dst.points_count > 0) {
        dst.positions = new float[dst.points_count * 3];
        for (size_t i = 0; i < dst.points_count; ++i) {
            dst.positions[i * 3 + 0] = src.positions[i].x;
            dst.positions[i * 3 + 1] = src.positions[i].y;
            dst.positions[i * 3 + 2] = src.positions[i].z;
        }
    }

    if (!src.vertex_colors.empty()) {
        dst.has_colors = 1;
        dst.colors = new float[src.vertex_colors.size() * 4];
        for (size_t i = 0; i < src.vertex_colors.size(); ++i) {
            dst.colors[i * 4 + 0] = src.vertex_colors[i].r;
            dst.colors[i * 4 + 1] = src.vertex_colors[i].g;
            dst.colors[i * 4 + 2] = src.vertex_colors[i].b;
            dst.colors[i * 4 + 3] = src.vertex_colors[i].a;
        }
        dst.points_count = std::max(dst.points_count, src.vertex_colors.size());
    }

    if (!src.normals.empty()) {
        dst.has_normals = 1;
        dst.normals = new float[src.normals.size() * 3];
        for (size_t i = 0; i < src.normals.size(); ++i) {
            dst.normals[i * 3 + 0] = src.normals[i].x;
            dst.normals[i * 3 + 1] = src.normals[i].y;
            dst.normals[i * 3 + 2] = src.normals[i].z;
        }
    }

    if (!src.widths.empty()) {
        dst.has_widths = 1;
        dst.widths = new float[src.widths.size()];
        std::memcpy(dst.widths, src.widths.data(), src.widths.size() * sizeof(float));
    }
    else if (!src.scalarAttributes.empty() && dst.points_count > 0)
    {
        size_t wcount = std::min(src.scalarAttributes.size(), dst.points_count);
        dst.has_widths = 1;
        dst.widths = new float[wcount];
        for (size_t i = 0; i < wcount; ++i) {
            dst.widths[i] = src.scalarAttributes[i].x;
        }
        MIDDLEWARE_LOG_INFO("Falling back to scalarAttributes.x for widths (%zu values)", wcount);
    }

    for (int i = 0; i < 3; ++i) {
        dst.bounding_box_min[i] = 0.0f;
        dst.bounding_box_max[i] = 0.0f;
    }
    if (dst.points_count > 0) {
        for (size_t i = 0; i < dst.points_count; ++i) {
            for (int j = 0; j < 3; ++j) {
                dst.bounding_box_min[j] = fminf(dst.bounding_box_min[j], dst.positions[i * 3 + j]);
                dst.bounding_box_max[j] = fmaxf(dst.bounding_box_max[j], dst.positions[i * 3 + j]);
            }
        }
    }
}

int ProcessPointCloudFromUSD_C(const unsigned char* buffer,
                               size_t buffer_size,
                               const char* filename,
                               CPointCloudData** out_clouds,
                               size_t* out_count) {
    if (!buffer || !filename || !out_clouds || !out_count) {
        return 0;
    }

    try {
        std::vector<uint8_t> std_buffer(buffer, buffer + buffer_size);
        std::string std_filename(filename);

        anari_usd_middleware::UsdProcessor& processor = GetThreadLocalUsdProcessor();
        std::vector<anari_usd_middleware::UsdProcessor::PointCloudData> pc_data;

        std::vector<anari_usd_middleware::UsdProcessor::MeshData> dummyMeshes;
        bool result = processor.LoadUSDBuffer(std_buffer, std_filename, dummyMeshes, &pc_data);

        if (!result || pc_data.empty()) {
            if (result && !dummyMeshes.empty() && pc_data.empty()) {
                *out_count = 0;
                *out_clouds = nullptr;
                return 1; // Success but no point clouds
            }
            *out_count = 0;
            *out_clouds = nullptr;
            return 0;
        }

        *out_count = pc_data.size();
        *out_clouds = new CPointCloudData[*out_count];

        for (size_t i = 0; i < pc_data.size(); ++i) {
            ConvertPointCloudDataToCFormat(pc_data[i], (*out_clouds)[i]);
        }

        MIDDLEWARE_LOG_INFO("Extracted %zu point clouds from '%s'", *out_count, std_filename.c_str());
        for (size_t i = 0; i < pc_data.size(); ++i) {
            MIDDLEWARE_LOG_INFO("  PointCloud[%zu]: '%s' (%zu points, colors=%d, normals=%d)",
                               i, (*out_clouds)[i].element_name, (*out_clouds)[i].points_count,
                               (*out_clouds)[i].has_colors, (*out_clouds)[i].has_normals);
        }

        return 1;

    } catch (...) {
        *out_count = 0;
        *out_clouds = nullptr;
        return 0;
    }
}

int LoadUSDFull_C(const unsigned char* buffer,
                  size_t buffer_size,
                  const char* filename,
                  CMeshData** out_meshes,
                  size_t* out_mesh_count,
                  CPointCloudData** out_clouds,
                  size_t* out_cloud_count) {
    return LoadUSDFullFromPointer_C(buffer, buffer_size, filename,
                                    out_meshes, out_mesh_count,
                                    out_clouds, out_cloud_count);
}

int LoadUSDFullFromPointer_C(const unsigned char* buffer,
                              size_t buffer_size,
                              const char* filename,
                              CMeshData** out_meshes,
                              size_t* out_mesh_count,
                              CPointCloudData** out_clouds,
                              size_t* out_cloud_count) {
    if (!buffer || !filename || !out_meshes || !out_mesh_count || !out_clouds || !out_cloud_count) {
        return 0;
    }

    try {
        std::string std_filename(filename);

        anari_usd_middleware::UsdProcessor& processor = GetThreadLocalUsdProcessor();
        std::vector<anari_usd_middleware::UsdProcessor::MeshData> mesh_data;
        std::vector<anari_usd_middleware::UsdProcessor::PointCloudData> pc_data;

        g_parse_semaphore.acquire();
        {
            ParseGuard guard;
            bool result = processor.LoadUSDBufferFromRaw(
                reinterpret_cast<const uint8_t*>(buffer), buffer_size,
                std_filename, mesh_data, &pc_data);

            if (!result) {
                MIDDLEWARE_LOG_ERROR("LoadUSDFullFromPointer_C: LoadUSDBufferFromRaw returned false for '%s' (size=%zu bytes)",
                    std_filename.c_str(), buffer_size);
                *out_mesh_count = 0;
                *out_meshes = nullptr;
                *out_cloud_count = 0;
                *out_clouds = nullptr;
                return 0;
            }
        } // semaphore released here

        {
            size_t valid_pc = 0, invalid_pc = 0;
            for (const auto& pc : pc_data) {
                if (pc.positions.size() > 0) valid_pc++;
                else invalid_pc++;
            }
            if (invalid_pc > 0) {
                MIDDLEWARE_LOG_ERROR("LoadUSDFullFromPointer_C: %zu point clouds have 0 positions for '%s' (valid=%zu, invalid=%zu)",
                    invalid_pc, std_filename.c_str(), valid_pc, invalid_pc);
            }
            if (valid_pc == 0 && invalid_pc > 0) {
                MIDDLEWARE_LOG_ERROR("LoadUSDFullFromPointer_C: no usable point clouds extracted — discarding for '%s'",
                    std_filename.c_str());
                pc_data.clear();
            }
        }

        if (mesh_data.empty() && pc_data.empty()) {
            MIDDLEWARE_LOG_WARNING("LoadUSDFullFromPointer_C: LoadUSDBufferFromRaw succeeded but returned 0 meshes + 0 PCs for '%s'",
                std_filename.c_str());
        }

        if (!mesh_data.empty()) {
            *out_mesh_count = mesh_data.size();
            *out_meshes = new CMeshData[*out_mesh_count];
            for (size_t i = 0; i < mesh_data.size(); ++i) {
                ConvertMeshDataToCFormat(mesh_data[i], (*out_meshes)[i]);
                (*out_meshes)[i].collision_type = COLLISION_NONE;
                (*out_meshes)[i].collision_vertices = nullptr;
                (*out_meshes)[i].collision_indices = nullptr;
                (*out_meshes)[i].collision_vertices_count = 0;
                (*out_meshes)[i].collision_indices_count = 0;
                for (int j = 0; j < 3; j++) {
                    (*out_meshes)[i].bounding_box_min[j] = 0.0f;
                    (*out_meshes)[i].bounding_box_max[j] = 0.0f;
                    (*out_meshes)[i].sphere_center[j] = 0.0f;
                }
                (*out_meshes)[i].sphere_radius = 0.0f;
            }
        } else {
            *out_mesh_count = 0;
            *out_meshes = nullptr;
        }

        if (!pc_data.empty()) {
            *out_cloud_count = pc_data.size();
            *out_clouds = new CPointCloudData[*out_cloud_count];
            for (size_t i = 0; i < pc_data.size(); ++i) {
                ConvertPointCloudDataToCFormat(pc_data[i], (*out_clouds)[i]);
            }
        } else {
            *out_cloud_count = 0;
            *out_clouds = nullptr;
        }

        MIDDLEWARE_LOG_INFO("LoadUSDFullFromPointer_C: extracted %zu meshes + %zu point clouds from '%s' in single pass (zero-copy)",
                            *out_mesh_count, *out_cloud_count, std_filename.c_str());
        return 1;
    } catch (...) {
        *out_mesh_count = 0;
        *out_meshes = nullptr;
        *out_cloud_count = 0;
        *out_clouds = nullptr;
        return 0;
    }
}

int GetCachedGradientTexture_C(unsigned char** gradient_png_data,
                               size_t* out_png_size,
                               int* out_width,
                               int* out_height) {
    if (!gradient_png_data || !out_png_size || !out_width || !out_height) {
        return 0;
    }

    try {
        if (!g_middleware) {
            return 0;
        }

        std::vector<uint8_t> outData;
        int w = 0, h = 0;
        bool result = g_middleware->GetCachedGradientTexture(outData, w, h);

        if (!result || outData.empty()) {
            *gradient_png_data = nullptr;
            *out_png_size = 0;
            *out_width = 0;
            *out_height = 0;
            return 0;
        }

        *gradient_png_data = new unsigned char[outData.size()];
        std::memcpy(*gradient_png_data, outData.data(), outData.size());
        *out_png_size = outData.size();
        *out_width = w;
        *out_height = h;
        return 1;

    } catch (...) {
        *gradient_png_data = nullptr;
        *out_png_size = 0;
        *out_width = 0;
        *out_height = 0;
        return 0;
    }
}

void FreePointCloudData_C(CPointCloudData* clouds, size_t count) {
    if (!clouds || count == 0) return;

    for (size_t i = 0; i < count; ++i) {
        delete[] clouds[i].positions;
        delete[] clouds[i].normals;
        delete[] clouds[i].colors;
        delete[] clouds[i].widths;
    }
    delete[] clouds;
}

void FreeCachedGradientTexture_C(unsigned char* gradient_rgba) {
    if (!gradient_rgba) return;
    delete[] gradient_rgba;
}


void JusyncDiagMark_C(const char* filename, int stage, uint64_t bytes,
                      uint64_t hashLo, uint64_t hashHi) {
    if (!filename) return;
    jusync_diag::Diagnostics& d = jusync_diag::Diagnostics::instance();
    d.mark(filename, stage, bytes, hashLo, hashHi);
    if (stage == jusync_diag::STAGE_SPAWNED) {
        const std::string line = d.summary(filename);
        if (!line.empty()) {
            MIDDLEWARE_LOG_INFO("[stream-diag] %s", line.c_str());
        }
    }
}

int JusyncDiagDump_C(char* outBuf, int outBufSize, int lastN) {
    if (!outBuf || outBufSize <= 0) return 0;
    const std::string table = jusync_diag::Diagnostics::instance().dump(
        lastN > 0 ? static_cast<size_t>(lastN) : 20);
    const int n = static_cast<int>(std::min<size_t>(table.size(),
                                                    static_cast<size_t>(outBufSize - 1)));
    std::memcpy(outBuf, table.data(), n);
    outBuf[n] = '\0';
    return n;
}

double JusyncDiagTotalMs_C(const char* filename) {
    if (!filename) return -1.0;
    return jusync_diag::Diagnostics::instance().totalMs(filename);
}

void JusyncDiagClear_C(void) {
    jusync_diag::Diagnostics::instance().clear();
}

void JusyncDiagSetEnabled_C(int enabled) {
    jusync_diag::Diagnostics::instance().setEnabled(enabled != 0);
}

} // extern "C"
