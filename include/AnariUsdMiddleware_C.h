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
 * @param endpoint ZeroMQ endpoint string (e.g., "tcp://*:5556") or NULL for default
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

#ifdef __cplusplus
}
#endif

#endif // ANARI_USD_MIDDLEWARE_C_H
