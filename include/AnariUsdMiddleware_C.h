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
// COLLISION COMPLEXITY ENUMERATION
// ============================================================================

/**
 * Collision complexity options for different use cases
 * These values are exposed to Unreal Engine Blueprints
 * Higher complexity = more accurate collision but slower performance
 */
typedef enum {
    COLLISION_NONE = 0,           // No collision generation
    COLLISION_SIMPLE = 1,         // Bounding box collision (fastest)
    COLLISION_CONVEX_HULL = 2,    // Convex hull around mesh (balanced)
    COLLISION_COMPLEX = 3,        // Full mesh collision (most accurate, default)
    COLLISION_SIMPLIFIED = 4,     // Decimated mesh for performance (25% triangles)
    COLLISION_CONVEX_DECOMP = 5   // V-HACD convex decomposition (best for concave shapes)
} ECollisionComplexity_C;

// ============================================================================
// C-COMPATIBLE DATA STRUCTURES
// ============================================================================

/**
 * File data structure for C interface
 * Contains received file information and binary data
 * Used by ZeroMQ callbacks when files are received
 */
typedef struct {
    char filename[256];          // Original filename (null-terminated)
    unsigned char* data;         // Binary file data (dynamically allocated)
    size_t data_size;           // Size of data in bytes
    char hash[64];              // SHA256 hash (null-terminated hex string)
    char file_type[32];         // File type identifier (e.g., "USD", "IMAGE")
} CFileData;

/**
 * Enhanced mesh data structure for C interface with collision support
 * Contains all geometric data for a single mesh primitive
 * Compatible with Unreal Engine RealtimeMeshComponent and Physics System
 *
 * Memory Layout:
 * - All arrays are flat and suitable for GPU upload
 * - Vertex data is interleaved for optimal cache performance
 * - Collision data is separate from visual mesh data
 */
typedef struct {
    char element_name[256];      // USD primitive name (null-terminated)
    char type_name[128];         // USD primitive type (null-terminated)

    // ========== VISUAL MESH DATA ==========
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

    // ========== COLLISION DATA ==========
    int collision_type;          // Maps to ECollisionComplexity_C enum

    // Collision mesh vertices (may differ from visual mesh)
    float* collision_vertices;
    size_t collision_vertices_count;  // Total number of floats (collision vertices * 3)

    // Collision mesh triangle indices
    unsigned int* collision_indices;
    size_t collision_indices_count;   // Total number of indices (collision triangles * 3)

    // Simple collision primitives (for bounding boxes, spheres)
    float bounding_box_min[3];   // Minimum bounds [x, y, z]
    float bounding_box_max[3];   // Maximum bounds [x, y, z]
    float sphere_center[3];      // Sphere center [x, y, z]
    float sphere_radius;         // Sphere radius

    // ========== USD GEOMETRY FEATURES ==========
    const char* subdivision_scheme;    // Subdivision scheme (e.g., "catmull-clark", "bilinear", "none")
    int double_sided;            // Double-sided flag (0 = false, 1 = true)

    // Face vertex counts for heterogenous polygons
    unsigned int* face_vertex_counts;
    size_t face_vertex_counts_size;

    // Multiple UV sets
    float** uv_sets;             // Array of UV set pointers (each is flat array [u,v,...])
    const char** uv_set_names;   // Array of UV set name pointers
    size_t uv_sets_count;        // Number of UV sets

} CMeshData;

/**
 * Texture data structure for C interface
 * Contains decoded image data ready for GPU upload
 * Automatically converted to RGBA format for consistency
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
 *
 * IMPORTANT: The file_data pointer is only valid during the callback.
 * If you need to keep the data, copy it immediately.
 *
 * @param file_data Pointer to received file data (valid only during callback)
 */
typedef void (*FileReceivedCallback_C)(const CFileData* file_data);

/**
 * Callback function type for message reception notifications
 * Called when a text message is received via ZeroMQ
 *
 * @param message Null-terminated message string (valid only during callback)
 */
typedef void (*MessageReceivedCallback_C)(const char* message);

// ============================================================================
// CORE MIDDLEWARE FUNCTIONS
// ============================================================================

