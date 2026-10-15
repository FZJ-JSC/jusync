#ifndef ANARI_USD_MIDDLEWARE_C_H
#define ANARI_USD_MIDDLEWARE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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


typedef enum {
    COLLISION_NONE = 0,           // No collision generation
    COLLISION_SIMPLE = 1,         // Bounding box collision (fastest)
    COLLISION_CONVEX_HULL = 2,    // Convex hull around mesh (balanced)
    COLLISION_COMPLEX = 3,        // Full mesh collision (most accurate, default)
    COLLISION_SIMPLIFIED = 4,     // Decimated mesh for performance (25% triangles)
    COLLISION_CONVEX_DECOMP = 5   // V-HACD convex decomposition (best for concave shapes)
} ECollisionComplexity_C;


typedef struct {
    char filename[256];          // Original filename (null-terminated)
    unsigned char* data;         // Binary file data (dynamically allocated)
    size_t data_size;           // Size of data in bytes
    char hash[64];              // SHA256 hash (null-terminated hex string)
    char file_type[32];         // File type identifier (e.g., "USD", "IMAGE")
} CFileData;

typedef struct {
    char element_name[256];      // USD primitive name (null-terminated)
    char type_name[128];         // USD primitive type (null-terminated)

    float* points;
    size_t points_count;         // Total number of floats (vertices * 3)

    unsigned int* indices;
    size_t indices_count;        // Total number of indices (triangles * 3)

    float* normals;
    size_t normals_count;        // Total number of floats (vertices * 3)

    float* uvs;
    size_t uvs_count;           // Total number of floats (vertices * 2)

    float* vertex_colors;
    size_t vertex_colors_count;  // Total number of floats (vertices * 4)

    int collision_type;          // Maps to ECollisionComplexity_C enum

    float* collision_vertices;
    size_t collision_vertices_count;  // Total number of floats (collision vertices * 3)

    unsigned int* collision_indices;
    size_t collision_indices_count;   // Total number of indices (collision triangles * 3)

    float bounding_box_min[3];   // Minimum bounds [x, y, z]
    float bounding_box_max[3];   // Maximum bounds [x, y, z]
    float sphere_center[3];      // Sphere center [x, y, z]
    float sphere_radius;         // Sphere radius

    const char* subdivision_scheme;
    int double_sided;            // Double-sided flag (0 = false, 1 = true)

    unsigned int* face_vertex_counts;
    size_t face_vertex_counts_size;

    float** uv_sets;             // Array of UV set pointers (each is flat array [u,v,...])
    const char** uv_set_names;   // Array of UV set name pointers
    size_t uv_sets_count;        // Number of UV sets

    unsigned char* vertex_colors8;

} CMeshData;

typedef struct {
    int width;                   // Image width in pixels
    int height;                  // Image height in pixels
    int channels;                // Number of channels (typically 3 or 4)
    unsigned char* data;         // Raw pixel data (dynamically allocated)
    size_t data_size;           // Size of pixel data in bytes
} CTextureData;

typedef struct {
    char element_name[256];      // USD primitive name (null-terminated)
    char type_name[128];         // Always "GeomPoints"

    float* positions;            // Flat xyz array, points_count * 3 floats
    size_t points_count;         // Number of points (not floats)

    float* normals;              // Flat xyz or NULL, points_count * 3
    float* colors;               // Flat rgba or NULL, points_count * 4, values [0.0, 1.0]
    float* widths;               // Flat float or NULL, one per point
    int has_normals;             // 1 if normals array is valid
    int has_colors;              // 1 if colors array is valid
    int has_widths;              // 1 if widths array is valid

    float bounding_box_min[3];
    float bounding_box_max[3];

    unsigned char* colors8;

} CPointCloudData;


typedef void (*FileReceivedCallback_C)(const CFileData* file_data);

typedef void (*FileReceivedSpanCallback_C)(
    const char* filename,
    const unsigned char* data,
    size_t data_size,
    const char* hash,
    const char* file_type);

typedef void (*MessageReceivedCallback_C)(const char* message);


ANARI_USD_MIDDLEWARE_C_API int InitializeMiddleware_C(const char* endpoint);

ANARI_USD_MIDDLEWARE_C_API void ShutdownMiddleware_C(void);

ANARI_USD_MIDDLEWARE_C_API int IsConnected_C(void);

ANARI_USD_MIDDLEWARE_C_API const char* GetStatusInfo_C(void);

ANARI_USD_MIDDLEWARE_C_API int StartReceiving_C(void);

ANARI_USD_MIDDLEWARE_C_API void StopReceiving_C(void);


