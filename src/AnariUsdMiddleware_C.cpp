#include "AnariUsdMiddleware_C.h"
#include "AnariUsdMiddleware.h"
#include <memory>
#include <string>
#include <cstring>

// ============================================================================
// GLOBAL STATE MANAGEMENT
// ============================================================================

// Global middleware instance - ensures single instance per process
static std::unique_ptr<anari_usd_middleware::AnariUsdMiddleware> g_middleware;

// Global callback storage - maintains C callback function pointers
static FileReceivedCallback_C g_file_callback = nullptr;
static MessageReceivedCallback_C g_message_callback = nullptr;

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

        // Use provided endpoint or default fallback
        std::string endpoint_str = endpoint ? endpoint : "tcp://*:5556";
        bool result = g_middleware->initialize(endpoint_str.c_str());

        // CRITICAL FIX: Only register callbacks after successful initialization
        if (result) {
            // Register file callback if available
            if (g_file_callback) {
                g_middleware->registerUpdateCallback([](const anari_usd_middleware::AnariUsdMiddleware::FileData& file_data) {
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

                        // FIXED: Call the callback without immediately freeing memory
                        // Memory will be cleaned up when Unreal calls FreeFileData_C
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

/**
 * Load USD data from memory buffer and extract mesh geometry
 * ENHANCED: Now includes vertex color extraction for Unreal RealtimeMesh
 */
int LoadUSDBuffer_C(const unsigned char* buffer, size_t buffer_size, const char* filename,
                   CMeshData** out_meshes, size_t* out_count) {
    // Validate input parameters
    if (!g_middleware || !buffer || !filename || !out_meshes || !out_count) {
        return 0;
    }

    try {
        // Convert C types to C++ types
        std::vector<unsigned char> std_buffer(buffer, buffer + buffer_size);
        std::string std_filename(filename);
        std::vector<anari_usd_middleware::AnariUsdMiddleware::MeshData> mesh_data;

        // Call middleware USD processing
        bool result = g_middleware->LoadUSDBuffer(std_buffer, std_filename, mesh_data);

        if (!result || mesh_data.empty()) {
            *out_count = 0;
            *out_meshes = nullptr;
            return 0;
        }

        // Allocate C mesh array
        *out_count = mesh_data.size();
        *out_meshes = new CMeshData[*out_count];

        // Convert each mesh from C++ to C format
        for (size_t i = 0; i < mesh_data.size(); ++i) {
            const auto& src = mesh_data[i];
            CMeshData& dst = (*out_meshes)[i];

            // Initialize all pointers to null for safety
            dst.points = nullptr;
            dst.indices = nullptr;
            dst.normals = nullptr;
            dst.uvs = nullptr;
            dst.vertex_colors = nullptr;

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

            // FIXED: Copy points (src.points is already a flat float array from middleware)
            dst.points_count = src.points.size();
            if (dst.points_count > 0) {
                dst.points = new float[dst.points_count];
                std::memcpy(dst.points, src.points.data(), dst.points_count * sizeof(float));
            }

            // Copy triangle indices
            dst.indices_count = src.indices.size();
            if (dst.indices_count > 0) {
                dst.indices = new unsigned int[dst.indices_count];
                std::memcpy(dst.indices, src.indices.data(), dst.indices_count * sizeof(unsigned int));
            }

            // FIXED: Copy normals (src.normals is already a flat float array from middleware)
            dst.normals_count = src.normals.size();
            if (dst.normals_count > 0) {
                dst.normals = new float[dst.normals_count];
                std::memcpy(dst.normals, src.normals.data(), dst.normals_count * sizeof(float));
            }

            // FIXED: Copy UVs (src.uvs is already a flat float array from middleware)
            dst.uvs_count = src.uvs.size();
            if (dst.uvs_count > 0) {
                dst.uvs = new float[dst.uvs_count];
                std::memcpy(dst.uvs, src.uvs.data(), dst.uvs_count * sizeof(float));
            }

            // ✅ NEW: Copy vertex colors (RGBA values from primvars:color.timeSamples)
            // This is the missing piece that will fix your color display issue
            dst.vertex_colors_count = src.vertex_colors.size();
            if (dst.vertex_colors_count > 0) {
                dst.vertex_colors = new float[dst.vertex_colors_count];
                std::memcpy(dst.vertex_colors, src.vertex_colors.data(),
                           dst.vertex_colors_count * sizeof(float));
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
 * Load USD data directly from disk file
 * ENHANCED: Now includes vertex color extraction for Unreal RealtimeMesh
 */
int LoadUSDFromDisk_C(const char* filepath, CMeshData** out_meshes, size_t* out_count) {
    // Validate input parameters
    if (!g_middleware || !filepath || !out_meshes || !out_count) {
        return 0;
    }

    try {
        // Convert C types to C++ types
        std::string std_filepath(filepath);
        std::vector<anari_usd_middleware::AnariUsdMiddleware::MeshData> mesh_data;

        // Call middleware USD processing
        bool result = g_middleware->LoadUSDFromDisk(std_filepath, mesh_data);

        if (!result || mesh_data.empty()) {
            *out_count = 0;
            *out_meshes = nullptr;
            return 0;
        }

        // Allocate C mesh array
        *out_count = mesh_data.size();
        *out_meshes = new CMeshData[*out_count];

        // Convert each mesh from C++ to C format
        for (size_t i = 0; i < mesh_data.size(); ++i) {
            const auto& src = mesh_data[i];
            CMeshData& dst = (*out_meshes)[i];

            // Initialize all pointers to null for safety
            dst.points = nullptr;
            dst.indices = nullptr;
            dst.normals = nullptr;
            dst.uvs = nullptr;
            dst.vertex_colors = nullptr;

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

            // FIXED: Direct memory copy for flat arrays (already processed by middleware)
            dst.points_count = src.points.size();
            if (dst.points_count > 0) {
                dst.points = new float[dst.points_count];
                std::memcpy(dst.points, src.points.data(), dst.points_count * sizeof(float));
            }

            dst.indices_count = src.indices.size();
            if (dst.indices_count > 0) {
                dst.indices = new unsigned int[dst.indices_count];
                std::memcpy(dst.indices, src.indices.data(), dst.indices_count * sizeof(unsigned int));
            }

            dst.normals_count = src.normals.size();
            if (dst.normals_count > 0) {
                dst.normals = new float[dst.normals_count];
                std::memcpy(dst.normals, src.normals.data(), dst.normals_count * sizeof(float));
            }

            dst.uvs_count = src.uvs.size();
            if (dst.uvs_count > 0) {
                dst.uvs = new float[dst.uvs_count];
                std::memcpy(dst.uvs, src.uvs.data(), dst.uvs_count * sizeof(float));
            }

            // ✅ NEW: Copy vertex colors (RGBA values from primvars:color.timeSamples)
            // This enables vertex colors from USD files in Unreal Engine
            dst.vertex_colors_count = src.vertex_colors.size();
            if (dst.vertex_colors_count > 0) {
                dst.vertex_colors = new float[dst.vertex_colors_count];
                std::memcpy(dst.vertex_colors, src.vertex_colors.data(),
                           dst.vertex_colors_count * sizeof(float));
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
        anari_usd_middleware::AnariUsdMiddleware::TextureData tex_data =
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
// MEMORY MANAGEMENT FUNCTIONS
// ============================================================================

/**
 * Free mesh data array allocated by LoadUSDBuffer_C or LoadUSDFromDisk_C
 * ENHANCED: Now properly frees vertex color data
 */
void FreeMeshData_C(CMeshData* meshes, size_t count) {
    if (!meshes) return;

    // Free each mesh's internal arrays
    for (size_t i = 0; i < count; ++i) {
        delete[] meshes[i].points;
        delete[] meshes[i].indices;
        delete[] meshes[i].normals;
        delete[] meshes[i].uvs;
        delete[] meshes[i].vertex_colors;  // ✅ NEW: Free vertex colors
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

} // extern "C"
