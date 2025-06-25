#ifndef ANARI_USD_MIDDLEWARE_C_H
#define ANARI_USD_MIDDLEWARE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fix the API macro definition - only define once
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

// C-compatible structures (no STL)
typedef struct {
    char filename[256];
    unsigned char* data;
    size_t data_size;
    char hash[64];
    char file_type[32];
} CFileData;

typedef struct {
    char element_name[256];
    char type_name[128];
    float* points;              // Flat array: [x1,y1,z1, x2,y2,z2, ...]
    size_t points_count;
    unsigned int* indices;      // Triangle indices
    size_t indices_count;
    float* normals;            // Flat array: [nx1,ny1,nz1, ...]
    size_t normals_count;
    float* uvs;                // Flat array: [u1,v1, u2,v2, ...]
    size_t uvs_count;
} CMeshData;

typedef struct {
    int width;
    int height;
    int channels;
    unsigned char* data;
    size_t data_size;
} CTextureData;

// Callback function types
typedef void (*FileReceivedCallback_C)(const CFileData* file_data);
typedef void (*MessageReceivedCallback_C)(const char* message);

// Core middleware functions
ANARI_USD_MIDDLEWARE_C_API int InitializeMiddleware_C(const char* endpoint);
ANARI_USD_MIDDLEWARE_C_API void ShutdownMiddleware_C(void);
ANARI_USD_MIDDLEWARE_C_API int IsConnected_C(void);
ANARI_USD_MIDDLEWARE_C_API const char* GetStatusInfo_C(void);
ANARI_USD_MIDDLEWARE_C_API int StartReceiving_C(void);
ANARI_USD_MIDDLEWARE_C_API void StopReceiving_C(void);

// USD processing functions
ANARI_USD_MIDDLEWARE_C_API int LoadUSDBuffer_C(const unsigned char* buffer,
                                               size_t buffer_size,
                                               const char* filename,
                                               CMeshData** out_meshes,
                                               size_t* out_count);

ANARI_USD_MIDDLEWARE_C_API int LoadUSDFromDisk_C(const char* filepath,
                                                  CMeshData** out_meshes,
                                                  size_t* out_count);

// Texture processing functions
ANARI_USD_MIDDLEWARE_C_API CTextureData CreateTextureFromBuffer_C(const unsigned char* buffer,
                                                                   size_t buffer_size);

ANARI_USD_MIDDLEWARE_C_API int WriteGradientLineAsPNG_C(const unsigned char* buffer,
                                                         size_t buffer_size,
                                                         const char* output_path);

ANARI_USD_MIDDLEWARE_C_API int GetGradientLineAsPNGBuffer_C(const unsigned char* buffer,
                                                            size_t buffer_size,
                                                            unsigned char** out_png_data,
                                                            size_t* out_png_size);

// Memory management functions
ANARI_USD_MIDDLEWARE_C_API void FreeMeshData_C(CMeshData* meshes, size_t count);
ANARI_USD_MIDDLEWARE_C_API void FreeTextureData_C(CTextureData* texture);
ANARI_USD_MIDDLEWARE_C_API void FreeBuffer_C(unsigned char* buffer);
    ANARI_USD_MIDDLEWARE_C_API void FreeFileData_C(CFileData* file_data);

// Callback registration
ANARI_USD_MIDDLEWARE_C_API void RegisterUpdateCallback_C(FileReceivedCallback_C callback);
ANARI_USD_MIDDLEWARE_C_API void RegisterMessageCallback_C(MessageReceivedCallback_C callback);

#ifdef __cplusplus
}
#endif

#endif // ANARI_USD_MIDDLEWARE_C_H
