#ifndef ANARI_USD_MIDDLEWARE_C_H
#define ANARI_USD_MIDDLEWARE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// API export/import macros for cross-platform compatibility
#ifndef ANARI_USD_MIDDLEWARE_C_API
#ifdef _WIN32
#ifdef ANARI_USD_MIDDLEWARE_EXPORTS
#define ANARI_USD_MIDDLEWARE_C_API __declspec(dllexport)
#else
#define ANARI_USD_MIDDLEWARE_C_API __declspec(dllimport)
#endif
#else
#define ANARI_USD_MIDDLEWARE_C_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// C-COMPATIBLE DATA STRUCTURES
// ============================================================================

/**
 * File data structure for C interface
 * Contains received file information and binary data
 */
typedef struct {
    char filename[256];          // Original filename (null-terminated)
    unsigned char* data;         // Binary file data (dynamically allocated)
    size_t data_size;           // Size of data in bytes
    char hash[64];              // SHA256 hash (null-terminated hex string)
    char file_type[32];         // File type identifier (e.g., "USD", "IMAGE")
} CFileData;

/**
 * Mesh data structure for C interface
 * Contains all geometric data for a single mesh primitive
 * Compatible with Unreal Engine RealtimeMesh component
 */
typedef struct {
    char element_name[256];      // USD primitive name (null-terminated)
    char type_name[128];         // USD primitive type (null-terminated)

    // Vertex positions as flat array [x1,y1,z1, x2,y2,z2, ...]
    float* points;
    size_t points_count;         // Total number of floats (vertices * 3)

    // Triangle indices referencing vertex positions
    unsigned int* indices;
    size_t indices_count;        // Total number of indices (triangles * 3)

    // Vertex normals as flat array [nx1,ny1,nz1, nx2,ny2,nz2, ...]
    float* normals;
    size_t normals_count;        // Total number of floats (vertices * 3)

    // UV coordinates as flat array [u1,v1, u2,v2, ...]
    float* uvs;
    size_t uvs_count;           // Total number of floats (vertices * 2)

    // Vertex colors as flat RGBA array [r1,g1,b1,a1, r2,g2,b2,a2, ...]
    // Values are in range [0.0, 1.0]
    float* vertex_colors;
    size_t vertex_colors_count;  // Total number of floats (vertices * 4)
} CMeshData;

/**
 * Texture data structure for C interface
 * Contains decoded image data ready for GPU upload
 */
typedef struct {
    int width;                   // Image width in pixels
    int height;                  // Image height in pixels
    int channels;                // Number of channels (typically 3 or 4)
    unsigned char* data;         // Raw pixel data (dynamically allocated)
    size_t data_size;           // Size of pixel data in bytes
} CTextureData;

// ============================================================================
// CALLBACK FUNCTION TYPES
// ============================================================================

/**
 * Callback function type for file reception notifications
 * Called when a new file is received via ZeroMQ
 * @param file_data Pointer to received file data (valid only during callback)
 */
typedef void (*FileReceivedCallback_C)(const CFileData* file_data);

/**
 * Callback function type for message reception notifications
 * Called when a text message is received via ZeroMQ
 * @param message Null-terminated message string (valid only during callback)
 */
typedef void (*MessageReceivedCallback_C)(const char* message);

// ============================================================================
// CORE MIDDLEWARE FUNCTIONS
// ============================================================================

/**
 * Initialize the middleware with ZeroMQ endpoint
 * Must be called before any other operations
 * @param endpoint ZeroMQ endpoint string (e.g., "tcp://*:5556") or NULL for default
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int InitializeMiddleware_C(const char* endpoint);

/**
 * Shutdown the middleware and cleanup all resources
 * Safe to call multiple times
 */
ANARI_USD_MIDDLEWARE_C_API void ShutdownMiddleware_C(void);

/**
 * Check if middleware is connected and ready to receive data
 * @return 1 if connected, 0 if not connected
 */
ANARI_USD_MIDDLEWARE_C_API int IsConnected_C(void);

/**
 * Get current status information for debugging
 * @return Pointer to status string (valid until next call)
 */
ANARI_USD_MIDDLEWARE_C_API const char* GetStatusInfo_C(void);

/**
 * Start the background receiver thread
 * Non-blocking operation that enables automatic file/message reception
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int StartReceiving_C(void);

/**
 * Stop the background receiver thread
 * Blocks until receiver thread has safely terminated
 */
ANARI_USD_MIDDLEWARE_C_API void StopReceiving_C(void);

