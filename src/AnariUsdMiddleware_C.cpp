// Define this before including headers to ensure proper export/import
#ifndef ANARI_USD_MIDDLEWARE_EXPORTS
#define ANARI_USD_MIDDLEWARE_EXPORTS
#endif

#include "AnariUsdMiddleware_C.h"
#include "AnariUsdMiddleware.h"
#include "AnariUsdClient.h"
#include "CollisionProcessor.h"
#include "UsdProcessor.h"
#include "AnariUsdMessages.h"

#include <memory>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <thread>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================================
// GLOBAL STATE MANAGEMENT
// ============================================================================

// Global middleware instance - ensures single instance per process
static std::unique_ptr<anari_usd_middleware::AnariUsdMiddleware> g_middleware;

// Global collision processor instance
static std::unique_ptr<anari_usd_middleware::CollisionProcessor> g_collision_processor;

// Global callback storage - maintains C callback function pointers
// Use atomic for thread-safe access from ZMQ callback threads
static std::atomic<FileReceivedCallback_C> g_file_callback = nullptr;
static std::atomic<MessageReceivedCallback_C> g_message_callback = nullptr;

// Default collision complexity setting
static int g_default_collision_complexity = COLLISION_COMPLEX;

// Global mutex for thread-safe access to g_middleware and g_collision_processor
static std::mutex g_middleware_mutex;

// ============================================================================
// C INTERFACE IMPLEMENTATION
// ============================================================================

