#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// AnariUsdMiddleware - public C++ API (thread-safe, UE-friendly)
// Now includes collision-generation support (simple → complex)
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <shared_mutex>

#include "MiddlewareLogging.h"
#include "CollisionProcessor.h"     // NEW: collision types / data

#ifndef ANARI_USD_MIDDLEWARE_API
# ifdef _WIN32
#   ifdef ANARI_USD_MIDDLEWARE_EXPORTS
#     define ANARI_USD_MIDDLEWARE_API __declspec(dllexport)
#   else
#     define ANARI_USD_MIDDLEWARE_API __declspec(dllimport)
#   endif
# else
#   define ANARI_USD_MIDDLEWARE_API __attribute__((visibility("default")))
# endif
#endif

namespace anari_usd_middleware {

// ─────────────────────────────────────────────────────────────────────────────
// Core safety limits (mirrors MiddlewareLogging.h > safety namespace)
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {
    namespace safety = anari_usd_middleware::safety;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public data structures
// ─────────────────────────────────────────────────────────────────────────────
struct FileData {
    std::string              filename;
    std::vector<uint8_t>     data;
    std::string              hash;       // SHA-256 (hex)
    std::string              fileType;   // "USD", "IMAGE", ...
    bool isValid() const {
        return !filename.empty() &&
               !data.empty() &&
               data.size() <= detail::safety::MAX_BUFFER_SIZE &&
               !hash.empty() && !fileType.empty();
    }
    void clear() { filename.clear(); data.clear(); hash.clear(); fileType.clear(); }
};

/* -------------------------------- Mesh / Collision ------------------------ */

struct MeshData {
    std::string          elementName;   // prim name
    std::string          typeName;      // prim type
    std::vector<float>   points;        // xyz…
    std::vector<uint32_t>indices;       // tris
    std::vector<float>   normals;       // xyz…
    std::vector<float>   uvs;           // uv…
    std::vector<float>   vertex_colors; // rgba…
    /* NEW: collision */
    CollisionData        collision;

    bool isValid() const {
        return !elementName.empty() &&
               !points.empty() &&
               !indices.empty() &&
               points.size() % 3 == 0 &&
               indices.size() % 3 == 0 &&
               (normals.empty() || normals.size() % 3 == 0) &&
               (uvs.empty()     || uvs.size() % 2  == 0) &&
               (vertex_colors.empty() || vertex_colors.size() % 4 == 0);
    }
    size_t getVertexCount()   const { return points.size()   / 3; }
    size_t getTriangleCount() const { return indices.size()  / 3; }
    void clear() { *this = MeshData(); }
};

/* -------------------------------- Texture --------------------------------- */
struct TextureData {
    int                    width{0}, height{0}, channels{0};
    std::vector<uint8_t>   data;
    bool isValid() const {
        return width>0 && height>0 && channels>0 &&
               data.size() == static_cast<size_t>(width*height*channels);
    }
    void clear() { *this = TextureData(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Callback typedefs
// ─────────────────────────────────────────────────────────────────────────────
using FileUpdateCallback = std::function<void(const FileData&)>;
using MessageCallback    = std::function<void(const std::string&)>;

// ─────────────────────────────────────────────────────────────────────────────
class ANARI_USD_MIDDLEWARE_API AnariUsdMiddleware {
public:
    /* construction */
    AnariUsdMiddleware();
    ~AnariUsdMiddleware();
    AnariUsdMiddleware(const AnariUsdMiddleware&)            = delete;
    AnariUsdMiddleware& operator=(const AnariUsdMiddleware&) = delete;

    /* connection / lifecycle */
    bool initialize(const char* endpoint = nullptr);
    void shutdown();
    bool isConnected() const;

    /* callbacks */
    int  registerUpdateCallback (FileUpdateCallback cb);
    void unregisterUpdateCallback(int id);
    int  registerMessageCallback(MessageCallback cb);
    void unregisterMessageCallback(int id);

    /* receiver thread */
    bool startReceiving();
    void stopReceiving();

    /* texture helpers */
    TextureData CreateTextureFromBuffer(const std::vector<uint8_t>& buffer);

    /* ---------------- USD loading (visual-only) -------------------------- */
    bool LoadUSDBuffer(const std::vector<uint8_t>& buffer,
                       const std::string& fileName,
                       std::vector<MeshData>& outMeshes);

    bool LoadUSDFromDisk(const std::string& filePath,
                         std::vector<MeshData>& outMeshes);

    /* ---------------- USD loading WITH collision ------------------------- */
    bool LoadUSDBufferWithCollision(const std::vector<uint8_t>& buffer,
                                    const std::string& fileName,
                                    ECollisionComplexity complexity,
                                    std::vector<MeshData>& outMeshes);

    bool LoadUSDFromDiskWithCollision(const std::string& filePath,
                                      ECollisionComplexity complexity,
                                      std::vector<MeshData>& outMeshes);

    /* collision configuration */
    void  setDefaultCollisionComplexity(ECollisionComplexity cpx);
    ECollisionComplexity getDefaultCollisionComplexity() const;

    void  setCollisionParameters(float simplificationRatio,
                                 float convexHullPrecision,
                                 int   maxConvexHulls);

    /* gradient helpers */
    bool WriteGradientLineAsPNG(const std::vector<uint8_t>& buffer,
                                const std::string& outPath);
    bool GetGradientLineAsPNGBuffer(const std::vector<uint8_t>& buffer,
                                    std::vector<uint8_t>& outPng);

    /* status / stats */
    std::string getStatusInfo() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;

    /* thread-safety for public state */
    mutable std::shared_mutex statusMutex;
    std::atomic<bool> initialized{false};
    std::atomic<bool> shutdownRequested{false};
};

} // namespace anari_usd_middleware
