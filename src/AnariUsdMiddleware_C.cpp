#include "AnariUsdMiddleware_C.h"
#include "AnariUsdMiddleware.h"
#include <memory>
#include <cstring>
#include <glm/glm.hpp>

// Global middleware instance

static std::unique_ptr<anari_usd_middleware::AnariUsdMiddleware> g_middleware;
static FileReceivedCallback_C g_file_callback = nullptr;
static MessageReceivedCallback_C g_message_callback = nullptr;

extern "C" {

int InitializeMiddleware_C(const char* endpoint) {
    try {
        if (!g_middleware) {
            g_middleware = std::make_unique<anari_usd_middleware::AnariUsdMiddleware>();
        }

        std::string endpoint_str = endpoint ? endpoint : "tcp://*:5556";
        bool result = g_middleware->initialize(endpoint_str.c_str());

        // ✅ CRITICAL FIX: Only register callbacks after successful initialization
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
                        c_data.filename[255] = '\0';
                        c_data.hash[63] = '\0';
                        c_data.file_type[31] = '\0';
                        #endif

                        c_data.data_size = file_data.data.size();
                        if (c_data.data_size > 0) {
                            c_data.data = new unsigned char[c_data.data_size];
                            std::memcpy(c_data.data, file_data.data.data(), c_data.data_size);
                        } else {
                            c_data.data = nullptr;
                        }

                        // ✅ FIXED: Call the callback
                        g_file_callback(&c_data);

                        // ✅ FIXED: DON'T delete memory immediately - let Unreal handle it
                        // The memory will be cleaned up when Unreal calls FreeFileData_C
                        // if (c_data.data) {
                        //     delete[] c_data.data;  // COMMENTED OUT - This was causing the access violation
                        // }
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
        return 0;
    }
}


void ShutdownMiddleware_C() {
    if (g_middleware) {
        g_middleware->shutdown();
        g_middleware.reset();
    }
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

int LoadUSDBuffer_C(const unsigned char* buffer, size_t buffer_size, const char* filename, CMeshData** out_meshes, size_t* out_count) {
    if (!g_middleware || !buffer || !filename || !out_meshes || !out_count) {
        return 0;
    }

    try {
        std::vector<uint8_t> std_buffer(buffer, buffer + buffer_size);
        std::string std_filename(filename);
        std::vector<anari_usd_middleware::AnariUsdMiddleware::MeshData> mesh_data;

        bool result = g_middleware->LoadUSDBuffer(std_buffer, std_filename, mesh_data);

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

            // Safe string copying
            #ifdef _WIN32
            strncpy_s(dst.element_name, sizeof(dst.element_name), src.elementName.c_str(), 255);
            strncpy_s(dst.type_name, sizeof(dst.type_name), src.typeName.c_str(), 127);
            #else
            std::strncpy(dst.element_name, src.elementName.c_str(), 255);
            std::strncpy(dst.type_name, src.typeName.c_str(), 127);
            dst.element_name[255] = '\0';
            dst.type_name[127] = '\0';
            #endif

            // ✅ FIXED: Copy points (src.points is already a flat float array)
            dst.points_count = src.points.size();
            if (dst.points_count > 0) {
                dst.points = new float[dst.points_count];
                std::memcpy(dst.points, src.points.data(), dst.points_count * sizeof(float));
            } else {
                dst.points = nullptr;
            }

            // Copy indices
            dst.indices_count = src.indices.size();
            if (dst.indices_count > 0) {
                dst.indices = new unsigned int[dst.indices_count];
                std::memcpy(dst.indices, src.indices.data(), dst.indices_count * sizeof(unsigned int));
            } else {
                dst.indices = nullptr;
            }

            // ✅ FIXED: Copy normals (src.normals is already a flat float array)
            dst.normals_count = src.normals.size();
            if (dst.normals_count > 0) {
                dst.normals = new float[dst.normals_count];
                std::memcpy(dst.normals, src.normals.data(), dst.normals_count * sizeof(float));
            } else {
                dst.normals = nullptr;
            }

            // ✅ FIXED: Copy UVs (src.uvs is already a flat float array)
            dst.uvs_count = src.uvs.size();
            if (dst.uvs_count > 0) {
                dst.uvs = new float[dst.uvs_count];
                std::memcpy(dst.uvs, src.uvs.data(), dst.uvs_count * sizeof(float));
            } else {
                dst.uvs = nullptr;
            }
        }

        return 1;
    } catch (...) {
        *out_count = 0;
        *out_meshes = nullptr;
        return 0;
    }
}


int LoadUSDFromDisk_C(const char* filepath, CMeshData** out_meshes, size_t* out_count) {
    if (!g_middleware || !filepath || !out_meshes || !out_count) {
        return 0;
    }

    try {
        std::string std_filepath(filepath);
        std::vector<anari_usd_middleware::AnariUsdMiddleware::MeshData> mesh_data;

        bool result = g_middleware->LoadUSDFromDisk(std_filepath, mesh_data);

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

            // Safe string copying
            #ifdef _WIN32
            strncpy_s(dst.element_name, sizeof(dst.element_name), src.elementName.c_str(), 255);
            strncpy_s(dst.type_name, sizeof(dst.type_name), src.typeName.c_str(), 127);
            #else
            std::strncpy(dst.element_name, src.elementName.c_str(), 255);
            std::strncpy(dst.type_name, src.typeName.c_str(), 127);
            dst.element_name[255] = '\0';
            dst.type_name[127] = '\0';
            #endif

            // ✅ FIXED: Direct memory copy for flat arrays
            dst.points_count = src.points.size();
            if (dst.points_count > 0) {
                dst.points = new float[dst.points_count];
                std::memcpy(dst.points, src.points.data(), dst.points_count * sizeof(float));
            } else {
                dst.points = nullptr;
            }

            dst.indices_count = src.indices.size();
            if (dst.indices_count > 0) {
                dst.indices = new unsigned int[dst.indices_count];
                std::memcpy(dst.indices, src.indices.data(), dst.indices_count * sizeof(unsigned int));
            } else {
                dst.indices = nullptr;
            }

            dst.normals_count = src.normals.size();
            if (dst.normals_count > 0) {
                dst.normals = new float[dst.normals_count];
                std::memcpy(dst.normals, src.normals.data(), dst.normals_count * sizeof(float));
            } else {
                dst.normals = nullptr;
            }

            dst.uvs_count = src.uvs.size();
            if (dst.uvs_count > 0) {
                dst.uvs = new float[dst.uvs_count];
                std::memcpy(dst.uvs, src.uvs.data(), dst.uvs_count * sizeof(float));
            } else {
                dst.uvs = nullptr;
            }
        }

        return 1;
    } catch (...) {
        *out_count = 0;
        *out_meshes = nullptr;
        return 0;
    }
}


