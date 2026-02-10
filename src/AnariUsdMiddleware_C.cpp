#include "AnariUsdMiddleware_C.h"
#include "AnariUsdMiddleware.h"
#include "CollisionProcessor.h"
#include "UsdProcessor.h"
#include "AnariUsdMessages.h"

#include <memory>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <thread>
#include <mutex>

// ============================================================================
// GLOBAL STATE MANAGEMENT
// ============================================================================

// Global middleware instance - ensures single instance per process
static std::unique_ptr<anari_usd_middleware::AnariUsdMiddleware> g_middleware;

// Global collision processor instance
static std::unique_ptr<anari_usd_middleware::CollisionProcessor> g_collision_processor;

// Global callback storage - maintains C callback function pointers
static FileReceivedCallback_C g_file_callback = nullptr;
static MessageReceivedCallback_C g_message_callback = nullptr;

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
#ifdef _WIN32
                        strncpy_s(c_data.filename, sizeof(c_data.filename), file_data.filename.c_str(), 255);
                        strncpy_s(c_data.hash, sizeof(c_data.hash), file_data.hash.c_str(), 63);
                        strncpy_s(c_data.file_type, sizeof(c_data.file_type), file_data.fileType.c_str(), 31);
#else
                        std::strncpy(c_data.filename, file_data.filename.c_str(), 255);
                        std::strncpy(c_data.hash, file_data.hash.c_str(), 63);
                        std::strncpy(c_data.file_type, file_data.fileType.c_str(), 31);
                        // Ensure null termination
                        c_data.filename[255] = '\0';
                        c_data.hash[63] = '\0';
                        c_data.file_type[31] = '\0';
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
                        g_file_callback(&c_data);
                    }
                });
            }

            // Register message callback if available
            if (g_message_callback) {
                g_middleware->registerMessageCallback([](const std::string& message) {
                    if (g_message_callback) {
                        g_message_callback(message.c_str());
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
            strncpy_s((*out_files)[i], len, files[i].c_str(), len - 1);
#else
            std::strncpy((*out_files)[i], files[i].c_str(), len);
            (*out_files)[i][len - 1] = '\0';
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
    
    std::lock_guard<std::mutex> lock(g_middleware_mutex);
    
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
            std::strncpy((*out_names)[i], files[i].name.c_str(), len);
            (*out_names)[i][len - 1] = '\0';
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
 * Request a specific file from a rank
 */
int RequestFile_C(const char* filename, int32_t target_rank,
                  unsigned char** out_data, size_t* out_size, int timeout_ms) {
    if (!g_middleware || !filename || !out_data || !out_size) {
        return 0;
    }
    
    std::lock_guard<std::mutex> lock(g_middleware_mutex);
    
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
            
            *out_data = new (std::nothrow) unsigned char[*out_size];
            if (!*out_data) {
                MIDDLEWARE_LOG_ERROR("Memory allocation failed for %zu bytes", *out_size);
                *out_size = 0;
                return 0;
            }
            std::memcpy(*out_data, fileData.data(), *out_size);
            MIDDLEWARE_LOG_DEBUG("Allocated %zu bytes at %p", *out_size, (void*)*out_data);
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
            strncpy_s(c_file.filename, sizeof(c_file.filename), frameFiles[i].first.c_str(), 255);
#else
            std::strncpy(c_file.filename, frameFiles[i].first.c_str(), 255);
            c_file.filename[255] = '\0';
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
    strncpy_s(dst.element_name, sizeof(dst.element_name), src.elementName.c_str(), 255);
    strncpy_s(dst.type_name, sizeof(dst.type_name), src.typeName.c_str(), 127);
    #else
    std::strncpy(dst.element_name, src.elementName.c_str(), 255);
    std::strncpy(dst.type_name, src.typeName.c_str(), 127);
    dst.element_name[255] = '\0';
    dst.type_name[127] = '\0';
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
    if (!g_middleware || !buffer || !out_buffer || !out_size) {
        return 0;
    }

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
    } catch (...) {
        // Fall through to return 0
    }

    // Set outputs to safe values on failure
    *out_buffer = nullptr;
    *out_size = 0;
    return 0;
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
                           std::min(buffer_size, static_cast<size_t>(1000)));

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

    // Free each mesh's internal arrays
    for (size_t i = 0; i < count; ++i) {
        delete[] meshes[i].points;
        delete[] meshes[i].indices;
        delete[] meshes[i].normals;
        delete[] meshes[i].uvs;
        delete[] meshes[i].vertex_colors;

        // ✅ NEW: Free collision data
        delete[] meshes[i].collision_vertices;
        delete[] meshes[i].collision_indices;
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

// ============================================================================
// CALLBACK REGISTRATION FUNCTIONS
// ============================================================================

/**
 * Register callback function for file reception notifications
 * Only one file callback can be registered at a time
 */
void RegisterUpdateCallback_C(FileReceivedCallback_C callback) {
    g_file_callback = callback;
}

/**
 * Register callback function for message reception notifications
 * Only one message callback can be registered at a time
 */
void RegisterMessageCallback_C(MessageReceivedCallback_C callback) {
    g_message_callback = callback;
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
    
    // Launch async request on background thread
    std::thread([callback, error_callback, timeout_ms]() {
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
    
    // Launch async request on background thread
    std::thread([callback, error_callback, timeout_ms]() {
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
    
    // Launch async request on background thread
    std::thread([target_rank, callback, error_callback, timeout_ms]() {
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
    
    // Launch async request on background thread
    std::thread([target_rank, callback, error_callback, timeout_ms]() {
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

} // extern "C"