/**
 * Initialize the middleware with ZeroMQ endpoint
 * Must be called before any other operations
 *
* @param endpoint ZeroMQ endpoint string (e.g., "tcp://0.0.0.0:5556") or NULL for default
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int InitializeMiddleware_C(const char* endpoint);

/**
 * Shutdown the middleware and cleanup all resources
 * Safe to call multiple times
 * Automatically stops receiver thread and disconnects ZeroMQ
 */
ANARI_USD_MIDDLEWARE_C_API void ShutdownMiddleware_C(void);

/**
 * Check if middleware is connected and ready to receive data
 * Thread-safe operation
 *
 * @return 1 if connected, 0 if not connected
 */
ANARI_USD_MIDDLEWARE_C_API int IsConnected_C(void);

/**
 * Get current status information for debugging
 * Returns connection status, statistics, and health information
 *
 * @return Pointer to status string (valid until next call)
 */
ANARI_USD_MIDDLEWARE_C_API const char* GetStatusInfo_C(void);

/**
 * Start the background receiver thread
 * Non-blocking operation that enables automatic file/message reception
 * The receiver thread handles all ZeroMQ communication
 *
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int StartReceiving_C(void);

/**
 * Stop the background receiver thread
 * Blocks until receiver thread has safely terminated
 * Safe to call multiple times
 */
ANARI_USD_MIDDLEWARE_C_API void StopReceiving_C(void);

// ============================================================================
// USD PROCESSING FUNCTIONS (Legacy - No Collision)
// ============================================================================

/**
 * Load USD data from memory buffer and extract mesh geometry (Legacy)
 * Supports .usd, .usda, .usdc, and .usdz formats
 * Extracts vertex positions, indices, normals, UVs, and vertex colors
 *
 * NOTE: This function does NOT generate collision data.
 * Use LoadUSDBufferWithCollision_C for collision support.
 *
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
 * Load USD data directly from disk file (Legacy)
 * Wrapper around LoadUSDBuffer_C with file I/O handling
 *
 * NOTE: This function does NOT generate collision data.
 * Use LoadUSDFromDiskWithCollision_C for collision support.
 *
 * @param filepath Path to USD file on disk
 * @param out_meshes Pointer to receive array of extracted meshes (caller must free)
 * @param out_count Pointer to receive number of extracted meshes
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int LoadUSDFromDisk_C(const char* filepath,
                                                  CMeshData** out_meshes,
                                                  size_t* out_count);

// ============================================================================
// USD PROCESSING FUNCTIONS WITH COLLISION SUPPORT
// ============================================================================

/**
 * Load USD data from memory buffer with collision generation
 * Enhanced version of LoadUSDBuffer_C with collision support
 *
 * Collision Generation Process:
 * 1. Extract visual mesh data (same as legacy function)
 * 2. Generate collision geometry based on complexity setting
 * 3. Populate collision fields in CMeshData structure
 *
 * Performance Recommendations:
 * - COLLISION_SIMPLE: Background/static objects
 * - COLLISION_COMPLEX: Interactive/detailed objects
 * - COLLISION_SIMPLIFIED: Performance-critical scenarios
 *
 * @param buffer Raw USD file data
 * @param buffer_size Size of buffer in bytes
 * @param filename Original filename (used for format detection)
 * @param collision_complexity Collision complexity level (ECollisionComplexity_C)
 * @param out_meshes Pointer to receive array of extracted meshes (caller must free)
 * @param out_count Pointer to receive number of extracted meshes
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int LoadUSDBufferWithCollision_C(const unsigned char* buffer,
                                                            size_t buffer_size,
                                                            const char* filename,
                                                            int collision_complexity,
                                                            CMeshData** out_meshes,
                                                            size_t* out_count);

/**
 * Load USD data from disk with collision generation
 * Enhanced version of LoadUSDFromDisk_C with collision support
 *
 * @param filepath Path to USD file on disk
 * @param collision_complexity Collision complexity level (ECollisionComplexity_C)
 * @param out_meshes Pointer to receive array of extracted meshes (caller must free)
 * @param out_count Pointer to receive number of extracted meshes
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int LoadUSDFromDiskWithCollision_C(const char* filepath,
                                                              int collision_complexity,
                                                              CMeshData** out_meshes,
                                                              size_t* out_count);

// ============================================================================
// COLLISION CONFIGURATION FUNCTIONS
// ============================================================================

/**
 * Set default collision complexity for future USD loading operations
 * This affects LoadUSDBufferWithCollision_C and LoadUSDFromDiskWithCollision_C
 * when collision_complexity parameter is set to -1 (use default)
 *
 * @param collision_complexity Default collision complexity level
 * @return 1 on success, 0 on failure (invalid complexity value)
 */