CTextureData CreateTextureFromBuffer_C(const unsigned char* buffer, size_t buffer_size) {
    CTextureData result = {};

    if (!g_middleware || !buffer) {
        return result;
    }

    try {
        std::vector<uint8_t> std_buffer(buffer, buffer + buffer_size);
        anari_usd_middleware::AnariUsdMiddleware::TextureData tex_data = g_middleware->CreateTextureFromBuffer(std_buffer);

        result.width = tex_data.width;
        result.height = tex_data.height;
        result.channels = tex_data.channels;
        result.data_size = tex_data.data.size();

        if (result.data_size > 0) {
            result.data = new unsigned char[result.data_size];
            std::memcpy(result.data, tex_data.data.data(), result.data_size);
        }

    } catch (...) {
        // Return empty result
    }

    return result;
}

int WriteGradientLineAsPNG_C(const unsigned char* buffer, size_t buffer_size, const char* output_path) {
    if (!g_middleware || !buffer || !output_path) {
        return 0;
    }

    try {
        std::vector<uint8_t> std_buffer(buffer, buffer + buffer_size);
        std::string std_path(output_path);
        return g_middleware->WriteGradientLineAsPNG(std_buffer, std_path) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int GetGradientLineAsPNGBuffer_C(const unsigned char* buffer, size_t buffer_size, unsigned char** out_buffer, size_t* out_size) {
    if (!g_middleware || !buffer || !out_buffer || !out_size) {
        return 0;
    }

    try {
        std::vector<uint8_t> std_buffer(buffer, buffer + buffer_size);
        std::vector<uint8_t> png_buffer;

        bool result = g_middleware->GetGradientLineAsPNGBuffer(std_buffer, png_buffer);

        if (result && !png_buffer.empty()) {
            *out_size = png_buffer.size();
            *out_buffer = new unsigned char[*out_size];
            std::memcpy(*out_buffer, png_buffer.data(), *out_size);
            return 1;
        }

    } catch (...) {
        // Fall through to return 0
    }

    *out_buffer = nullptr;
    *out_size = 0;
    return 0;
}

void FreeMeshData_C(CMeshData* meshes, size_t count) {
    if (!meshes) return;

    for (size_t i = 0; i < count; ++i) {
        delete[] meshes[i].points;
        delete[] meshes[i].indices;
        delete[] meshes[i].normals;
        delete[] meshes[i].uvs;
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

void RegisterUpdateCallback_C(FileReceivedCallback_C callback) {
    g_file_callback = callback;
}

void RegisterMessageCallback_C(MessageReceivedCallback_C callback) {
    g_message_callback = callback;
}

    void FreeFileData_C(CFileData* file_data) {
    if (file_data && file_data->data) {
        delete[] file_data->data;
        file_data->data = nullptr;
        file_data->data_size = 0;
    }
}

} // extern "C"