ANARI_USD_MIDDLEWARE_C_API int LoadUSDBuffer_C(const unsigned char* buffer,
                                               size_t buffer_size,
                                               const char* filename,
                                               CMeshData** out_meshes,
                                               size_t* out_count);

ANARI_USD_MIDDLEWARE_C_API int LoadUSDFromDisk_C(const char* filepath,
                                                   CMeshData** out_meshes,
                                                   size_t* out_count);

ANARI_USD_MIDDLEWARE_C_API int LoadUSDFull_C(
    const unsigned char* buffer,
    size_t buffer_size,
    const char* filename,
    CMeshData** out_meshes,
    size_t* out_mesh_count,
    CPointCloudData** out_clouds,
    size_t* out_cloud_count);

ANARI_USD_MIDDLEWARE_C_API int LoadUSDFullFromPointer_C(
    const unsigned char* buffer,
    size_t buffer_size,
    const char* filename,
    CMeshData** out_meshes,
    size_t* out_mesh_count,
    CPointCloudData** out_clouds,
    size_t* out_cloud_count);


typedef struct {
    char element_name[256];
    char type_name[128];
    size_t points_count;        // number of vertices
    size_t indices_count;       // number of triangle indices (triangulated)
    size_t normals_count;       // number of normals (0 if absent)
    size_t uvs_count;           // number of UV pairs (0 if absent)
    size_t vertex_colors_count; // number of per-vertex colors (0 if absent)
} CMeshLayout;

typedef struct {
    char element_name[256];
    size_t point_count;
    int has_colors;
    int has_normals;
    int has_widths;
} CPointCloudLayout;

typedef struct {
    double* points;            // 3 doubles per vertex
    int32_t* indices;          // 1 int32 per index
    double* normals;           // 3 doubles per normal (nullable)
    double* uvs;               // 2 doubles per UV pair (nullable)
    unsigned char* vertex_colors8; // 4 bytes per vertex (nullable)
} CMeshDataFill;

typedef struct {
    float* points;             // 3 floats per vertex
    int32_t* indices;          // 1 int32 per index
    float* normals;            // 3 floats per normal (nullable)
    float* uvs;                // 2 floats per UV pair (nullable)
    unsigned char* vertex_colors8; // 4 bytes per vertex (nullable)
} CMeshDataFillCompact;

#pragma pack(push, 1)
typedef struct {
    float location[3];
    unsigned char color[4];
    unsigned char normal[3];
    unsigned char flags;
} CPointCloudLidarPoint_v1;
#pragma pack(pop)

typedef struct {
    double* positions;         // 3 doubles per point
    unsigned char* colors8;    // 4 bytes per point (nullable)
    float* widths;             // 1 float per point (nullable)
    float bounding_box_min[3];
    float bounding_box_max[3];

    void* lidar_points;
    int32_t lidar_point_stride;
    uint32_t lidar_point_version;
} CPointCloudDataFill;

ANARI_USD_MIDDLEWARE_C_API int QueryUSDFullLayout_C(
    const unsigned char* buffer,
    size_t buffer_size,
    const char* filename,
    void** out_handle,
    CMeshLayout** out_mesh_layouts,
    size_t* out_mesh_count,
    CPointCloudLayout** out_cloud_layouts,
    size_t* out_cloud_count);

ANARI_USD_MIDDLEWARE_C_API int FillUSDFull_C(
    void* handle,
    CMeshDataFill* meshes,
    size_t mesh_count,
    CPointCloudDataFill* clouds,
    size_t cloud_count);

ANARI_USD_MIDDLEWARE_C_API int FillUSDFullCompact_C(
    void* handle,
    CMeshDataFillCompact* meshes,
    size_t mesh_count,
    CPointCloudDataFill* clouds,
    size_t cloud_count);

ANARI_USD_MIDDLEWARE_C_API void FreeParsedUSD_C(void* handle);

ANARI_USD_MIDDLEWARE_C_API void FreeUSDFullLayouts_C(
    CMeshLayout* mesh_layouts,
    size_t mesh_count,
    CPointCloudLayout* cloud_layouts,
    size_t cloud_count);


ANARI_USD_MIDDLEWARE_C_API int LoadUSDBufferWithCollision_C(const unsigned char* buffer,
                                                            size_t buffer_size,
                                                            const char* filename,
                                                            int collision_complexity,
                                                            CMeshData** out_meshes,
                                                            size_t* out_count);

ANARI_USD_MIDDLEWARE_C_API int LoadUSDFromDiskWithCollision_C(const char* filepath,
                                                              int collision_complexity,
                                                              CMeshData** out_meshes,
                                                              size_t* out_count);