ANARI_USD_MIDDLEWARE_C_API int SetDefaultCollisionComplexity_C(int collision_complexity);

/**
 * Get collision complexity name for debugging and UI display
 * Useful for dropdown menus in Unreal Blueprint functions
 *
 * @param collision_complexity Collision complexity enum value
 * @return Pointer to collision name string (valid until next call)
 */
ANARI_USD_MIDDLEWARE_C_API const char* GetCollisionComplexityName_C(int collision_complexity);

/**
 * Set collision generation parameters for fine-tuning
 * Advanced configuration for collision processing
 *
 * @param simplification_ratio Ratio for simplified collision (0.1 to 0.9, default 0.25)
 * @param convex_hull_precision Precision for convex hull generation (0.001 to 0.1, default 0.001)
 * @param max_convex_hulls Maximum number of convex hulls for decomposition (1 to 64, default 32)
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int SetCollisionParameters_C(float simplification_ratio,
                                                        float convex_hull_precision,
                                                        int max_convex_hulls);

// ============================================================================
// TEXTURE PROCESSING FUNCTIONS
// ============================================================================

/**
 * Create texture data from raw image buffer
 * Supports common image formats (PNG, JPG, TGA, BMP, etc.)
 * Automatically converts to RGBA format for consistency
 *
 * @param buffer Raw image file data
 * @param buffer_size Size of buffer in bytes
 * @return Texture data structure (caller must free with FreeTextureData_C)
 */
ANARI_USD_MIDDLEWARE_C_API CTextureData CreateTextureFromBuffer_C(const unsigned char* buffer,
                                                                   size_t buffer_size);

/**
 * Extract gradient line from image and write as PNG file
 * Specialized function for gradient/colormap processing
 * Extracts the top row of a 2-pixel-high gradient image
 *
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
 * Useful for in-memory processing and network transmission
 *
 * @param buffer Raw image data containing gradient
 * @param buffer_size Size of buffer in bytes
 * @param out_png_data Pointer to receive PNG data (caller must free with FreeBuffer_C)
 * @param out_png_size Pointer to receive PNG data size
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int GetGradientLineAsPNGBuffer_C(const unsigned char* buffer,
                                                         size_t buffer_size,
                                                         unsigned char** out_png_data,
                                                         size_t* out_png_size);

/**
 * Extract specific row from image and return PNG data in memory
 * Flexible version of GetGradientLineAsPNGBuffer_C that lets you choose which row to extract
 * Useful for 2-pixel-high gradient images where top row = gradient, bottom row = metadata
 *
 * @param buffer Raw image data containing gradient
 * @param buffer_size Size of buffer in bytes
 * @param row_index Which row to extract (0 = top row, 1 = bottom row for 2-pixel images)
 * @param out_png_data Pointer to receive PNG data (caller must free with FreeBuffer_C)
 * @param out_png_size Pointer to receive PNG data size
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int GetImageRowAsPNGBuffer_C(const unsigned char* buffer,
                                                     size_t buffer_size,
                                                     int row_index,
                                                     unsigned char** out_png_data,
                                                     size_t* out_png_size);

/**
 * Get PNG image dimensions without loading full texture data
 * Lightweight function that reads PNG header to extract width, height, and channels
 * Much faster than CreateTextureFromBuffer_C for just dimension checking
 *
 * @param buffer Raw PNG image data
 * @param buffer_size Size of buffer in bytes
 * @param out_width Pointer to receive image width (pixels)
 * @param out_height Pointer to receive image height (pixels)
 * @param out_channels Pointer to receive number of color channels (3 for RGB, 4 for RGBA)
 * @return 1 on success, 0 on failure (invalid PNG or buffer too small)
 */