extern "C" {

/**
 * Initialize the middleware with ZeroMQ endpoint
 * Sets up connection and registers callbacks if they were set before initialization
 */
int InitializeMiddleware_C(const char* endpoint) {
    try {
        // Log DLL version info for debugging
        #ifdef ENABLE_CUDA_ACCELERATION
        MIDDLEWARE_LOG_INFO("🔥🔥🔥 GPU ACCELERATION ENABLED IN DLL 🔥🔥🔥");
        #else
        MIDDLEWARE_LOG_INFO("⚠️⚠️⚠️ GPU ACCELERATION NOT COMPILED IN DLL ⚠️⚠️⚠️");
        #endif
        
        // Create middleware instance if not already created
        if (!g_middleware) {
            g_middleware = std::make_unique<anari_usd_middleware::AnariUsdMiddleware>();
        }

        // Initialize collision processor
        if (!g_collision_processor) {
            g_collision_processor = std::make_unique<anari_usd_middleware::CollisionProcessor>();
        }

        // Use provided endpoint or default fallback
        std::string endpoint_str = endpoint ? endpoint : "tcp://*:5556";
        bool result = g_middleware->initialize(endpoint_str.c_str());

        // CRITICAL FIX: Only register callbacks after successful initialization
        if (result) {
            // Register file callback if available
            if (g_file_callback) {
                g_middleware->registerUpdateCallback([](const anari_usd_middleware::FileData& file_data) {
                    if (g_file_callback) {
                        CFileData c_data = {};

                        // Safe string copying with bounds checking
                        // Use snprintf for cross-platform safety with guaranteed null termination
#ifdef _WIN32
                        strncpy_s(c_data.filename, sizeof(c_data.filename), file_data.filename.c_str(), _TRUNCATE);
                        strncpy_s(c_data.hash, sizeof(c_data.hash), file_data.hash.c_str(), _TRUNCATE);
                        strncpy_s(c_data.file_type, sizeof(c_data.file_type), file_data.fileType.c_str(), _TRUNCATE);
#else
                        // Use snprintf for guaranteed null termination and bounds checking
                        snprintf(c_data.filename, sizeof(c_data.filename), "%s", file_data.filename.c_str());
                        snprintf(c_data.hash, sizeof(c_data.hash), "%s", file_data.hash.c_str());
                        snprintf(c_data.file_type, sizeof(c_data.file_type), "%s", file_data.fileType.c_str());
#endif

                        // Copy binary data safely
                        c_data.data_size = file_data.data.size();
                        if (c_data.data_size > 0) {
                            c_data.data = new unsigned char[c_data.data_size];
                            std::memcpy(c_data.data, file_data.data.data(), c_data.data_size);
                        } else {
                            c_data.data = nullptr;
                        }

                        // Call the callback
                        FileReceivedCallback_C file_callback = g_file_callback.load(std::memory_order_acquire);
                        if (file_callback) {
                            file_callback(&c_data);
                        }
                        
                        // ✅ CRITICAL FIX: Clean up allocated memory after callback
                        // The callback should have copied any data it needs to keep
                        if (c_data.data) {
                            delete[] c_data.data;
                            c_data.data = nullptr;
                            c_data.data_size = 0;
                        }
                    }
                });
            }

            // Register message callback if available
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
        // Catch all exceptions to prevent crashes in C interface
        return 0;
    }
}

/**
 * Shutdown middleware and cleanup all resources
 * Safe to call multiple times
 */
void ShutdownMiddleware_C() {
    if (g_middleware) {
        g_middleware->shutdown();
        g_middleware.reset();
    }

    // ✅ NEW: Cleanup collision processor
    if (g_collision_processor) {
        g_collision_processor.reset();
    }

    // Clear callback pointers
    g_file_callback = nullptr;
    g_message_callback = nullptr;
}

/**
 * Check if middleware is connected and ready
 */
int IsConnected_C() {
    return (g_middleware && g_middleware->isConnected()) ? 1 : 0;
}

/**
 * Get current status information for debugging
 * Returns static string that remains valid until next call
 */
const char* GetStatusInfo_C() {
    static std::string status;
    if (g_middleware) {
        status = g_middleware->getStatusInfo();
        return status.c_str();
    }
    return "Middleware not initialized";
}

/**
 * Start the background receiver thread
 * Non-blocking operation
 */
int StartReceiving_C() {
    return (g_middleware && g_middleware->startReceiving()) ? 1 : 0;
}

/**
 * Stop the background receiver thread
 * Blocks until thread terminates safely
 */
void StopReceiving_C() {
    if (g_middleware) {
        g_middleware->stopReceiving();
    }
}

// ============================================================================
// ANARI USD DEALER CLIENT FUNCTIONS
// ============================================================================

/**
 * Connect to ANARI USD broker as DEALER client
 */
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

/**
 * Disconnect from ANARI USD broker
 */
void DisconnectFromBroker_C() {
    if (g_middleware) {
        g_middleware->disconnectFromBroker();
    }
}

/**
 * Check if connected to ANARI USD broker
 */
int IsBrokerConnected_C() {
    return (g_middleware && g_middleware->isBrokerConnected()) ? 1 : 0;
}

/**
 * Request total worker count INCLUDING rank 0
 * Uses requestTotalWorkerCount method which includes rank 0
 * Signature: int RequestTotalWorkerCount_C(uint32_t* out_total_count, int timeout_ms)
 */
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

/**
 * Request worker list via callback (avoids C++ ABI issues with std::vector<tuple>)
 * Signature: int RequestWorkerListStringCallback_C(WorkerListCallback_C callback, int timeout_ms)
 */
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
        
        // Convert to C arrays (stack-allocated for small lists, heap for large)
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

/**
 * Request worker list returning raw data buffer (synchronous, no callback needed)
 * Returns worker list as a flat string "rank:hostname:ip;rank:hostname:ip;..."
 * Signature: int RequestWorkerListString_C(uint32_t* out_worker_count, unsigned char** out_data, size_t* out_size, int timeout_ms)
 */
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

        // Build a flat string: "rank:hostname:ip;rank:hostname:ip;..."
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

/**
 * Request worker count EXCLUDING rank 0 (computational workers only)
 * Uses existing requestWorkerCount method
 * Signature: int RequestWorkerCountExcludingRank0_C(uint32_t* out_count, int timeout_ms)
 */
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

/**
 * Request list of available files from a specific rank
 */
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
        
        // Allocate C string array
        *out_count = files.size();
        *out_files = new char*[*out_count];
        
        // Copy each filename
        for (size_t i = 0; i < files.size(); ++i) {
            size_t len = files[i].length() + 1;
            (*out_files)[i] = new char[len];
#ifdef _WIN32
            strncpy_s((*out_files)[i], len, files[i].c_str(), _TRUNCATE);
#else
            // Use snprintf for guaranteed null termination
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

/**
 * Request file list with sizes from a specific rank
 */
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
        
        // Allocate C string array and size array
        *out_count = files.size();
        *out_names = new char*[*out_count];
        *out_sizes = new uint64_t[*out_count];
        
        // Copy each filename and size
        for (size_t i = 0; i < files.size(); ++i) {
            size_t len = files[i].name.length() + 1;
            (*out_names)[i] = new char[len];
#ifdef _WIN32
            strncpy_s((*out_names)[i], len, files[i].name.c_str(), _TRUNCATE);
#else
            // Use snprintf for guaranteed null termination
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

/**
 * Request file list with sizes and source ranks from worker rank(s)
 */
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
        
        // Allocate C arrays
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

/**
 * Request a specific file from a rank
 */
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
        
        // Allocate and copy file data
        *out_size = fileData.size();
        if (*out_size > 0) {
            // Validate size is reasonable (max 100GB to catch obviously wrong values)
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

/**
 * Request all files for a specific frame number
 */
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
        
        // Allocate C file data array
        *out_count = frameFiles.size();
        *out_files = new CFileData[*out_count];
        
        // Convert each file
        for (size_t i = 0; i < frameFiles.size(); ++i) {
            CFileData& c_file = (*out_files)[i];
            
            // Copy filename
#ifdef _WIN32
            strncpy_s(c_file.filename, sizeof(c_file.filename), frameFiles[i].first.c_str(), _TRUNCATE);
#else
            // Use snprintf for guaranteed null termination
            snprintf(c_file.filename, sizeof(c_file.filename), "%s", frameFiles[i].first.c_str());
#endif
            
            // Copy file data
            c_file.data_size = frameFiles[i].second.size();
            if (c_file.data_size > 0) {
                c_file.data = new unsigned char[c_file.data_size];
                std::memcpy(c_file.data, frameFiles[i].second.data(), c_file.data_size);
            } else {
                c_file.data = nullptr;
            }
            
            // Set default hash and file type (not available in this context)
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

/**
 * Free file list array allocated by RequestFileList_C
 */
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

// ============================================================================
// INTERNAL HELPER FUNCTIONS
// ============================================================================

// Thread-local storage for UV set names and subdivision schemes to ensure lifetime
static thread_local std::vector<std::string> g_subdivision_scheme_storage;
static thread_local std::vector<std::string> g_uv_name_storage;

/**
 * Helper function to convert UsdProcessor::MeshData to CMeshData
 * Handles all USD geometry features including subdivision, UV sets, etc.
 *
 * CRITICAL: src.points is std::vector<glm::vec3>, NOT std::vector<float>!
 *           Each vec3 becomes 3 floats in the output array.
 */
static void ConvertMeshDataToCFormat(const anari_usd_middleware::UsdProcessor::MeshData& src,
                                     CMeshData& dst) {
    // Initialize all pointers to null for safety
    dst.points = nullptr;
    dst.indices = nullptr;
    dst.normals = nullptr;
    dst.uvs = nullptr;
    dst.vertex_colors = nullptr;

    // Initialize USD geometry feature pointers (if they exist in CMeshData)
    // Note: These fields may not exist in all versions of CMeshData
    // They are only used for USD geometry features which are optional

    // Safe string copying with bounds checking
    #ifdef _WIN32
    strncpy_s(dst.element_name, sizeof(dst.element_name), src.elementName.c_str(), _TRUNCATE);
    strncpy_s(dst.type_name, sizeof(dst.type_name), src.typeName.c_str(), _TRUNCATE);
    #else
    // Use snprintf for guaranteed null termination
    snprintf(dst.element_name, sizeof(dst.element_name), "%s", src.elementName.c_str());
    snprintf(dst.type_name, sizeof(dst.type_name), "%s", src.typeName.c_str());
    #endif

    // ========================================================================
    // COPY VISUAL MESH DATA (CORRECTED FOR GLM TYPES)
    // ========================================================================

    // Points: src.points is std::vector<glm::vec3> -> convert to flat float array
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

    // Indices: src.indices is std::vector<unsigned int> -> direct copy
    dst.indices_count = src.indices.size();
    if (dst.indices_count > 0) {
        dst.indices = new unsigned int[dst.indices_count];
        std::memcpy(dst.indices, src.indices.data(), dst.indices_count * sizeof(unsigned int));
    }

    // Normals: src.normals is std::vector<glm::vec3> -> convert to flat float array
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

    // UVs: src.uvs is std::vector<glm::vec2> -> convert to flat float array
    size_t numUVs = src.uvs.size();
    dst.uvs_count = numUVs * 2;  // ✅ FIXED: Each vec2 = 2 floats
    if (dst.uvs_count > 0) {
        dst.uvs = new float[dst.uvs_count];
        for (size_t i = 0; i < numUVs; ++i) {
            dst.uvs[i * 2 + 0] = src.uvs[i].x;
            dst.uvs[i * 2 + 1] = src.uvs[i].y;
        }
    }

    // Vertex colors: src.vertex_colors is std::vector<glm::vec3> -> convert to flat float array
    size_t numColors = src.vertex_colors.size();
    dst.vertex_colors_count = numColors * 3;  // ✅ FIXED: Each vec3 = 3 floats
    if (dst.vertex_colors_count > 0) {
        dst.vertex_colors = new float[dst.vertex_colors_count];
        for (size_t i = 0; i < numColors; ++i) {
            dst.vertex_colors[i * 3 + 0] = src.vertex_colors[i].x;
            dst.vertex_colors[i * 3 + 1] = src.vertex_colors[i].y;
            dst.vertex_colors[i * 3 + 2] = src.vertex_colors[i].z;
        }
    }

    // Note: USD geometry features are not copied to CMeshData
    // The Unreal plugin's CMeshData may not have these fields
    // If needed, they should be added to the CMeshData struct definition
}


// ============================================================================
// USD PROCESSING FUNCTIONS (Legacy - No Collision)
// ============================================================================

/**
 * Load USD data from memory buffer and extract mesh geometry (Legacy)
 * ENHANCED: Now includes USD geometry features (subdivision, multi-UV, etc.)
 */
int LoadUSDBuffer_C(const unsigned char* buffer, size_t buffer_size, const char* filename,
                    CMeshData** out_meshes, size_t* out_count) {
    if (!buffer || !filename || !out_meshes || !out_count) {
        return 0;
    }

    try {
        // Convert C types to C++ types
        std::vector<uint8_t> std_buffer(buffer, buffer + buffer_size);
        std::string std_filename(filename);
        std::vector<anari_usd_middleware::UsdProcessor::MeshData> mesh_data;

        // Create UsdProcessor instance and call ProcessFile directly
        anari_usd_middleware::UsdProcessor processor;
        bool result = processor.LoadUSDBuffer(std_buffer, std_filename, mesh_data);


        if (!result || mesh_data.empty()) {
            *out_count = 0;
            *out_meshes = nullptr;
            return 0;
        }

        // Allocate C mesh array
        *out_count = mesh_data.size();
        *out_meshes = new CMeshData[*out_count];

        // Convert each mesh using helper
        for (size_t i = 0; i < mesh_data.size(); ++i) {
            ConvertMeshDataToCFormat(mesh_data[i], (*out_meshes)[i]);

            // Initialize collision fields to defaults
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

/**
 * Load USD data directly from disk file (Legacy)
 * ENHANCED: Now includes USD geometry features (subdivision, multi-UV, etc.)
 */
int LoadUSDFromDisk_C(const char* filepath, CMeshData** out_meshes, size_t* out_count) {
    if (!filepath || !out_meshes || !out_count) {
        return 0;
    }

    try {
        // Read file into buffer
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

        // Create UsdProcessor instance and call ProcessFile directly
        anari_usd_middleware::UsdProcessor processor;
        bool result = processor.LoadUSDFromDisk(std_filepath, mesh_data);


        if (!result || mesh_data.empty()) {
            *out_count = 0;
            *out_meshes = nullptr;
            return 0;
        }

        // Allocate C mesh array
        *out_count = mesh_data.size();
        *out_meshes = new CMeshData[*out_count];

        // Convert each mesh using helper
        for (size_t i = 0; i < mesh_data.size(); ++i) {
            ConvertMeshDataToCFormat(mesh_data[i], (*out_meshes)[i]);

            // Initialize collision fields to defaults
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

// ============================================================================
// USD PROCESSING FUNCTIONS WITH COLLISION SUPPORT
// ============================================================================

/**
 * Load USD data from memory buffer with collision generation
 * ENHANCED: Now includes USD geometry features + collision support
 */
int LoadUSDBufferWithCollision_C(const unsigned char* buffer,
                                  size_t buffer_size,
                                  const char* filename,
                                  int collision_complexity,
                                  CMeshData** out_meshes,
                                  size_t* out_count) {
    // Validate input parameters
    if (!g_middleware || !buffer || !filename || !out_meshes || !out_count) {
        return 0;
    }

    // Use default collision complexity if -1 is passed
    if (collision_complexity == -1) {
        collision_complexity = g_default_collision_complexity;
    }

    // Validate collision complexity
    if (collision_complexity < COLLISION_NONE || collision_complexity > COLLISION_CONVEX_DECOMP) {
        return 0;
    }

    try {
        // Initialize collision processor if not already created
        if (!g_collision_processor) {
            g_collision_processor = std::make_unique<anari_usd_middleware::CollisionProcessor>();
        }

        // Convert C types to C++ types
        std::vector<uint8_t> std_buffer(buffer, buffer + buffer_size);
        std::string std_filename(filename);
        std::vector<anari_usd_middleware::UsdProcessor::MeshData> mesh_data;

        // Call middleware USD processing (existing function)
        anari_usd_middleware::UsdProcessor processor;
        bool result = processor.LoadUSDBuffer(std_buffer, std_filename, mesh_data);

        if (!result || mesh_data.empty()) {
            *out_count = 0;
            *out_meshes = nullptr;
            return 0;
        }

        // Allocate C mesh array
        *out_count = mesh_data.size();
        *out_meshes = new CMeshData[*out_count];

        // Convert each mesh from C++ to C format WITH collision processing
        for (size_t i = 0; i < mesh_data.size(); ++i) {
            const auto& src = mesh_data[i];
            CMeshData& dst = (*out_meshes)[i];

            // Use helper function for visual mesh conversion (includes USD features)
            ConvertMeshDataToCFormat(src, dst);

            // Set collision type
            dst.collision_type = collision_complexity;

            // Generate collision data if requested
            if (collision_complexity != COLLISION_NONE) {
                // Create collision data structure
                anari_usd_middleware::CollisionData collisionData;

                // Generate collision using the CollisionProcessor
                anari_usd_middleware::ECollisionComplexity complexity =
                    static_cast<anari_usd_middleware::ECollisionComplexity>(collision_complexity);

                // Convert glm::vec3 points to flat float array
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
                    // Copy collision vertices
                    dst.collision_vertices_count = collisionData.vertices.size();
                    if (dst.collision_vertices_count > 0) {
                        dst.collision_vertices = new float[dst.collision_vertices_count];
                        std::memcpy(dst.collision_vertices, collisionData.vertices.data(),
                                   dst.collision_vertices_count * sizeof(float));
                    }

                    // Copy collision indices
                    dst.collision_indices_count = collisionData.indices.size();
                    if (dst.collision_indices_count > 0) {
                        dst.collision_indices = new unsigned int[dst.collision_indices_count];
                        std::memcpy(dst.collision_indices, collisionData.indices.data(),
                                   dst.collision_indices_count * sizeof(unsigned int));

                        // ✅ DEBUG PRINT
                        std::cout << "🔍 COLLISION COPY: vertices=" << dst.collision_vertices_count
                                  << " indices=" << dst.collision_indices_count
                                  << " ptr=" << (void*)dst.collision_vertices << std::endl;
                    }

                    // Copy simple collision data
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
                    // Collision generation failed, set defaults
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
                // No collision requested
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
        // Cleanup on exception
        *out_count = 0;
        *out_meshes = nullptr;
        return 0;
    }
}

/**
 * Load USD data from disk with collision generation
 */
int LoadUSDFromDiskWithCollision_C(const char* filepath,
                                   int collision_complexity,
                                   CMeshData** out_meshes,
                                   size_t* out_count) {
    // Validate input parameters
    if (!g_middleware || !filepath || !out_meshes || !out_count) {
        return 0;
    }

    try {
        // Read file to buffer first
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

        // Extract filename from path
        std::filesystem::path path(filepath);
        std::string filename = path.filename().string();

        // Use the buffer version with collision
        return LoadUSDBufferWithCollision_C(buffer.data(), buffer.size(),
                                           filename.c_str(), collision_complexity,
                                           out_meshes, out_count);
    } catch (...) {
        // Cleanup on exception
        *out_count = 0;
        *out_meshes = nullptr;
        return 0;
    }
}

// ============================================================================
// COLLISION CONFIGURATION FUNCTIONS
// ============================================================================

/**
 * Set default collision complexity for future USD loading operations
 */
int SetDefaultCollisionComplexity_C(int collision_complexity) {
    if (collision_complexity >= COLLISION_NONE && collision_complexity <= COLLISION_CONVEX_DECOMP) {
        g_default_collision_complexity = collision_complexity;
        return 1;
    }
    return 0;
}

/**
 * Get collision complexity name for debugging
 */
const char* GetCollisionComplexityName_C(int collision_complexity) {
    static std::string name;

    if (!g_collision_processor) {
        g_collision_processor = std::make_unique<anari_usd_middleware::CollisionProcessor>();
    }

    auto complexity = static_cast<anari_usd_middleware::ECollisionComplexity>(collision_complexity);
    name = anari_usd_middleware::CollisionProcessor::getComplexityName(complexity);
    return name.c_str();
}

/**
 * Set collision generation parameters for fine-tuning
 */
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

// ============================================================================
// TEXTURE PROCESSING FUNCTIONS
// ============================================================================

/**
 * Create texture data from raw image buffer
 * Supports common image formats and converts to RGBA
 */
CTextureData CreateTextureFromBuffer_C(const unsigned char* buffer, size_t buffer_size) {
    CTextureData result = {};

    // Validate inputs
    if (!g_middleware || !buffer) {
        return result;
    }

    try {
        // Convert to C++ vector
        std::vector<unsigned char> std_buffer(buffer, buffer + buffer_size);

        // Process through middleware
        anari_usd_middleware::TextureData tex_data =
            g_middleware->CreateTextureFromBuffer(std_buffer);

        // Copy results to C structure
        result.width = tex_data.width;
        result.height = tex_data.height;
        result.channels = tex_data.channels;
        result.data_size = tex_data.data.size();

        // Allocate and copy pixel data
        if (result.data_size > 0) {
            result.data = new unsigned char[result.data_size];
            std::memcpy(result.data, tex_data.data.data(), result.data_size);
        }

    } catch (...) {
        // Return empty result on exception
    }

    return result;
}

/**
 * Extract gradient line from image and write as PNG file
 * Specialized function for gradient/colormap processing
 */
int WriteGradientLineAsPNG_C(const unsigned char* buffer, size_t buffer_size, const char* output_path) {
    // Validate inputs
    if (!g_middleware || !buffer || !output_path) {
        return 0;
    }

    try {
        // Convert to C++ types
        std::vector<unsigned char> std_buffer(buffer, buffer + buffer_size);
        std::string std_path(output_path);

        // Process through middleware
        return g_middleware->WriteGradientLineAsPNG(std_buffer, std_path) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

/**
 * Extract gradient line from image and return PNG data in memory
 * Similar to WriteGradientLineAsPNG_C but returns data instead of writing file
 */
int GetGradientLineAsPNGBuffer_C(const unsigned char* buffer, size_t buffer_size,
                                 unsigned char** out_buffer, size_t* out_size) {
    // Validate inputs
    if (!g_middleware || !buffer || !out_buffer || !out_size || buffer_size == 0) {
        if (out_buffer) *out_buffer = nullptr;
        if (out_size) *out_size = 0;
        return 0;
    }

    // Initialize outputs
    *out_buffer = nullptr;
    *out_size = 0;

    try {
        // Convert to C++ vector
        std::vector<unsigned char> std_buffer(buffer, buffer + buffer_size);
        std::vector<unsigned char> png_buffer;

        // Process through middleware
        bool result = g_middleware->GetGradientLineAsPNGBuffer(std_buffer, png_buffer);
        if (result && !png_buffer.empty()) {
            // Allocate and copy PNG data
            *out_size = png_buffer.size();
            *out_buffer = new unsigned char[*out_size];
            std::memcpy(*out_buffer, png_buffer.data(), *out_size);
            return 1;
        }
        else {
            // Middleware failed to process
            return 0;
        }
    } 
    catch (const std::exception& e) {
        // Log error if possible
        return 0;
    }
    catch (...) {
        // Catch any other exceptions
        return 0;
    }
}

/**
 * Extract specific row from image and return PNG data in memory
 * Flexible version of GetGradientLineAsPNGBuffer_C that lets you choose which row to extract
 * Useful for 2-pixel-high gradient images where top row = gradient, bottom row = metadata
 * 
 * Implementation: Creates a 1-pixel-high PNG from the specified row of the source image
 */
int GetImageRowAsPNGBuffer_C(const unsigned char* buffer, size_t buffer_size,
                             int row_index, unsigned char** out_buffer, size_t* out_size) {
    // Validate inputs and initialize outputs
    if (!buffer || !out_buffer || !out_size || row_index < 0 || buffer_size < 30) {
        if (out_buffer) *out_buffer = nullptr;
        if (out_size) *out_size = 0;
        return 0;
    }

    // Initialize outputs
    *out_buffer = nullptr;
    *out_size = 0;

    // First, get image dimensions using our PNG header parser
    int width = 0, height = 0, channels = 0;
    int dim_result = GetPNGDimensions_C(buffer, buffer_size, &width, &height, &channels);
    
    if (dim_result == 0 || width <= 0 || height <= 0 || channels <= 0) {
        return 0;
    }

    // Validate row index
    if (row_index >= height) {
        return 0;
    }

    // For now, we'll implement a simple approach: use the existing gradient function
    // which extracts row 0, and for other rows we need more complex PNG manipulation
    // Since this is a complex feature, we'll implement it to work with the middleware
    // if available, otherwise return the top row for row_index = 0
    
    if (row_index == 0) {
        // Use existing gradient function for top row
        return GetGradientLineAsPNGBuffer_C(buffer, buffer_size, out_buffer, out_size);
    }
    else {
        // For other rows, we need full PNG decoding/encoding
        // This is complex - for now, return failure for non-zero rows
        // In a full implementation, we would decode PNG, extract row, re-encode
        return 0;
    }
}

/**
 * Get PNG image dimensions without loading full texture data
 * Lightweight function that reads PNG header to extract width, height, and channels
 * Much faster than CreateTextureFromBuffer_C for just dimension checking
 * 
 * PNG header format (first 24 bytes):
 * - Bytes 0-7: PNG signature (89 50 4E 47 0D 0A 1A 0A)
 * - Bytes 8-11: IHDR chunk length (00 00 00 0D = 13)
 * - Bytes 12-15: "IHDR" chunk type
 * - Bytes 16-19: Width (4 bytes, big-endian)
 * - Bytes 20-23: Height (4 bytes, big-endian)
 * - Byte 24: Bit depth
 * - Byte 25: Color type (2 = RGB, 6 = RGBA)
 */
int GetPNGDimensions_C(const unsigned char* buffer, size_t buffer_size,
                       int* out_width, int* out_height, int* out_channels) {
    // Validate inputs
    if (!buffer || !out_width || !out_height || !out_channels || buffer_size < 30) {
        *out_width = 0;
        *out_height = 0;
        *out_channels = 0;
        return 0;
    }

    // Check PNG signature
    const unsigned char png_signature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(buffer, png_signature, 8) != 0) {
        // Not a PNG file
        *out_width = 0;
        *out_height = 0;
        *out_channels = 0;
        return 0;
    }

    // Check for IHDR chunk at position 8
    if (std::memcmp(buffer + 12, "IHDR", 4) != 0) {
        // Invalid PNG structure
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
        
        // Read color type (byte 25) to determine channels
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

        // Validate dimensions
        if (*out_width <= 0 || *out_height <= 0 || *out_channels <= 0) {
            *out_width = 0;
            *out_height = 0;
            *out_channels = 0;
            return 0;
        }

        return 1;
    } catch (...) {
        // Set outputs to safe values on failure
        *out_width = 0;
        *out_height = 0;
        *out_channels = 0;
        return 0;
    }
}

// ============================================================================
// UTILITY AND DEBUG FUNCTIONS
// ============================================================================

/**
 * Get middleware version information
 */
const char* GetMiddlewareVersion_C() {
    static const char* version = "AnariUsdMiddleware v2.0.0 with Collision Support";
    return version;
}

/**
 * Validate USD file format without full processing
 */
int ValidateUSDFormat_C(const unsigned char* buffer, size_t buffer_size, const char* filename) {
    if (!buffer || !filename || buffer_size == 0) {
        return 0;
    }

    try {
        // Basic format validation
        std::string content(reinterpret_cast<const char*>(buffer),
                           (std::min)(buffer_size, static_cast<size_t>(1000)));

        // Check for USD-specific patterns
        return (content.find("#usda") != std::string::npos ||
                content.find("PXR-USDC") != std::string::npos ||
                content.find("def ") != std::string::npos ||
                content.find("over ") != std::string::npos) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

/**
 * Get supported USD file extensions
 */
const char* GetSupportedUSDExtensions_C() {
    static const char* extensions = ".usd,.usda,.usdc,.usdz";
    return extensions;
}

/**
 * Reset processing statistics
 */
void ResetProcessingStats_C() {
    // Implementation would reset internal statistics
    // For now, this is a placeholder
}

/**
 * Get processing statistics as formatted string
 */
const char* GetProcessingStats_C() {
    static std::string stats = "Processing statistics not yet implemented";
    return stats.c_str();
}

// ============================================================================
// MEMORY MANAGEMENT FUNCTIONS
// ============================================================================

/**
 * Free mesh data array allocated by LoadUSDBuffer_C or LoadUSDFromDisk_C
 * ENHANCED: Now properly frees collision data
 */
void FreeMeshData_C(CMeshData* meshes, size_t count) {
    if (!meshes) return;

    // Free each mesh's internal arrays with null checks
    for (size_t i = 0; i < count; ++i) {
        if (meshes[i].points) delete[] meshes[i].points;
        if (meshes[i].indices) delete[] meshes[i].indices;
        if (meshes[i].normals) delete[] meshes[i].normals;
        if (meshes[i].uvs) delete[] meshes[i].uvs;
        if (meshes[i].vertex_colors) delete[] meshes[i].vertex_colors;

        // ✅ NEW: Free collision data with null checks
        if (meshes[i].collision_vertices) delete[] meshes[i].collision_vertices;
        if (meshes[i].collision_indices) delete[] meshes[i].collision_indices;
    }

    // Free the main array
    delete[] meshes;
}

/**
 * Free texture data allocated by CreateTextureFromBuffer_C
 */
void FreeTextureData_C(CTextureData* texture) {
    if (texture && texture->data) {
        delete[] texture->data;
        texture->data = nullptr;
        texture->data_size = 0;
    }
}

/**
 * Free generic buffer allocated by middleware functions
 */
void FreeBuffer_C(unsigned char* buffer) {
    delete[] buffer;
}

/**
 * Free file data structure (for callback cleanup)
 */
void FreeFileData_C(CFileData* file_data) {
    if (file_data && file_data->data) {
        delete[] file_data->data;
        file_data->data = nullptr;
        file_data->data_size = 0;
    }
}

/**
 * Free frame files array allocated by RequestFrame_C
 */
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

// ============================================================================
// CALLBACK REGISTRATION FUNCTIONS
// ============================================================================

/**
 * Register callback function for file reception notifications
 * Only one file callback can be registered at a time
 */
void RegisterUpdateCallback_C(FileReceivedCallback_C callback) {
    g_file_callback.store(callback, std::memory_order_release);
}

/**
 * Register callback function for message reception notifications
 * Only one message callback can be registered at a time
 */
void RegisterMessageCallback_C(MessageReceivedCallback_C callback) {
    g_message_callback.store(callback, std::memory_order_release);
}

/**
 * Register callback for broker push notifications (NOTIFY_FILE_UPDATE, NOTIFY_COMMIT_COMPLETE)
 */
void RegisterNotificationCallback_C(NotificationCallback_C callback) {
    if (!g_middleware) {
        MIDDLEWARE_LOG_WARNING("RegisterNotificationCallback_C: g_middleware is NULL!");
        return;
    }
    if (callback) {
        MIDDLEWARE_LOG_INFO("RegisterNotificationCallback_C: registering notification callback");
        g_middleware->setNotificationCallback([callback](uint32_t messageType, int32_t sourceRank,
                                                           const std::string& filename,
                                                           uint64_t fileSize, uint64_t timestamp) {
            callback(messageType, sourceRank, filename.c_str(), fileSize, timestamp);
        });
    } else {
        MIDDLEWARE_LOG_INFO("RegisterNotificationCallback_C: clearing notification callback");
        g_middleware->setNotificationCallback(nullptr);
    }
}

// ============================================================================
// ASYNC BROKER FUNCTIONS (NON-BLOCKING)
// ============================================================================

/**
 * Request total worker count asynchronously (non-blocking)
 */
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
    
    // Launch async request on background thread - with null check for safe shutdown
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

/**
 * Request worker count asynchronously (non-blocking)
 */
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
    
    // Launch async request on background thread - with null check for safe shutdown
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

/**
 * Request worker status asynchronously (non-blocking)
 */
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
    
    // Launch async request on background thread - with null check for safe shutdown
    std::thread([target_rank, callback, error_callback, timeout_ms]() {
        if (!g_middleware) {
            if (error_callback) error_callback("Middleware shut down");
            return;
        }
        std::vector<std::tuple<int32_t, uint32_t, std::string, std::string, uint64_t>> worker_status;
        bool success = g_middleware->requestWorkerStatus(target_rank, worker_status, timeout_ms);
        
        if (success && callback) {
            // Serialize worker status to string for C callback
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

/**
 * Request file list asynchronously (non-blocking)
 */
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
    
    // Launch async request on background thread - with null check for safe shutdown
    std::thread([target_rank, callback, error_callback, timeout_ms]() {
        if (!g_middleware) {
            if (error_callback) error_callback("Middleware shut down");
            return;
        }
        std::vector<std::string> files;
        bool success = g_middleware->requestFileList(target_rank, files, timeout_ms);
        
        if (success && callback) {
            // Convert to C-style array
            char** file_array = new char*[files.size()];
            for (size_t i = 0; i < files.size(); i++) {
                file_array[i] = strdup(files[i].c_str());
            }
            callback(target_rank, file_array, files.size());
            
            // Free the allocated strings
            for (size_t i = 0; i < files.size(); i++) {
                free(file_array[i]);
            }
            delete[] file_array;
        } else if (error_callback) {
            error_callback(success ? "Unknown error" : "Failed to retrieve file list");
        }
    }).detach();
}

/**
 * Request files in parallel asynchronously (non-blocking)
 * Downloads multiple files simultaneously from distributed workers
 */
void RequestFilesParallelAsync_C(
    const char** filenames,
    size_t filename_count,
    const int32_t* target_ranks,
    ParallelFileReceivedCallback_C file_received_callback,
    ParallelDownloadCompleteCallback_C completion_callback,
    ParallelDownloadErrorCallback_C error_callback,
    int timeout_ms) {
    
    // NUCLEAR DEBUG: Force immediate logging that CANNOT be missed
    // Use OutputDebugString for Windows - appears in DebugView
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
    
    // Force log to middleware log AND debug output
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
    
    // Convert C arrays to C++ vectors with deduplication (unique filenames only)
    // The same file filename from different ranks will only be requested once (from the first rank it appears)
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
    
    // Convert C callbacks to C++ callbacks
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
    
    // Call the C++ async function with extreme crash protection
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

/**
 * Version verification function - call this from Unreal to verify DLL is loaded correctly
 * Returns: 1 if working, 0 if broken
 */
ANARI_USD_MIDDLEWARE_C_API int VerifyParallelDownloadDLL_C() {
    #ifdef _WIN32
    OutputDebugStringA("=== JUSYNC DEBUG: VerifyParallelDownloadDLL_C called ===\n");
    #endif
    
    MIDDLEWARE_LOG_INFO("=== VerifyParallelDownloadDLL_C ===");
    
    // Check if middleware is initialized
    if (!g_middleware) {
        MIDDLEWARE_LOG_ERROR("g_middleware is NULL");
        return 0;
    }
    
    // Check if connected
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
    
    // Get the client from the middleware
    auto client = g_middleware->getClient();
    if (!client) {
        #ifdef _WIN32
        OutputDebugStringA("[ANARI] RequestFilesParallelDirect_C: Client is NULL!\n");
        #endif
        return 0;
    }
    
    // Convert C arrays to C++ vectors
    std::vector<std::string> filename_vec;
    std::vector<int32_t> target_ranks_vec;
    
    filename_vec.reserve(filename_count);
    target_ranks_vec.reserve(filename_count);
    
    for (size_t i = 0; i < filename_count; i++) {
        filename_vec.push_back(filenames[i] ? filenames[i] : "");
        target_ranks_vec.push_back(target_ranks ? target_ranks[i] : -1);
    }
    
    // Convert C callbacks to C++ callbacks
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

// ============================================================================
// POINT CLOUD EXTRACTION
// ============================================================================

/**
 * Convert a UsdProcessor::PointCloudData to CPointCloudData format
 */
static void ConvertPointCloudDataToCFormat(const anari_usd_middleware::UsdProcessor::PointCloudData& src,
                                           CPointCloudData& dst) {
    // Initialize
    memset(&dst, 0, sizeof(CPointCloudData));

    #ifdef _WIN32
    strncpy_s(dst.element_name, sizeof(dst.element_name), src.elementName.c_str(), _TRUNCATE);
    strncpy_s(dst.type_name, sizeof(dst.type_name), src.typeName.c_str(), _TRUNCATE);
    #else
    snprintf(dst.element_name, sizeof(dst.element_name), "%s", src.elementName.c_str());
    snprintf(dst.type_name, sizeof(dst.type_name), "%s", src.typeName.c_str());
    #endif

    dst.points_count = src.positions.size();

    // Positions
    if (dst.points_count > 0) {
        dst.positions = new float[dst.points_count * 3];
        for (size_t i = 0; i < dst.points_count; ++i) {
            dst.positions[i * 3 + 0] = src.positions[i].x;
            dst.positions[i * 3 + 1] = src.positions[i].y;
            dst.positions[i * 3 + 2] = src.positions[i].z;
        }
    }

    // Colors
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

    // Normals
    if (!src.normals.empty()) {
        dst.has_normals = 1;
        dst.normals = new float[src.normals.size() * 3];
        for (size_t i = 0; i < src.normals.size(); ++i) {
            dst.normals[i * 3 + 0] = src.normals[i].x;
            dst.normals[i * 3 + 1] = src.normals[i].y;
            dst.normals[i * 3 + 2] = src.normals[i].z;
        }
    }

    // Widths — fallback to scalarAttributes[0].x (attribute0 colormap value) when USD widths empty
    if (!src.widths.empty()) {
        dst.has_widths = 1;
        dst.widths = new float[src.widths.size()];
        std::memcpy(dst.widths, src.widths.data(), src.widths.size() * sizeof(float));
    }
    else if (!src.scalarAttributes.empty() && dst.points_count > 0)
    {
        // Use scalarAttributes.x as widths for gradient/color-mapping purposes
        size_t wcount = std::min(src.scalarAttributes.size(), dst.points_count);
        dst.has_widths = 1;
        dst.widths = new float[wcount];
        for (size_t i = 0; i < wcount; ++i) {
            dst.widths[i] = src.scalarAttributes[i].x;
        }
        MIDDLEWARE_LOG_INFO("Falling back to scalarAttributes.x for widths (%zu values)", wcount);
    }

    // Bounding box
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

/**
 * Extract point cloud data from a USD buffer
 */
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

        anari_usd_middleware::UsdProcessor processor;
        std::vector<anari_usd_middleware::UsdProcessor::PointCloudData> pc_data;

        // Load USD with point cloud extraction
        std::vector<anari_usd_middleware::UsdProcessor::MeshData> dummyMeshes;
        bool result = processor.LoadUSDBuffer(std_buffer, std_filename, dummyMeshes, &pc_data);

        if (!result || pc_data.empty()) {
            // Try to see if there were any meshes but no point clouds
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

/**
 * Load USD data from buffer and extract BOTH meshes + point clouds in a single-pass parse.
 * Eliminates the double-parse bottleneck of calling LoadUSDBuffer_C + ProcessPointCloudFromUSD_C.
 */
int LoadUSDFull_C(const unsigned char* buffer,
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
        std::vector<uint8_t> std_buffer(buffer, buffer + buffer_size);
        std::string std_filename(filename);

        anari_usd_middleware::UsdProcessor processor;
        std::vector<anari_usd_middleware::UsdProcessor::MeshData> mesh_data;
        std::vector<anari_usd_middleware::UsdProcessor::PointCloudData> pc_data;

        bool result = processor.LoadUSDBuffer(std_buffer, std_filename, mesh_data, &pc_data);

        if (!result) {
            MIDDLEWARE_LOG_ERROR("LoadUSDFull_C: LoadUSDBuffer returned false for '%s' (size=%zu bytes)",
                std_filename.c_str(), buffer_size);
            *out_mesh_count = 0;
            *out_meshes = nullptr;
            *out_cloud_count = 0;
            *out_clouds = nullptr;
            return 0;
        }

        /*
         * Diagnose: if result is true but pc_data has an entry with 0 positions,
         * TinyUSDZ parsed the file but ExtractPointCloudData couldn't get point data.
         */
        {
            size_t valid_pc = 0, invalid_pc = 0;
            for (const auto& pc : pc_data) {
                if (pc.positions.size() > 0) valid_pc++;
                else invalid_pc++;
            }
            if (invalid_pc > 0) {
                MIDDLEWARE_LOG_ERROR("LoadUSDFull_C: %zu point clouds have 0 positions for '%s' (valid=%zu, invalid=%zu)",
                    invalid_pc, std_filename.c_str(), valid_pc, invalid_pc);
            }
            /*
             * If all extracted PCs have 0 positions, treat as failure (no usable data)
             */
            if (valid_pc == 0 && invalid_pc > 0) {
                MIDDLEWARE_LOG_ERROR("LoadUSDFull_C: no usable point clouds extracted — discarding for '%s'",
                    std_filename.c_str());
                pc_data.clear();
            }
        }

        if (mesh_data.empty() && pc_data.empty()) {
            MIDDLEWARE_LOG_WARNING("LoadUSDFull_C: LoadUSDBuffer succeeded but returned 0 meshes + 0 PCs for '%s'",
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

        MIDDLEWARE_LOG_INFO("LoadUSDFull_C: extracted %zu meshes + %zu point clouds from '%s' in single pass",
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

/**
 * Get the most recently cached gradient/colormap texture
 * Returns PNG-encoded raw bytes; caller must decode
 */
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

        // Allocate and copy the PNG data for caller
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

/**
 * Free memory allocated by ProcessPointCloudFromUSD_C
 */
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

/**
 * Free memory allocated by GetCachedGradientTexture_C
 */
void FreeCachedGradientTexture_C(unsigned char* gradient_rgba) {
    if (!gradient_rgba) return;
    delete[] gradient_rgba;
}

} // extern "C"