ANARI_USD_MIDDLEWARE_C_API int SetDefaultCollisionComplexity_C(int collision_complexity);

ANARI_USD_MIDDLEWARE_C_API const char* GetCollisionComplexityName_C(int collision_complexity);

ANARI_USD_MIDDLEWARE_C_API int SetCollisionParameters_C(float simplification_ratio,
                                                        float convex_hull_precision,
                                                        int max_convex_hulls);


ANARI_USD_MIDDLEWARE_C_API CTextureData CreateTextureFromBuffer_C(const unsigned char* buffer,
                                                                   size_t buffer_size);

ANARI_USD_MIDDLEWARE_C_API int WriteGradientLineAsPNG_C(const unsigned char* buffer,
                                                        size_t buffer_size,
                                                        const char* output_path);

ANARI_USD_MIDDLEWARE_C_API int GetGradientLineAsPNGBuffer_C(const unsigned char* buffer,
                                                         size_t buffer_size,
                                                         unsigned char** out_png_data,
                                                         size_t* out_png_size);

ANARI_USD_MIDDLEWARE_C_API int GetImageRowAsPNGBuffer_C(const unsigned char* buffer,
                                                     size_t buffer_size,
                                                     int row_index,
                                                     unsigned char** out_png_data,
                                                     size_t* out_png_size);

ANARI_USD_MIDDLEWARE_C_API int GetPNGDimensions_C(const unsigned char* buffer,
                                               size_t buffer_size,
                                               int* out_width,
                                               int* out_height,
                                               int* out_channels);


ANARI_USD_MIDDLEWARE_C_API void FreeMeshData_C(CMeshData* meshes, size_t count);

ANARI_USD_MIDDLEWARE_C_API void FreeTextureData_C(CTextureData* texture);

ANARI_USD_MIDDLEWARE_C_API void FreeBuffer_C(unsigned char* buffer);

ANARI_USD_MIDDLEWARE_C_API void FreeFileData_C(CFileData* file_data);

ANARI_USD_MIDDLEWARE_C_API void FreeFrameFiles_C(CFileData* files, size_t count);


ANARI_USD_MIDDLEWARE_C_API void RegisterUpdateCallback_C(FileReceivedCallback_C callback);

ANARI_USD_MIDDLEWARE_C_API void RegisterUpdateCallbackSpan_C(FileReceivedSpanCallback_C callback);

ANARI_USD_MIDDLEWARE_C_API void RegisterMessageCallback_C(MessageReceivedCallback_C callback);


typedef enum {
    CNotificationType_FileUpdate = 300,     // A file has been updated on the broker
    CNotificationType_CommitComplete = 301  // A scene commit is complete
} CNotificationType;

typedef void (*NotificationCallback_C)(uint32_t message_type,
                                         int32_t source_rank,
                                         const char* filename,
                                         uint64_t file_size,
                                         uint64_t timestamp,
                                         uint64_t hashLo,
                                         uint64_t hashHi,
                                         uint64_t hashPrevLo,
                                         uint64_t hashPrevHi,
                                         bool hasOldData);

ANARI_USD_MIDDLEWARE_C_API void RegisterNotificationCallback_C(NotificationCallback_C callback);


typedef enum {
    CSceneChange_None = 0,
    CSceneChange_Created = 1,
    CSceneChange_Removed = 2,
    CSceneChange_Visibility = 3,
    CSceneChange_Transform = 4,
    CSceneChange_Material = 5,
    CSceneChange_Attribute = 6,
    CSceneChange_Commit = 7,
    CSceneChange_Property = 8
} CSceneChangeType;

typedef enum {
    CPropertyValue_None = 0,
    CPropertyValue_Int = 1,
    CPropertyValue_Bool = 2,
    CPropertyValue_Float = 3,
    CPropertyValue_Float2 = 4,
    CPropertyValue_Float3 = 5,
    CPropertyValue_Float4 = 6,
    CPropertyValue_String = 7,
    CPropertyValue_Path = 8,
    CPropertyValue_ArrayRef = 9
} CPropertyValueType;

typedef void (*SceneUpdateCallback_C)(
    uint32_t message_type,
    int32_t source_rank,
    uint64_t timestamp,
    uint64_t commit_id,
    uint64_t revision,
    const char* prim_path,
    const char* property_name,
    int32_t change_type,
    int32_t value_type,
    int64_t int_value,
    float float_value,
    const float* vec4,
    const char* string_value,
    uint32_t payload_size);

ANARI_USD_MIDDLEWARE_C_API void RegisterSceneUpdateCallback_C(SceneUpdateCallback_C callback);