ANARI_USD_MIDDLEWARE_C_API int GetPNGDimensions_C(const unsigned char* buffer,
                                               size_t buffer_size,
                                               int* out_width,
                                               int* out_height,
                                               int* out_channels);

// ============================================================================
// MEMORY MANAGEMENT FUNCTIONS
// ============================================================================

/**
 * Free mesh data array allocated by USD loading functions
 * Safely deallocates all internal arrays including collision data
 *
 * IMPORTANT: Always call this function to free mesh data.
 * Do NOT use standard free() or delete[] on mesh arrays.
 *
 * Frees the following arrays for each mesh:
 * - points, indices, normals, uvs, vertex_colors
 * - collision_vertices, collision_indices
 *
 * @param meshes Pointer to mesh array to free
 * @param count Number of meshes in array
 */
ANARI_USD_MIDDLEWARE_C_API void FreeMeshData_C(CMeshData* meshes, size_t count);

/**
 * Free texture data allocated by CreateTextureFromBuffer_C
 *
 * @param texture Pointer to texture data to free
 */
ANARI_USD_MIDDLEWARE_C_API void FreeTextureData_C(CTextureData* texture);

/**
 * Free generic buffer allocated by middleware functions
 * Use this for buffers returned by GetGradientLineAsPNGBuffer_C
 *
 * @param buffer Pointer to buffer to free
 */
ANARI_USD_MIDDLEWARE_C_API void FreeBuffer_C(unsigned char* buffer);

/**
 * Free file data structure (for callback cleanup if needed)
 * Typically not needed as file data is automatically managed
 *
 * @param file_data Pointer to file data to free
 */
ANARI_USD_MIDDLEWARE_C_API void FreeFileData_C(CFileData* file_data);

// ============================================================================
// CALLBACK REGISTRATION FUNCTIONS
// ============================================================================

/**
 * Register callback function for file reception notifications
 * Only one file callback can be registered at a time
 * Subsequent calls will replace the previous callback
 *
 * The callback is called from the receiver thread context.
 * Keep callback processing minimal to avoid blocking reception.
 *
 * @param callback Function pointer to call when files are received (NULL to unregister)
 */
ANARI_USD_MIDDLEWARE_C_API void RegisterUpdateCallback_C(FileReceivedCallback_C callback);

/**
 * Register callback function for message reception notifications
 * Only one message callback can be registered at a time
 * Subsequent calls will replace the previous callback
 *
 * The callback is called from the receiver thread context.
 * Keep callback processing minimal to avoid blocking reception.
 *
 * @param callback Function pointer to call when messages are received (NULL to unregister)
 */
ANARI_USD_MIDDLEWARE_C_API void RegisterMessageCallback_C(MessageReceivedCallback_C callback);

// ============================================================================
// UTILITY AND DEBUG FUNCTIONS
// ============================================================================

/**
 * Get middleware version information
 * Returns version string with build information
 *
 * @return Pointer to version string (static, always valid)
 */
ANARI_USD_MIDDLEWARE_C_API const char* GetMiddlewareVersion_C(void);

/**
 * Validate USD file format without full processing
 * Quick check to determine if buffer contains valid USD data
 *
 * @param buffer USD data buffer to validate
 * @param buffer_size Size of buffer in bytes
 * @param filename Filename for format detection
 * @return 1 if valid USD format, 0 if invalid
 */
ANARI_USD_MIDDLEWARE_C_API int ValidateUSDFormat_C(const unsigned char* buffer,
                                                   size_t buffer_size,
                                                   const char* filename);

/**
 * Get supported USD file extensions
 * Returns comma-separated list of supported extensions
 *
 * @return Pointer to extension list string (static, always valid)
 */
ANARI_USD_MIDDLEWARE_C_API const char* GetSupportedUSDExtensions_C(void);

/**
 * Reset processing statistics
 * Clears all internal counters and statistics
 * Useful for performance monitoring and testing
 */