// ============================================================================
// USD PROCESSING FUNCTIONS
// ============================================================================

/**
 * Load USD data from memory buffer and extract mesh geometry
 * Supports .usd, .usda, .usdc, and .usdz formats
 * Extracts vertex positions, indices, normals, UVs, and vertex colors
 * @param buffer Raw USD file data
 * @param buffer_size Size of buffer in bytes
 * @param filename Original filename (used for format detection)
 * @param out_meshes Pointer to receive array of extracted meshes (caller must free)
 * @param out_count Pointer to receive number of extracted meshes
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int LoadUSDBuffer_C(const unsigned char* buffer,
                                              size_t buffer_size,
                                              const char* filename,
                                              CMeshData** out_meshes,
                                              size_t* out_count);

/**
 * Load USD data directly from disk file
 * Wrapper around LoadUSDBuffer_C with file I/O handling
 * @param filepath Path to USD file on disk
 * @param out_meshes Pointer to receive array of extracted meshes (caller must free)
 * @param out_count Pointer to receive number of extracted meshes
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int LoadUSDFromDisk_C(const char* filepath,
                                                 CMeshData** out_meshes,
                                                 size_t* out_count);

// ============================================================================
// TEXTURE PROCESSING FUNCTIONS
// ============================================================================

/**
 * Create texture data from raw image buffer
 * Supports common image formats (PNG, JPG, etc.)
 * Automatically converts to RGBA format
 * @param buffer Raw image file data
 * @param buffer_size Size of buffer in bytes
 * @return Texture data structure (caller must free with FreeTextureData_C)
 */
ANARI_USD_MIDDLEWARE_C_API CTextureData CreateTextureFromBuffer_C(const unsigned char* buffer,
                                                                  size_t buffer_size);

/**
 * Extract gradient line from image and write as PNG file
 * Specialized function for gradient/colormap processing
 * @param buffer Raw image data containing gradient
 * @param buffer_size Size of buffer in bytes
 * @param output_path Output file path for PNG
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int WriteGradientLineAsPNG_C(const unsigned char* buffer,
                                                       size_t buffer_size,
                                                       const char* output_path);

/**
 * Extract gradient line from image and return PNG data in memory
 * Similar to WriteGradientLineAsPNG_C but returns data instead of writing file
 * @param buffer Raw image data containing gradient
 * @param buffer_size Size of buffer in bytes
 * @param out_png_data Pointer to receive PNG data (caller must free)
 * @param out_png_size Pointer to receive PNG data size
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int GetGradientLineAsPNGBuffer_C(const unsigned char* buffer,
                                                           size_t buffer_size,
                                                           unsigned char** out_png_data,
                                                           size_t* out_png_size);

// ============================================================================
// MEMORY MANAGEMENT FUNCTIONS
// ============================================================================

/**
 * Free mesh data array allocated by LoadUSDBuffer_C or LoadUSDFromDisk_C
 * Safely deallocates all internal arrays and the main array
 * @param meshes Pointer to mesh array to free
 * @param count Number of meshes in array
 */
ANARI_USD_MIDDLEWARE_C_API void FreeMeshData_C(CMeshData* meshes, size_t count);

/**
 * Free texture data allocated by CreateTextureFromBuffer_C
 * @param texture Pointer to texture data to free
 */
ANARI_USD_MIDDLEWARE_C_API void FreeTextureData_C(CTextureData* texture);

/**
 * Free generic buffer allocated by middleware functions
 * @param buffer Pointer to buffer to free
 */
ANARI_USD_MIDDLEWARE_C_API void FreeBuffer_C(unsigned char* buffer);

/**
 * Free file data structure (for callback cleanup if needed)
 * @param file_data Pointer to file data to free
 */
ANARI_USD_MIDDLEWARE_C_API void FreeFileData_C(CFileData* file_data);

// ============================================================================
// CALLBACK REGISTRATION FUNCTIONS
// ============================================================================

/**
 * Register callback function for file reception notifications
 * Only one file callback can be registered at a time
 * @param callback Function pointer to call when files are received
 */
ANARI_USD_MIDDLEWARE_C_API void RegisterUpdateCallback_C(FileReceivedCallback_C callback);

/**
 * Register callback function for message reception notifications
 * Only one message callback can be registered at a time
 * @param callback Function pointer to call when messages are received
 */
ANARI_USD_MIDDLEWARE_C_API void RegisterMessageCallback_C(MessageReceivedCallback_C callback);

#ifdef __cplusplus
}
#endif

#endif // ANARI_USD_MIDDLEWARE_C_H