typedef void (*ProtocolDiagnosticsCallback_C)(
    const char* event,
    const char* message,
    uint64_t value0,
    uint64_t value1);

ANARI_USD_MIDDLEWARE_C_API void RegisterProtocolDiagnosticsCallback_C(ProtocolDiagnosticsCallback_C callback);

ANARI_USD_MIDDLEWARE_C_API uint32_t GetProtocolVersion_C(void);


ANARI_USD_MIDDLEWARE_C_API const char* GetMiddlewareVersion_C(void);

ANARI_USD_MIDDLEWARE_C_API int ValidateUSDFormat_C(const unsigned char* buffer,
                                                   size_t buffer_size,
                                                   const char* filename);

ANARI_USD_MIDDLEWARE_C_API const char* GetSupportedUSDExtensions_C(void);


ANARI_USD_MIDDLEWARE_C_API int ConnectToBroker_C(
    const char* broker_endpoint,
    int timeout_ms);

ANARI_USD_MIDDLEWARE_C_API void DisconnectFromBroker_C(void);

ANARI_USD_MIDDLEWARE_C_API int IsBrokerConnected_C(void);

ANARI_USD_MIDDLEWARE_C_API int RequestFileList_C(
    int32_t target_rank,
    char*** out_files,
    size_t* out_count,
    int timeout_ms);

ANARI_USD_MIDDLEWARE_C_API int RequestFileListWithSizes_C(
    int32_t target_rank,
    char*** out_names,
    uint64_t** out_sizes,
    size_t* out_count,
    int timeout_ms);

ANARI_USD_MIDDLEWARE_C_API int RequestFileListWithSizesAndRanks_C(
    int32_t target_rank,
    char*** out_names,
    uint64_t** out_sizes,
    int32_t** out_ranks,
    uint64_t** out_hash_lo,
    uint64_t** out_hash_hi,
    size_t* out_count,
    int timeout_ms);

ANARI_USD_MIDDLEWARE_C_API void FreeFileList_C(
    char** files,
    size_t count);

ANARI_USD_MIDDLEWARE_C_API void FreeFileListWithSizes_C(
    char** names,
    uint64_t* sizes,
    size_t count);

ANARI_USD_MIDDLEWARE_C_API void FreeFileListWithSizesAndRanks_C(
    char** names,
    uint64_t* sizes,
    int32_t* ranks,
    uint64_t* hash_lo,
    uint64_t* hash_hi,
    size_t count);

ANARI_USD_MIDDLEWARE_C_API int RequestFile_C(
    const char* filename,
    int32_t target_rank,
    unsigned char** out_data,
    size_t* out_size,
    int timeout_ms);

ANARI_USD_MIDDLEWARE_C_API int64_t RequestFileIntoBuffer_C(
    const char* filename,
    int32_t target_rank,
    int timeout_ms,
    unsigned char* out_buffer,
    size_t out_capacity,
    size_t* out_size);

ANARI_USD_MIDDLEWARE_C_API int64_t RequestFileIntoBufferAbort_C(
    const char* filename,
    int32_t target_rank,
    int timeout_ms,
    unsigned char* out_buffer,
    size_t out_capacity,
    size_t* out_size,
    uint64_t abort_handle);


ANARI_USD_MIDDLEWARE_C_API uint64_t JusyncAbortCreate_C(void);

ANARI_USD_MIDDLEWARE_C_API int32_t JusyncAbortTrigger_C(uint64_t handle);

ANARI_USD_MIDDLEWARE_C_API void JusyncAbortRelease_C(uint64_t handle);


typedef void (*AsyncFileComplete_C)(
    const char* filename,
    const unsigned char* data,
    size_t data_size,
    int success,
    void* userData);

ANARI_USD_MIDDLEWARE_C_API int AsyncRequestFile_C(
    const char* filename,
    int32_t target_rank,
    AsyncFileComplete_C completeCallback,
    void* userData,
    int timeout_ms);

ANARI_USD_MIDDLEWARE_C_API int RequestFrame_C(
    int32_t frame_number,
    int32_t target_rank,
    CFileData** out_files,
    size_t* out_count,
    int timeout_ms);

ANARI_USD_MIDDLEWARE_C_API int RequestWorkerCountExcludingRank0_C(
    uint32_t* out_count,
    int timeout_ms);

// WORKER LIST / COUNT FUNCTIONS (Legacy String Protocol)


 ANARI_USD_MIDDLEWARE_C_API int RequestWorkerListString_C(
     uint32_t* out_worker_count,
     unsigned char** out_data,
     size_t* out_size,
     int timeout_ms);