ANARI_USD_MIDDLEWARE_C_API void ResetProcessingStats_C(void);

/**
 * Get processing statistics as formatted string
 * Returns detailed information about processed files, meshes, errors, etc.
 *
 * @return Pointer to statistics string (valid until next call)
 */
ANARI_USD_MIDDLEWARE_C_API const char* GetProcessingStats_C(void);

// ============================================================================
// BROKER CONNECTION AND FILE REQUEST FUNCTIONS
// ============================================================================

/**
 * Connect to ANARI USD broker as DEALER client
 *
 * @param broker_endpoint Broker endpoint (e.g., "tcp://localhost:5555")
 * @param timeout_ms Connection timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int ConnectToBroker_C(
    const char* broker_endpoint,
    int timeout_ms);

/**
 * Disconnect from broker
 */
ANARI_USD_MIDDLEWARE_C_API void DisconnectFromBroker_C(void);

/**
 * Check if connected to broker
 *
 * @return 1 if connected, 0 if not connected
 */
ANARI_USD_MIDDLEWARE_C_API int IsBrokerConnected_C(void);

/**
 * Request file list from specific worker rank
 *
 * @param target_rank Target worker rank
 * @param out_files Pointer to receive array of filenames (caller must free with FreeFileList_C)
 * @param out_count Pointer to receive number of files
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestFileList_C(
    int32_t target_rank,
    char*** out_files,
    size_t* out_count,
    int timeout_ms);

/**
 * Request file list with sizes from worker rank
 *
 * @param target_rank Target worker rank
 * @param out_names Pointer to receive array of filenames (caller must free with FreeFileList_C)
 * @param out_sizes Pointer to receive array of file sizes (caller must free with FreeBuffer_C)
 * @param out_count Pointer to receive number of files
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestFileListWithSizes_C(
    int32_t target_rank,
    char*** out_names,
    uint64_t** out_sizes,
    size_t* out_count,
    int timeout_ms);

/**
 * Request file list with sizes and source ranks from worker rank(s)
 * When target_rank = -1 (broadcast), returns files from all ranks with their source ranks
 *
 * @param target_rank Target worker rank (-1 for broadcast to all ranks)
 * @param out_names Pointer to receive array of filenames (caller must free with FreeFileList_C)
 * @param out_sizes Pointer to receive array of file sizes (caller must free with FreeBuffer_C)
 * @param out_ranks Pointer to receive array of source ranks (caller must free with FreeBuffer_C)
 * @param out_count Pointer to receive number of files
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestFileListWithSizesAndRanks_C(
    int32_t target_rank,
    char*** out_names,
    uint64_t** out_sizes,
    int32_t** out_ranks,
    size_t* out_count,
    int timeout_ms);

/**
 * Free file list allocated by RequestFileList_C
 *
 * @param files Array of filenames to free
 * @param count Number of files in array
 */
ANARI_USD_MIDDLEWARE_C_API void FreeFileList_C(
    char** files,
    size_t count);

/**
 * Free file list with sizes allocated by RequestFileListWithSizes_C
 *
 * @param names Array of filenames to free
 * @param sizes Array of file sizes to free
 * @param count Number of files in array
 */
ANARI_USD_MIDDLEWARE_C_API void FreeFileListWithSizes_C(
    char** names,
    uint64_t* sizes,
    size_t count);

/**
 * Free file list with sizes and ranks allocated by RequestFileListWithSizesAndRanks_C
 *
 * @param names Array of filenames to free
 * @param sizes Array of file sizes to free
 * @param ranks Array of source ranks to free
 * @param count Number of files in array
 */
ANARI_USD_MIDDLEWARE_C_API void FreeFileListWithSizesAndRanks_C(
    char** names,
    uint64_t* sizes,
    int32_t* ranks,
    size_t count);