typedef void (*WorkerListCallback_C)(uint32_t count, const int32_t* ranks, const char** hostnames, const char** ips);

ANARI_USD_MIDDLEWARE_C_API int RequestWorkerListStringCallback_C(
    WorkerListCallback_C callback,
    int timeout_ms);

ANARI_USD_MIDDLEWARE_C_API int RequestTotalWorkerCount_C(
    uint32_t* out_total_count,
    int timeout_ms);


typedef void (*WorkerCountCallback_C)(uint32_t worker_count);
typedef void (*WorkerStatusCallback_C)(int32_t target_rank, const char* status_data, size_t status_size);
typedef void (*FileListCallback_C)(int32_t target_rank, char** files, size_t file_count);
typedef void (*BrokerErrorCallback_C)(const char* error_message);

ANARI_USD_MIDDLEWARE_C_API void RequestTotalWorkerCountAsync_C(
    WorkerCountCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);

ANARI_USD_MIDDLEWARE_C_API void RequestWorkerCountAsync_C(
    WorkerCountCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);

ANARI_USD_MIDDLEWARE_C_API void RequestWorkerStatusAsync_C(
    int32_t target_rank,
    WorkerStatusCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);

ANARI_USD_MIDDLEWARE_C_API void RequestFileListAsync_C(
    int32_t target_rank,
    FileListCallback_C callback,
    BrokerErrorCallback_C error_callback,
    int timeout_ms);


typedef void (*ParallelFileReceivedCallback_C)(const char* filename, const unsigned char* data, size_t data_size);

typedef void (*ParallelDownloadCompleteCallback_C)(void);

typedef void (*ParallelDownloadErrorCallback_C)(const char* filename, const char* error_message);

ANARI_USD_MIDDLEWARE_C_API void RequestFilesParallelAsync_C(
    const char** filenames,
    size_t filename_count,
    const int32_t* target_ranks,
    ParallelFileReceivedCallback_C file_received_callback,
    ParallelDownloadCompleteCallback_C completion_callback,
    ParallelDownloadErrorCallback_C error_callback,
    int timeout_ms);

ANARI_USD_MIDDLEWARE_C_API int VerifyParallelDownloadDLL_C();

ANARI_USD_MIDDLEWARE_C_API int RequestFilesParallelDirect_C(
    const char** filenames,
    size_t filename_count,
    const int32_t* target_ranks,
    ParallelFileReceivedCallback_C file_received_callback,
    ParallelDownloadCompleteCallback_C completion_callback,
    ParallelDownloadErrorCallback_C error_callback,
    int timeout_ms);


ANARI_USD_MIDDLEWARE_C_API int ProcessPointCloudFromUSD_C(
    const unsigned char* buffer,
    size_t buffer_size,
    const char* filename,
    CPointCloudData** out_clouds,
    size_t* out_count);

ANARI_USD_MIDDLEWARE_C_API int GetCachedGradientTexture_C(
    unsigned char** gradient_png_data,
    size_t* out_png_size,
    int* out_width,
    int* out_height);

ANARI_USD_MIDDLEWARE_C_API void FreePointCloudData_C(CPointCloudData* clouds, size_t count);

ANARI_USD_MIDDLEWARE_C_API void FreeCachedGradientTexture_C(unsigned char* gradient_rgba);

typedef enum JusyncDiagStage {
    JUSYNC_DIAG_NOTIFY      = 0,
    JUSYNC_DIAG_QUEUE       = 1,
    JUSYNC_DIAG_DL_START    = 2,
    JUSYNC_DIAG_DL_DONE     = 3,
    JUSYNC_DIAG_PARSE_START = 4,
    JUSYNC_DIAG_PARSE_DONE  = 5,
    JUSYNC_DIAG_FILL_DONE   = 6,
    JUSYNC_DIAG_SPAWNED     = 7
} JusyncDiagStage;

ANARI_USD_MIDDLEWARE_C_API void JusyncDiagMark_C(const char* filename, int stage,
                                                 uint64_t bytes, uint64_t hashLo, uint64_t hashHi);

ANARI_USD_MIDDLEWARE_C_API int JusyncDiagDump_C(char* outBuf, int outBufSize, int lastN);

ANARI_USD_MIDDLEWARE_C_API double JusyncDiagTotalMs_C(const char* filename);

ANARI_USD_MIDDLEWARE_C_API void JusyncDiagClear_C(void);

ANARI_USD_MIDDLEWARE_C_API void JusyncDiagSetEnabled_C(int enabled);

#ifdef __cplusplus
}
#endif

#endif // ANARI_USD_MIDDLEWARE_C_H