/**
 * Request specific file from worker rank
 *
 * @param filename Name of file to request
 * @param target_rank Target worker rank
 * @param out_data Pointer to receive file data (caller must free with FreeBuffer_C)
 * @param out_size Pointer to receive data size
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestFile_C(
    const char* filename,
    int32_t target_rank,
    unsigned char** out_data,
    size_t* out_size,
    int timeout_ms);

/**
 * Request frame (collection of files) from worker rank
 *
 * @param frame_number Frame number to request
 * @param target_rank Target worker rank
 * @param out_files Pointer to receive array of file data (caller must free with FreeFileData_C for each)
 * @param out_count Pointer to receive number of files in frame
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestFrame_C(
    int32_t frame_number,
    int32_t target_rank,
    CFileData** out_files,
    size_t* out_count,
    int timeout_ms);

/**
 * Request worker count excluding rank 0 (computational workers only)
 * Uses binary protocol REQ_WORKER_COUNT/RESP_WORKER_COUNT
 *
 * @param out_count Pointer to receive worker count (excluding rank 0)
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestWorkerCountExcludingRank0_C(
    uint32_t* out_count,
    int timeout_ms);

// ============================================================================
// WORKER LIST / COUNT FUNCTIONS (Legacy String Protocol)
// ============================================================================

/**
 * Request worker list from broker using legacy string protocol
 * Returns list of all workers including rank 0
 * Format: "rank:hostname:ip;rank:hostname:ip;..."
 *
 * @param out_worker_count Pointer to receive number of workers
 * @param out_data Buffer to receive worker list data (caller must free with FreeBuffer_C)
 * @param out_size Pointer to receive data size
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestWorkerListString_C(
    uint32_t* out_worker_count,
    unsigned char** out_data,
    size_t* out_size,
    int timeout_ms);

/**
 * Get total worker count including rank 0
 * Wrapper around RequestWorkerListString_C that just returns the count
 *
 * @param out_total_count Pointer to receive total worker count
 * @param timeout_ms Timeout in milliseconds
 * @return 1 on success, 0 on failure
 */
ANARI_USD_MIDDLEWARE_C_API int RequestTotalWorkerCount_C(
    uint32_t* out_total_count,
    int timeout_ms);

// ============================================================================
// ASYNC BROKER FUNCTIONS (NON-BLOCKING)
// ============================================================================

/**
 * Callback types for async broker operations
 */
typedef void (*WorkerCountCallback_C)(uint32_t worker_count);
typedef void (*WorkerStatusCallback_C)(int32_t target_rank, const char* status_data, size_t status_size);
typedef void (*FileListCallback_C)(int32_t target_rank, char** files, size_t file_count);
typedef void (*BrokerErrorCallback_C)(const char* error_message);

/**
 * Request total worker count asynchronously (non-blocking)
 * Calls callback on background thread when complete
 *
 * @param callback Function to call with worker count on success
 * @param error_callback Function to call on error (can be NULL)
 * @param timeout_ms Timeout in milliseconds
 */
ANARI_USD_MIDDLEWARE_C_API void RequestTotalWorkerCountAsync_C(
    WorkerCountCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);

/**
 * Request worker count asynchronously (non-blocking)
 * Calls callback on background thread when complete
 *
 * @param callback Function to call with worker count on success
 * @param error_callback Function to call on error (can be NULL)
 * @param timeout_ms Timeout in milliseconds
 */
ANARI_USD_MIDDLEWARE_C_API void RequestWorkerCountAsync_C(
    WorkerCountCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);

/**
 * Request worker status asynchronously (non-blocking)
 * Calls callback on background thread when complete
 *
 * @param target_rank Target worker rank (-1 for all workers)
 * @param callback Function to call with worker status on success
 * @param error_callback Function to call on error (can be NULL)
 * @param timeout_ms Timeout in milliseconds
 */
ANARI_USD_MIDDLEWARE_C_API void RequestWorkerStatusAsync_C(
    int32_t target_rank,
    WorkerStatusCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);

/**
 * Request file list asynchronously (non-blocking)
 * Calls callback on background thread when complete
 *
 * @param target_rank Target worker rank
 * @param callback Function to call with file list on success
 * @param error_callback Function to call on error (can be NULL)
 * @param timeout_ms Timeout in milliseconds
 */
ANARI_USD_MIDDLEWARE_C_API void RequestFileListAsync_C(
    int32_t target_rank,
    FileListCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // ANARI_USD_MIDDLEWARE_C_H
