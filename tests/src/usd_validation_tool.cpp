#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <memory>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <streambuf>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdio>

// Include both C and C++ interfaces
#include "AnariUsdMiddleware.h"
#include "AnariUsdMiddleware_C.h"
#include "FileView.h"
#include "MiddlewareLogging.h"

using namespace anari_usd_middleware;
namespace fs = std::filesystem;

// Custom stream buffer to capture and filter output
class FilteredStreamBuf : public std::streambuf {
private:
    std::streambuf* originalBuf;
    std::string buffer;
    std::ostream* outputStream;
    std::vector<std::string> filterKeywords = {
        "[INFO]", "[DEBUG]", "[WARNING]", "[VERBOSE]"
    };

public:
    FilteredStreamBuf(std::streambuf* orig, std::ostream* output)
        : originalBuf(orig), outputStream(output) {}

protected:
    int overflow(int c) override {
        if (c != EOF) {
            buffer += static_cast<char>(c);
            if (c == '\n') {
                processLine();
                buffer.clear();
            }
        }
        return c;
    }

private:
    void processLine() {
        bool shouldFilter = false;
        for (const auto& keyword : filterKeywords) {
            if (buffer.find(keyword) != std::string::npos) {
                shouldFilter = true;
                break;
            }
        }
        if (!shouldFilter && !buffer.empty()) {
            *outputStream << buffer;
            outputStream->flush();
        }
    }
};

// Test modes for GitHub Actions workflow
enum TestMode {
    FULL,
    STATIC_CHECK,
    USD_BASIC,
    GEOMETRY_HASH,
    TEXTURE_HASH,
    FILE_VIEW,
    CORE_API,
    COLLISION,
    ERROR_HANDLING,
    PERFORMANCE,
    INTEGRATION
};

static const char* ModeName(TestMode mode) {
    switch (mode) {
        case FULL: return "full";
        case STATIC_CHECK: return "static-check";
        case USD_BASIC: return "usd-basic";
        case GEOMETRY_HASH: return "geometry-hash";
        case TEXTURE_HASH: return "texture-hash";
        case FILE_VIEW: return "file-view";
        case CORE_API: return "core-api";
        case COLLISION: return "collision";
        case ERROR_HANDLING: return "error-handling";
        case PERFORMANCE: return "performance";
        case INTEGRATION: return "integration";
    }
    return "unknown";
}

// FNV-1a 64 over raw bytes: a stable digest of the exact payload a consumer
// receives, used to prove optimisations do not alter geometry.
static uint64_t HashBytes(uint64_t h, const void* data, size_t bytes) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

class USDValidator {
private:
    struct ValidationResults {
        int totalFiles = 0;
        int successfulFiles = 0;
        int failedFiles = 0;
        size_t totalMeshes = 0;
        size_t totalVertices = 0;
        size_t totalTriangles = 0;
        std::vector<std::string> failures;
        std::chrono::steady_clock::time_point startTime;

        ValidationResults() : startTime(std::chrono::steady_clock::now()) {}

        void addSuccess(const std::string& filename, size_t meshCount, size_t vertices, size_t triangles) {
            totalFiles++;
            successfulFiles++;
            totalMeshes += meshCount;
            totalVertices += vertices;
            totalTriangles += triangles;
        }

        void addFailure(const std::string& filename, const std::string& reason) {
            totalFiles++;
            failedFiles++;
            failures.emplace_back(filename + ": " + reason);
        }

        void printSummary() {
            auto endTime = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            std::cout << "\n" << std::string(60, '=') << std::endl;
            std::cout << "🎯 USD VALIDATION SUMMARY" << std::endl;
            std::cout << std::string(60, '=') << std::endl;
            std::cout << "📁 Files: " << totalFiles << " (✅ " << successfulFiles << " ❌ " << failedFiles << ")" << std::endl;
            std::cout << "📦 Meshes: " << totalMeshes << std::endl;
            std::cout << "🔺 Vertices: " << totalVertices << std::endl;
            std::cout << "🔻 Triangles: " << totalTriangles << std::endl;
            std::cout << "⏱️ Time: " << duration.count() << "ms" << std::endl;

            if (totalFiles > 0) {
                std::cout << "📊 Success Rate: " << std::fixed << std::setprecision(1)
                          << (static_cast<double>(successfulFiles) * 100.0 / totalFiles) << "%" << std::endl;
            }

            if (!failures.empty()) {
                std::cout << "\n❌ FAILURES:" << std::endl;
                for (size_t i = 0; i < failures.size(); ++i) {
                    std::cout << "   " << (i+1) << ". " << failures[i] << std::endl;
                }
            }
            std::cout << std::string(60, '=') << std::endl;
        }
    } results;

    std::vector<int> collisionTypes = {
        COLLISION_NONE,
        COLLISION_SIMPLE,
        COLLISION_CONVEX_HULL,
        COLLISION_COMPLEX,
        COLLISION_SIMPLIFIED,
        COLLISION_CONVEX_DECOMP
    };

public:
    std::string explicitFile;

    // Static checks for GitHub Actions
    int runStaticChecks() {
        std::cout << "🔍 Running static checks..." << std::endl;

        // Find middleware library
        fs::path libPath = findMiddlewareLib("libanari_usd_middleware");
        if (libPath.empty()) {
            std::cerr << "❌ Middleware library not found" << std::endl;
            return 1;
        }

        std::cout << "✅ Found middleware library at: " << libPath << std::endl;

        // Check executable linking (Linux/Mac only)
        if (!checkExecutableLinking()) {
            std::cerr << "❌ Executable has linking issues" << std::endl;
            return 1;
        }

        std::cout << "✅ Static checks passed" << std::endl;
        return 0;
    }

    // USD basic processing tests
    int runUSDBasicTests(const std::string& testDataPath) {
        std::cout << "📦 Testing basic USD processing..." << std::endl;

        std::vector<std::string> usdFiles;
        if (!explicitFile.empty()) {
            if (!fs::exists(explicitFile)) {
                std::cerr << "❌ Explicit USD file not found: " << explicitFile << std::endl;
                return 1;
            }
            usdFiles.push_back(explicitFile);
        } else {
            if (!fs::exists(testDataPath)) {
                createMinimalUSDTest(testDataPath);
            }

            usdFiles = findUSDFiles(testDataPath);
            if (usdFiles.empty()) {
                std::cout << "⚠️ No USD files found, creating minimal test" << std::endl;
                createMinimalUSDTest(testDataPath);
                usdFiles = findUSDFiles(testDataPath);
            }
        }

        if (!InitializeMiddleware_C(nullptr)) {
            std::cerr << "❌ Failed to initialize middleware" << std::endl;
            return 1;
        }

        std::cout << "📁 Found " << usdFiles.size() << " USD files" << std::endl;

        for (const auto& filePath : usdFiles) {
            processFileBasic(filePath);
        }

        ShutdownMiddleware_C();

        std::cout << "📊 USD Basic: " << results.successfulFiles << " passed, "
                  << results.failedFiles << " failed" << std::endl;

        return results.failedFiles > 0 ? 1 : 0;
    }

    // Geometry digest across the in-situ compact path (the one the UE plugin
    // uses). Prints stable hashes so before/after builds can be diffed.
    int runGeometryHashTests(const std::string& testDataPath) {
        std::cout << "🔎 Hashing geometry via Query/Fill compact path..." << std::endl;

        std::vector<std::string> usdFiles;
        if (!explicitFile.empty()) {
            if (!fs::exists(explicitFile)) {
                std::cerr << "❌ Explicit USD file not found: " << explicitFile << std::endl;
                return 1;
            }
            usdFiles.push_back(explicitFile);
        } else {
            usdFiles = findUSDFiles(testDataPath);
            if (usdFiles.empty()) {
                std::cerr << "❌ No USD files found" << std::endl;
                return 1;
            }
        }

        if (!InitializeMiddleware_C(nullptr)) {
            std::cerr << "❌ Failed to initialize middleware" << std::endl;
            return 1;
        }

        int failures = 0;
        for (const auto& filePath : usdFiles) {
            if (!hashFileGeometry(filePath)) {
                ++failures;
            }
        }

        ShutdownMiddleware_C();
        std::cout << "📊 Geometry hash: " << (usdFiles.size() - failures) << " ok, "
                  << failures << " failed" << std::endl;
        return failures > 0 ? 1 : 0;
    }

    // Builds an uncompressed BMP so the texture tests need no image files on
    // disk.  Rows are bottom-up, as BMP expects.  Both 24-bit RGB and 32-bit
    // BGRA are produced by the same pattern so their decoded pixels match.
    static std::vector<unsigned char> makeTestBmp(int width, int height,
                                                  int bitsPerPixel = 24) {
        const int bytesPerPixel = bitsPerPixel / 8;
        const int rowBytes = width * bytesPerPixel;
        const int pad = (4 - (rowBytes % 4)) % 4;
        const int imageSize = (rowBytes + pad) * height;
        std::vector<unsigned char> bmp(54 + imageSize, 0);

        auto put32 = [&bmp](int off, uint32_t v) {
            bmp[off] = static_cast<unsigned char>(v & 0xFF);
            bmp[off + 1] = static_cast<unsigned char>((v >> 8) & 0xFF);
            bmp[off + 2] = static_cast<unsigned char>((v >> 16) & 0xFF);
            bmp[off + 3] = static_cast<unsigned char>((v >> 24) & 0xFF);
        };
        auto put16 = [&bmp](int off, uint16_t v) {
            bmp[off] = static_cast<unsigned char>(v & 0xFF);
            bmp[off + 1] = static_cast<unsigned char>((v >> 8) & 0xFF);
        };

        bmp[0] = 'B';
        bmp[1] = 'M';
        put32(2, static_cast<uint32_t>(54 + imageSize));
        put32(10, 54);
        put32(14, 40);
        put32(18, static_cast<uint32_t>(width));
        put32(22, static_cast<uint32_t>(height));
        put16(26, 1);
        put16(28, static_cast<uint16_t>(bitsPerPixel));
        put32(34, static_cast<uint32_t>(imageSize));

        int off = 54;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                bmp[off++] = static_cast<unsigned char>(x * 40 + y * 7);       // B
                bmp[off++] = static_cast<unsigned char>(200 - x * 13 + y * 3); // G
                bmp[off++] = static_cast<unsigned char>(50 + x * 11 + y * 20); // R
                if (bytesPerPixel == 4) {
                    bmp[off++] = static_cast<unsigned char>(80 + x * 17 + y * 9); // A
                }
            }
            for (int p = 0; p < pad; ++p) {
                bmp[off++] = 0;
            }
        }
        return bmp;
    }

    // The handle API must return exactly the same pixels as the legacy
    // struct API for several image sizes and both RGB and RGBA BMPs.  PNG
    // encode/decode is checked in memory and through a file, and malformed
    // inputs must be rejected without crashing.
    int runTextureHashTests() {
        std::cout << "Comparing texture decode, PNG round-trips and bad input..." << std::endl;

        if (!InitializeMiddleware_C(nullptr)) {
            std::cerr << "Failed to initialize middleware" << std::endl;
            return 1;
        }

        int failures = 0;
        int total = 0;
        const struct { const char* name; int width; int height; int bpp; } cases[] = {
            {"bmp_4x3_rgb", 4, 3, 24},
            {"bmp_4x2_gradient_rgb", 4, 2, 24},
            {"bmp_5x3_rgba", 5, 3, 32},
            {"bmp_1x1_rgb", 1, 1, 24},
        };

        for (const auto& c : cases) {
            ++total;
            const std::vector<unsigned char> bmp =
                makeTestBmp(c.width, c.height, c.bpp);

            CTextureData legacy = CreateTextureFromBuffer_C(bmp.data(), bmp.size());
            CTextureHandle* handle = CreateTextureHandle_C(bmp.data(), bmp.size());

            int hw = 0, hh = 0, hc = 0;
            const unsigned char* hp = nullptr;
            size_t hs = 0;
            const bool haveHandle = handle &&
                GetTextureHandleData_C(handle, &hw, &hh, &hc, &hp, &hs) != 0;

            const bool ok = legacy.data && legacy.data_size > 0 && haveHandle &&
                legacy.width == hw && legacy.height == hh &&
                legacy.channels == hc && legacy.data_size == hs &&
                hp && std::memcmp(legacy.data, hp, hs) == 0;

            uint64_t digest = 1469598103934665603ull;
            if (haveHandle) {
                digest = HashBytes(digest, hp, hs);
            }

            std::cout << (ok ? "  ok   " : "  FAIL ") << c.name
                      << " legacy=" << legacy.width << "x" << legacy.height
                      << " handle=" << hw << "x" << hh
                      << " channels=" << hc
                      << " bytes=" << (haveHandle ? hs : 0)
                      << " hash=" << std::hex << digest << std::dec << std::endl;

            if (!ok) {
                ++failures;
            }

            FreeTextureHandle_C(handle);
            FreeTextureData_C(&legacy);
        }

        // In-memory PNG encode/decode must preserve the source pixels.
        ++total;
        {
            const std::vector<unsigned char> bmp = makeTestBmp(6, 1, 24);
            CTextureData source = CreateTextureFromBuffer_C(bmp.data(), bmp.size());

            unsigned char* png = nullptr;
            size_t pngSize = 0;
            const bool encoded = GetGradientLineAsPNGBuffer_C(
                bmp.data(), bmp.size(), &png, &pngSize) != 0;

            int pw = 0, ph = 0, pc = 0;
            const bool haveDims = encoded &&
                GetPNGDimensions_C(png, pngSize, &pw, &ph, &pc) != 0;

            CTextureData decoded = {};
            if (encoded) {
                decoded = CreateTextureFromBuffer_C(png, pngSize);
            }

            const bool ok = encoded && haveDims && source.data && decoded.data &&
                pw == source.width && ph == source.height &&
                pc == source.channels &&
                decoded.width == source.width &&
                decoded.height == source.height &&
                decoded.channels == source.channels &&
                decoded.data_size == source.data_size &&
                std::memcmp(decoded.data, source.data, source.data_size) == 0;

            std::cout << (ok ? "  ok   " : "  FAIL ")
                      << "png_memory_roundtrip"
                      << " png=" << source.width << "x" << source.height
                      << " dims=" << pw << "x" << ph << "x" << pc
                      << " bytes=" << pngSize << std::endl;

            if (!ok) {
                ++failures;
            }

            FreeBuffer_C(png);
            FreeTextureData_C(&decoded);
            FreeTextureData_C(&source);
        }

        // The file-writing variant must produce the same PNG payload.
        ++total;
        {
            const std::vector<unsigned char> bmp = makeTestBmp(6, 1, 24);
            CTextureData source = CreateTextureFromBuffer_C(bmp.data(), bmp.size());
            const fs::path pngPath = fs::current_path() / "jusync_texture_roundtrip.png";

            const bool wrote = WriteGradientLineAsPNG_C(
                bmp.data(), bmp.size(), pngPath.string().c_str()) != 0;

            std::vector<unsigned char> pngBytes;
            const bool read = wrote && readFileBytes(pngPath.string(), pngBytes);
            CTextureData decoded = {};
            if (read) {
                decoded = CreateTextureFromBuffer_C(pngBytes.data(), pngBytes.size());
            }

            const bool ok = wrote && read && source.data && decoded.data &&
                decoded.width == source.width &&
                decoded.height == source.height &&
                decoded.channels == source.channels &&
                decoded.data_size == source.data_size &&
                std::memcmp(decoded.data, source.data, source.data_size) == 0;

            std::cout << (ok ? "  ok   " : "  FAIL ")
                      << "png_file_roundtrip"
                      << " bytes=" << pngBytes.size() << std::endl;

            if (!ok) {
                ++failures;
            }

            std::error_code ec;
            fs::remove(pngPath, ec);
            FreeTextureData_C(&decoded);
            FreeTextureData_C(&source);
        }

        // Bad buffers and impossible row requests must fail cleanly.
        ++total;
        {
            bool ok = true;
            unsigned char junk[32] = {};

            CTextureData bad = CreateTextureFromBuffer_C(nullptr, 0);
            if (bad.data || bad.data_size != 0) {
                ok = false;
            }

            CTextureData badData = CreateTextureFromBuffer_C(junk, sizeof(junk));
            if (badData.data) {
                ok = false;
            }

            CTextureHandle* badHandle = CreateTextureHandle_C(junk, sizeof(junk));
            if (badHandle) {
                ok = false;
            }

            int w = 1, h = 1, c = 1;
            if (GetPNGDimensions_C(junk, sizeof(junk), &w, &h, &c) != 0) {
                ok = false;
            }

            unsigned char* row = nullptr;
            size_t rowSize = 0;
            if (GetImageRowAsPNGBuffer_C(junk, sizeof(junk), 0, &row, &rowSize) != 0 ||
                row != nullptr) {
                ok = false;
            }

            std::cout << (ok ? "  ok   " : "  FAIL ")
                      << "malformed_texture_input" << std::endl;

            if (!ok) {
                ++failures;
            }

            FreeTextureHandle_C(badHandle);
            FreeTextureData_C(&badData);
            FreeTextureData_C(&bad);
        }

        ShutdownMiddleware_C();
        std::cout << "Texture hash: " << (total - failures) << " ok, "
                  << failures << " failed" << std::endl;
        return failures > 0 ? 1 : 0;
    }


    // The transport can hand over a file as one in-place chunk or as several
    // separate frames.  A one-chunk file must be exposed without a copy; a
    // split file must be assembled once and still match the legacy download.
    int runFileViewTests() {
        std::cout << "Checking file-view assembly (alias vs owned)..." << std::endl;

        int failures = 0;
        int total = 0;
        auto check = [&](const char* name, bool ok) {
            ++total;
            if (!ok) ++failures;
            std::cout << (ok ? "  ok   " : "  FAIL ") << name << std::endl;
        };

        struct Owner {
            std::vector<uint8_t> payload;
            bool* released = nullptr;
            ~Owner() { if (released) *released = true; }
        };

        // 1. One chunk covering the whole file: alias it and retain its owner.
        {
            bool released = false;
            auto owner = std::make_shared<Owner>();
            owner->released = &released;
            owner->payload.resize(64);
            for (size_t i = 0; i < owner->payload.size(); ++i) {
                owner->payload[i] = static_cast<uint8_t>(i * 3 + 1);
            }

            const uint8_t* base = owner->payload.data();
            const size_t n = owner->payload.size();

            anari_usd_middleware::FileViewAssembler assembler;
            assembler.AddChunk(owner, base, n, 0, n);
            assembler.Complete(n);
            anari_usd_middleware::FileView view = assembler.TakeView();

            const bool aliased = view.isAliased();
            const bool samePtr = view.data() == base;
            const bool content = view.size() == n &&
                std::memcmp(view.data(), base, n) == 0;

            owner.reset();
            const bool retained = !released;
            check("alias_single_chunk", aliased && samePtr && content && retained);

            {
                anari_usd_middleware::FileView discarded = std::move(view);
                (void)discarded;
            }
            check("alias_releases_owner", released);
        }

        // 2. Two chunks must be assembled contiguously into owned storage.
        {
            auto first = std::make_shared<Owner>();
            auto second = std::make_shared<Owner>();
            first->payload = {1, 2, 3, 4};
            second->payload = {5, 6, 7, 8};
            const std::vector<uint8_t> expected = {1, 2, 3, 4, 5, 6, 7, 8};
            const uint64_t totalSize = expected.size();

            anari_usd_middleware::FileViewAssembler assembler;
            assembler.AddChunk(first, first->payload.data(), first->payload.size(),
                               0, totalSize);
            assembler.AddChunk(second, second->payload.data(), second->payload.size(),
                               first->payload.size(), totalSize);
            assembler.Complete(totalSize);
            anari_usd_middleware::FileView view = assembler.TakeView();

            bool ok = !view.isAliased() && view.size() == expected.size() &&
                std::memcmp(view.data(), expected.data(), expected.size()) == 0;

            // The assembled view must not depend on the source frames.
            first->payload.assign(first->payload.size(), 0xFF);
            second->payload.assign(second->payload.size(), 0xFF);
            ok = ok && std::memcmp(view.data(), expected.data(), expected.size()) == 0;
            check("assemble_two_chunks", ok);
        }

        // 3. Out-of-order chunks with a tail gap are placed by offset and the
        //    gap is zero-filled on completion, like RequestFileIntoBuffer_C.
        {
            std::vector<uint8_t> tail = {9, 9};
            std::vector<uint8_t> head = {1, 2};

            anari_usd_middleware::FileViewAssembler assembler;
            assembler.AddChunk(nullptr, tail.data(), tail.size(), 2, 6);
            assembler.AddChunk(nullptr, head.data(), head.size(), 0, 6);
            assembler.Complete(6);
            anari_usd_middleware::FileView view = assembler.TakeView();

            const std::vector<uint8_t> expected = {1, 2, 9, 9, 0, 0};
            const bool ok = !view.isAliased() && view.size() == expected.size() &&
                std::memcmp(view.data(), expected.data(), expected.size()) == 0;
            check("assemble_out_of_order", ok);
        }

        // 4. A single chunk shorter than the announced file must not alias, or
        //    reading the tail would run past the frame.
        {
            std::vector<uint8_t> part = {4, 5, 6};

            anari_usd_middleware::FileViewAssembler assembler;
            assembler.AddChunk(nullptr, part.data(), part.size(), 0, 8);
            assembler.Complete(8);
            anari_usd_middleware::FileView view = assembler.TakeView();

            const std::vector<uint8_t> expected = {4, 5, 6, 0, 0, 0, 0, 0};
            const bool ok = !view.isAliased() && view.size() == expected.size() &&
                std::memcmp(view.data(), expected.data(), expected.size()) == 0;
            check("short_single_chunk_not_aliased", ok);
        }

        // 5. An empty transfer produces an empty view.
        {
            anari_usd_middleware::FileViewAssembler assembler;
            assembler.Complete(0);
            anari_usd_middleware::FileView view = assembler.TakeView();
            check("empty_transfer", !view && view.size() == 0);
        }

        // 6. An announced size with no chunks arrives zero-filled.
        {
            anari_usd_middleware::FileViewAssembler assembler;
            assembler.Complete(4);
            anari_usd_middleware::FileView view = assembler.TakeView();
            const std::vector<uint8_t> zeros(4, 0);
            const bool ok = !view.isAliased() && view.size() == 4 &&
                std::memcmp(view.data(), zeros.data(), zeros.size()) == 0;
            check("complete_without_chunks", ok);
        }

        // 7. C API guards: null view reads and frees must be safe.
        {
            size_t n = 123;
            bool ok = GetFileViewData_C(nullptr, &n) == nullptr && n == 0;
            ok = ok && FileViewIsAliased_C(nullptr) == 0;
            FreeFileView_C(nullptr);
            ok = ok && RequestFileView_C(nullptr, -1, 100, 0) == nullptr;
            check("c_api_null_guards", ok);
        }

        std::cout << "File view: " << (total - failures) << " ok, "
                  << failures << " failed" << std::endl;
        return failures > 0 ? 1 : 0;
    }

    // Cross-checks every public parse/fill entry point against the in-situ
    // view path the UE plugin uses.  The sample files cover a large mesh and a
    // mesh without colours; the synthetic scene adds point clouds, vertex
    // colours and a UV set without needing another file on disk.
    int runCoreApiTests(const std::string& testDataPath) {
        std::cout << "Cross-checking disk, buffer, view and fill APIs..." << std::endl;

        std::vector<std::string> usdFiles;
        if (!explicitFile.empty()) {
            if (!fs::exists(explicitFile)) {
                std::cerr << "Explicit USD file not found: " << explicitFile << std::endl;
                return 1;
            }
            usdFiles.push_back(explicitFile);
        } else {
            usdFiles = findUSDFiles(testDataPath);
            if (usdFiles.empty()) {
                std::cerr << "No USD files found" << std::endl;
                return 1;
            }
        }

        if (!InitializeMiddleware_C(nullptr)) {
            std::cerr << "Failed to initialize middleware" << std::endl;
            return 1;
        }

        int failures = 0;
        int total = 0;
        for (const auto& filePath : usdFiles) {
            ++total;
            std::vector<unsigned char> bytes;
            if (!readFileBytes(filePath, bytes)) {
                std::cerr << "  FAIL " << filePath << ": cannot read file" << std::endl;
                ++failures;
                continue;
            }

            if (!checkCoreApiBuffer(bytes, fs::path(filePath).filename().string())) {
                ++failures;
            }
        }

        static const char* syntheticScene =
            "#usda 1.0\n"
            "def Xform \"World\"\n"
            "{\n"
            "    def Mesh \"Tri\"\n"
            "    {\n"
            "        int[] faceVertexCounts = [3, 3]\n"
            "        int[] faceVertexIndices = [0, 1, 2, 0, 2, 3]\n"
            "        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
            "        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (interpolation = \"vertex\")\n"
            "        color3f[] primvars:color = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0)] (interpolation = \"vertex\")\n"
            "        float2[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (interpolation = \"vertex\")\n"
            "        uniform token subdivisionScheme = \"none\"\n"
            "    }\n"
            "    def Points \"Cloud\"\n"
            "    {\n"
            "        point3f[] points = [(0, 1, 2), (3, 4, 5), (-1, 0.5, 2.5)]\n"
            "        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1)]\n"
            "        float[] widths = [0.1, 0.2, 0.3]\n"
            "        color3f[] primvars:color = [(1, 0, 0), (0, 1, 0), (0, 0, 1)]\n"
            "    }\n"
            "}\n";

        ++total;
        std::vector<unsigned char> synthetic(syntheticScene,
                                             syntheticScene + std::strlen(syntheticScene));
        if (!checkCoreApiBuffer(synthetic, "synthetic_core_api.usda")) {
            ++failures;
        }

        ShutdownMiddleware_C();
        std::cout << "Core API parity: " << (total - failures) << " ok, "
                  << failures << " failed" << std::endl;
        return failures > 0 ? 1 : 0;
    }

    // Collision generation tests
    int runCollisionTests(const std::string& testDataPath, const std::string& collisionType) {
        std::cout << "⚔️ Testing collision generation (" << collisionType << ")..." << std::endl;

        std::vector<std::string> usdFiles;
        if (!explicitFile.empty()) {
            if (!fs::exists(explicitFile)) {
                std::cerr << "❌ Explicit USD file not found: " << explicitFile << std::endl;
                return 1;
            }
            usdFiles.push_back(explicitFile);
        } else {
            usdFiles = findUSDFiles(testDataPath);
            if (usdFiles.empty()) {
                std::cout << "⚠️ No USD files found" << std::endl;
                return 1;
            }
        }

        if (!InitializeMiddleware_C(nullptr)) {
            std::cerr << "❌ Failed to initialize middleware" << std::endl;
            return 1;
        }

        int collisionMode = parseCollisionType(collisionType);

        for (const auto& filePath : usdFiles) {
            processFileCollision(filePath, collisionMode);
        }

        ShutdownMiddleware_C();

        std::cout << "📊 Collision Tests: " << results.successfulFiles << " passed, "
                  << results.failedFiles << " failed" << std::endl;

        return results.failedFiles > 0 ? 1 : 0;
    }

    // Error handling tests
    int runErrorHandlingTests() {
        std::cout << "🔒 Testing error handling..." << std::endl;

        bool allPassed = true;

        // Test non-existent file
        {
            if (InitializeMiddleware_C(nullptr)) {
                CMeshData* meshes = nullptr;
                size_t meshCount = 0;
                int result = LoadUSDFromDiskWithCollision_C("nonexistent.usd", COLLISION_SIMPLE, &meshes, &meshCount);
                ShutdownMiddleware_C();

                if (result == 0) {
                    std::cout << "   ✅ Non-existent file correctly rejected" << std::endl;
                } else {
                    std::cout << "   ❌ Should have failed for non-existent file" << std::endl;
                    allPassed = false;
                }
            }
        }

        // Test invalid USD data
        {
            std::string tempFile = "temp_invalid.usda";
            std::ofstream temp(tempFile);
            temp << "This is not valid USD data\nJust random text\n";
            temp.close();

            if (InitializeMiddleware_C(nullptr)) {
                CMeshData* meshes = nullptr;
                size_t meshCount = 0;
                int result = LoadUSDFromDiskWithCollision_C(tempFile.c_str(), COLLISION_SIMPLE, &meshes, &meshCount);
                ShutdownMiddleware_C();
                fs::remove(tempFile);

                if (result == 0) {
                    std::cout << "   ✅ Invalid data correctly rejected" << std::endl;
                } else {
                    std::cout << "   ❌ Should have failed for invalid data" << std::endl;
                    allPassed = false;
                }
            }
        }

        // Test the pointer-based entry points with invalid input.  These are
        // the paths used by the streaming client, so they must reject bad
        // buffers instead of returning half-filled layouts.
        {
            if (InitializeMiddleware_C(nullptr)) {
                unsigned char junk[32] = {};
                void* handle = nullptr;
                CMeshLayout* meshLayouts = nullptr;
                size_t meshCount = 0;
                CPointCloudLayout* cloudLayouts = nullptr;
                size_t cloudCount = 0;

                const bool layoutRejected = QueryUSDFullLayout_C(
                    junk, sizeof(junk), "junk.usda",
                    &handle, &meshLayouts, &meshCount,
                    &cloudLayouts, &cloudCount) == 0 && handle == nullptr;

                CMeshData* meshes = nullptr;
                size_t fullMeshCount = 0;
                CPointCloudData* clouds = nullptr;
                size_t fullCloudCount = 0;
                const bool fullRejected = LoadUSDFullFromPointer_C(
                    junk, sizeof(junk), "junk.usda",
                    &meshes, &fullMeshCount, &clouds, &fullCloudCount) == 0;

                const bool cloudRejected = ProcessPointCloudFromUSD_C(
                    junk, sizeof(junk), "junk.usda", &clouds, &fullCloudCount) == 0;

                const bool formatRejected = ValidateUSDFormat_C(
                    junk, sizeof(junk), "junk.usda") == 0;

                ShutdownMiddleware_C();

                if (layoutRejected && fullRejected && cloudRejected && formatRejected) {
                    std::cout << "   ok Invalid pointer payloads correctly rejected" << std::endl;
                } else {
                    std::cout << "   FAIL Invalid pointer payloads were accepted" << std::endl;
                    allPassed = false;
                }
            }
        }

        return allPassed ? 0 : 1;
    }

    // Performance benchmarks
    int runPerformanceTests(const std::string& testDataPath) {
        std::cout << "🚀 Running performance benchmarks..." << std::endl;

        std::vector<std::string> usdFiles;
        if (!explicitFile.empty()) {
            if (!fs::exists(explicitFile)) {
                std::cerr << "❌ Explicit USD file not found: " << explicitFile << std::endl;
                return 1;
            }
            usdFiles.push_back(explicitFile);
        } else {
            usdFiles = findUSDFiles(testDataPath);
            if (usdFiles.empty()) {
                std::cout << "⚠️ No USD files found" << std::endl;
                return 1;
            }
        }

        if (!InitializeMiddleware_C(nullptr)) {
            std::cerr << "❌ Failed to initialize middleware" << std::endl;
            return 1;
        }

        const int runs = 3;
        auto totalStart = std::chrono::high_resolution_clock::now();

        for (int run = 1; run <= runs; ++run) {
            std::cout << "🔄 Performance run " << run << "/" << runs << std::endl;

            for (const auto& filePath : usdFiles) {
                processFilePerformance(filePath);
            }
        }

        auto totalEnd = std::chrono::high_resolution_clock::now();
        auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart);

        ShutdownMiddleware_C();

        std::cout << "📊 Average processing time: " << (totalDuration.count() / runs) << "ms" << std::endl;

        return 0;
    }

    // Integration tests
    int runIntegrationTests(const std::string& testDataPath) {
        std::cout << "🔗 Running integration tests..." << std::endl;

        // Run all test types in sequence
        int result = 0;
        result |= runStaticChecks();
        result |= runUSDBasicTests(testDataPath);
        result |= runCoreApiTests(testDataPath);
        result |= runErrorHandlingTests();

        if (result == 0) {
            std::cout << "✅ All integration tests passed" << std::endl;
        } else {
            std::cout << "❌ Some integration tests failed" << std::endl;
        }

        return result;
    }

    void printResults(bool bPrintGuide = false) {
        if (results.totalFiles > 0) {
            results.printSummary();
        }

        if (bPrintGuide) {
            std::cout << "\n💡 COLLISION COMPLEXITY GUIDE:" << std::endl;
            std::cout << "   • SIMPLE: Fast bounding box collision" << std::endl;
            std::cout << "   • COMPLEX: Full mesh collision (most accurate)" << std::endl;
            std::cout << "   • SIMPLIFIED: Reduced triangle count for performance" << std::endl;
            std::cout << "   • CONVEX_HULL: Convex hull approximation" << std::endl;
            std::cout << "   • CONVEX_DECOMP: Advanced concave shape handling" << std::endl;
            std::cout << "\n🎯 Ready for Unreal Engine integration!" << std::endl;
        }
    }

    int getFailureCount() const {
        return results.failedFiles;
    }

private:
    static bool SameBytes(const void* a, size_t aBytes, const void* b, size_t bBytes) {
        if (aBytes != bBytes) return false;
        if (aBytes == 0) return true;
        if (!a || !b) return false;
        return std::memcmp(a, b, aBytes) == 0;
    }

    static unsigned char ColorToByte(float v) {
        float c = v * 255.0f;
        if (c < 0.0f) c = 0.0f;
        if (c > 255.0f) c = 255.0f;
        return static_cast<unsigned char>(c);
    }

    bool readFileBytes(const std::string& filePath, std::vector<unsigned char>& out) {
        std::ifstream in(filePath, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return !out.empty();
    }

    bool checkCoreApiBuffer(const std::vector<unsigned char>& bytes,
                            const std::string& filename) {
        struct MeshOwner {
            CMeshData* data = nullptr;
            size_t count = 0;
            ~MeshOwner() { if (data) FreeMeshData_C(data, count); }
        };
        struct CloudOwner {
            CPointCloudData* data = nullptr;
            size_t count = 0;
            ~CloudOwner() { if (data) FreePointCloudData_C(data, count); }
        };
        struct LayoutOwner {
            CMeshLayout* meshes = nullptr;
            size_t meshCount = 0;
            CPointCloudLayout* clouds = nullptr;
            size_t cloudCount = 0;
            ~LayoutOwner() {
                if (meshes || clouds) {
                    FreeUSDFullLayouts_C(meshes, meshCount, clouds, cloudCount);
                }
            }
        };
        struct ParsedOwner {
            void* handle = nullptr;
            ~ParsedOwner() { if (handle) FreeParsedUSD_C(handle); }
        };

        bool ok = true;
        auto fail = [&](const std::string& reason) {
            ok = false;
            std::cerr << "  FAIL " << filename << ": " << reason << std::endl;
        };

        if (bytes.empty()) {
            fail("empty buffer");
            return false;
        }

        if (ValidateUSDFormat_C(bytes.data(), bytes.size(), filename.c_str()) != 1) {
            fail("ValidateUSDFormat_C rejected a valid USD buffer");
        }

        const char* extensions = GetSupportedUSDExtensions_C();
        if (!extensions || !std::strstr(extensions, ".usda") ||
            !std::strstr(extensions, ".usdc") || !std::strstr(extensions, ".usdz")) {
            fail("GetSupportedUSDExtensions_C is missing a supported extension");
        }

        // Copying entry point (historical path).
        MeshOwner legacy;
        if (!LoadUSDBuffer_C(bytes.data(), bytes.size(), filename.c_str(),
                             &legacy.data, &legacy.count)) {
            fail("LoadUSDBuffer_C failed");
            return false;
        }

        // In-situ view entry point used by the UE plugin.
        MeshOwner full;
        CloudOwner fullClouds;
        if (!LoadUSDFullFromPointer_C(bytes.data(), bytes.size(), filename.c_str(),
                                      &full.data, &full.count,
                                      &fullClouds.data, &fullClouds.count)) {
            fail("LoadUSDFullFromPointer_C failed");
            return false;
        }

        if (legacy.count != full.count) {
            fail("mesh count differs between copy and view paths");
        }

        const size_t meshCount = std::min(legacy.count, full.count);
        for (size_t i = 0; i < meshCount; ++i) {
            const CMeshData& a = legacy.data[i];
            const CMeshData& b = full.data[i];
            if (std::strcmp(a.element_name, b.element_name) != 0 ||
                std::strcmp(a.type_name, b.type_name) != 0) {
                fail("mesh name/type differs between copy and view paths");
            }
            if (a.points_count != b.points_count ||
                a.indices_count != b.indices_count ||
                a.normals_count != b.normals_count ||
                a.uvs_count != b.uvs_count ||
                a.vertex_colors_count != b.vertex_colors_count) {
                fail("mesh attribute counts differ between copy and view paths");
            }
            if (!SameBytes(a.points, a.points_count * sizeof(float),
                           b.points, b.points_count * sizeof(float)) ||
                !SameBytes(a.indices, a.indices_count * sizeof(unsigned int),
                           b.indices, b.indices_count * sizeof(unsigned int)) ||
                !SameBytes(a.normals, a.normals_count * sizeof(float),
                           b.normals, b.normals_count * sizeof(float)) ||
                !SameBytes(a.uvs, a.uvs_count * sizeof(float),
                           b.uvs, b.uvs_count * sizeof(float)) ||
                !SameBytes(a.vertex_colors, a.vertex_colors_count * sizeof(float),
                           b.vertex_colors, b.vertex_colors_count * sizeof(float))) {
                fail("mesh payload differs between copy and view paths");
            }
        }

        // Layout + compact fill path, exactly what the UE stream builder uses.
        ParsedOwner parsed;
        LayoutOwner layout;
        if (!QueryUSDFullLayout_C(bytes.data(), bytes.size(), filename.c_str(),
                                  &parsed.handle,
                                  &layout.meshes, &layout.meshCount,
                                  &layout.clouds, &layout.cloudCount)) {
            fail("QueryUSDFullLayout_C failed");
            return false;
        }

        if (layout.meshCount != full.count) {
            fail("layout mesh count differs from view path");
        }
        if (layout.cloudCount != fullClouds.count) {
            fail("layout cloud count differs from view path");
        }

        const size_t compactMeshCount = std::min(layout.meshCount, full.count);
        std::vector<std::vector<float>> compactPoints(compactMeshCount);
        std::vector<std::vector<int32_t>> compactIndices(compactMeshCount);
        std::vector<std::vector<float>> compactNormals(compactMeshCount);
        std::vector<std::vector<float>> compactUvs(compactMeshCount);
        std::vector<std::vector<unsigned char>> compactColors(compactMeshCount);
        std::vector<CMeshDataFillCompact> compactMeshes(compactMeshCount);

        for (size_t i = 0; i < compactMeshCount; ++i) {
            const CMeshLayout& l = layout.meshes[i];
            const CMeshData& src = full.data[i];
            if (std::strcmp(l.element_name, src.element_name) != 0 ||
                l.points_count * 3 != src.points_count ||
                l.indices_count != src.indices_count ||
                l.normals_count * 3 != src.normals_count ||
                l.uvs_count * 2 != src.uvs_count ||
                l.vertex_colors_count != src.vertex_colors_count) {
                fail("layout counts differ from parsed view data");
            }

            compactPoints[i].assign(l.points_count * 3, 0.0f);
            compactIndices[i].assign(l.indices_count, 0);
            compactNormals[i].assign(l.normals_count * 3, 0.0f);
            compactUvs[i].assign(l.uvs_count * 2, 0.0f);
            compactColors[i].assign(l.vertex_colors_count * 4, 0);

            CMeshDataFillCompact& dst = compactMeshes[i];
            dst.points = compactPoints[i].empty() ? nullptr : compactPoints[i].data();
            dst.indices = compactIndices[i].empty() ? nullptr : compactIndices[i].data();
            dst.normals = compactNormals[i].empty() ? nullptr : compactNormals[i].data();
            dst.uvs = compactUvs[i].empty() ? nullptr : compactUvs[i].data();
            dst.vertex_colors8 = compactColors[i].empty() ? nullptr : compactColors[i].data();
        }

        if (!FillUSDFullCompact_C(parsed.handle,
                                  compactMeshes.empty() ? nullptr : compactMeshes.data(),
                                  compactMeshes.size(), nullptr, 0)) {
            fail("FillUSDFullCompact_C failed");
        } else {
            for (size_t i = 0; i < compactMeshCount; ++i) {
                const CMeshData& src = full.data[i];
                const size_t nv = std::min(src.points_count / 3,
                                           compactPoints[i].size() / 3);
                for (size_t v = 0; v < nv; ++v) {
                    const size_t o = v * 3;
                    if (compactPoints[i][o + 0] != src.points[o + 0] ||
                        compactPoints[i][o + 1] != -src.points[o + 1] ||
                        compactPoints[i][o + 2] != src.points[o + 2]) {
                        fail("compact points differ from the Z-up view convention");
                        break;
                    }
                }

                const size_t nn = std::min(src.normals_count / 3,
                                           compactNormals[i].size() / 3);
                for (size_t n = 0; n < nn; ++n) {
                    const size_t o = n * 3;
                    if (compactNormals[i][o + 0] != src.normals[o + 0] ||
                        compactNormals[i][o + 1] != -src.normals[o + 1] ||
                        compactNormals[i][o + 2] != src.normals[o + 2]) {
                        fail("compact normals differ from the Z-up view convention");
                        break;
                    }
                }

                const size_t ni = std::min(src.indices_count, compactIndices[i].size());
                for (size_t n = 0; n < ni; ++n) {
                    if (compactIndices[i][n] != static_cast<int32_t>(src.indices[n])) {
                        fail("compact indices differ from parsed indices");
                        break;
                    }
                }

                const size_t nu = std::min(src.uvs_count, compactUvs[i].size());
                for (size_t n = 0; n < nu; ++n) {
                    if (compactUvs[i][n] != src.uvs[n]) {
                        fail("compact UVs differ from parsed UVs");
                        break;
                    }
                }

            }
        }

        // Full (double) fill must agree with the compact fill.
        std::vector<std::vector<double>> fullPoints(compactMeshCount);
        std::vector<std::vector<int32_t>> fullIndices(compactMeshCount);
        std::vector<std::vector<double>> fullNormals(compactMeshCount);
        std::vector<std::vector<double>> fullUvs(compactMeshCount);
        std::vector<std::vector<unsigned char>> fullColors(compactMeshCount);
        std::vector<CMeshDataFill> fullMeshes(compactMeshCount);

        for (size_t i = 0; i < compactMeshCount; ++i) {
            const CMeshLayout& l = layout.meshes[i];
            fullPoints[i].assign(l.points_count * 3, 0.0);
            fullIndices[i].assign(l.indices_count, 0);
            fullNormals[i].assign(l.normals_count * 3, 0.0);
            fullUvs[i].assign(l.uvs_count * 2, 0.0);
            fullColors[i].assign(l.vertex_colors_count * 4, 0);

            CMeshDataFill& dst = fullMeshes[i];
            dst.points = fullPoints[i].empty() ? nullptr : fullPoints[i].data();
            dst.indices = fullIndices[i].empty() ? nullptr : fullIndices[i].data();
            dst.normals = fullNormals[i].empty() ? nullptr : fullNormals[i].data();
            dst.uvs = fullUvs[i].empty() ? nullptr : fullUvs[i].data();
            dst.vertex_colors8 = fullColors[i].empty() ? nullptr : fullColors[i].data();
        }

        if (!FillUSDFull_C(parsed.handle,
                           fullMeshes.empty() ? nullptr : fullMeshes.data(),
                           fullMeshes.size(), nullptr, 0)) {
            fail("FillUSDFull_C failed");
        } else {
            for (size_t i = 0; i < compactMeshCount; ++i) {
                if (fullPoints[i].size() != compactPoints[i].size() ||
                    fullNormals[i].size() != compactNormals[i].size() ||
                    fullUvs[i].size() != compactUvs[i].size() ||
                    fullColors[i] != compactColors[i]) {
                    fail("full-fill buffer sizing differs from compact fill");
                    continue;
                }
                for (size_t n = 0; n < fullPoints[i].size(); ++n) {
                    if (fullPoints[i][n] != static_cast<double>(compactPoints[i][n]) ||
                        (n < fullNormals[i].size() &&
                         fullNormals[i][n] != static_cast<double>(compactNormals[i][n])) ||
                        (n < fullUvs[i].size() &&
                         fullUvs[i][n] != static_cast<double>(compactUvs[i][n]))) {
                        fail("full fill differs from compact fill");
                        break;
                    }
                }
                for (size_t n = 0; n < fullIndices[i].size(); ++n) {
                    if (fullIndices[i][n] != compactIndices[i][n]) {
                        fail("full-fill indices differ from compact fill");
                        break;
                    }
                }
            }
        }

        // Point clouds: the copying extractor and the view path must match.
        CloudOwner processedClouds;
        if (!ProcessPointCloudFromUSD_C(bytes.data(), bytes.size(), filename.c_str(),
                                        &processedClouds.data, &processedClouds.count)) {
            fail("ProcessPointCloudFromUSD_C failed");
        } else {
            if (processedClouds.count != fullClouds.count) {
                fail("point-cloud count differs between copy and view paths");
            }
            const size_t cloudCount = std::min(processedClouds.count, fullClouds.count);
            for (size_t i = 0; i < cloudCount; ++i) {
                const CPointCloudData& a = processedClouds.data[i];
                const CPointCloudData& b = fullClouds.data[i];
                if (std::strcmp(a.element_name, b.element_name) != 0 ||
                    a.points_count != b.points_count ||
                    a.has_colors != b.has_colors ||
                    a.has_normals != b.has_normals ||
                    a.has_widths != b.has_widths) {
                    fail("point-cloud metadata differs between copy and view paths");
                }
                if (!SameBytes(a.positions, a.points_count * 3 * sizeof(float),
                               b.positions, b.points_count * 3 * sizeof(float)) ||
                    (a.has_colors && !SameBytes(a.colors, a.points_count * 4 * sizeof(float),
                                                b.colors, b.points_count * 4 * sizeof(float))) ||
                    (a.has_widths && !SameBytes(a.widths, a.points_count * sizeof(float),
                                                b.widths, b.points_count * sizeof(float)))) {
                    fail("point-cloud payload differs between copy and view paths");
                }
            }
        }

        for (size_t i = 0; i < std::min(layout.cloudCount, fullClouds.count); ++i) {
            const CPointCloudLayout& l = layout.clouds[i];
            const CPointCloudData& c = fullClouds.data[i];
            if (std::strcmp(l.element_name, c.element_name) != 0 ||
                l.point_count != c.points_count ||
                l.has_colors != c.has_colors ||
                l.has_normals != c.has_normals ||
                l.has_widths != c.has_widths) {
                fail("point-cloud layout differs from parsed view data");
            }
        }

        const size_t cloudCount = std::min(layout.cloudCount, fullClouds.count);
        std::vector<std::vector<double>> compactCloudPositions(cloudCount);
        std::vector<std::vector<unsigned char>> compactCloudColors(cloudCount);
        std::vector<std::vector<float>> compactCloudWidths(cloudCount);
        std::vector<CPointCloudDataFill> compactClouds(cloudCount);

        for (size_t i = 0; i < cloudCount; ++i) {
            const CPointCloudLayout& l = layout.clouds[i];
            compactCloudPositions[i].assign(l.point_count * 3, 0.0);
            compactCloudColors[i].assign(l.has_colors ? l.point_count * 4 : 0, 0);
            compactCloudWidths[i].assign(l.has_widths ? l.point_count : 0, 0.0f);

            CPointCloudDataFill& dst = compactClouds[i];
            dst.positions = compactCloudPositions[i].empty()
                ? nullptr : compactCloudPositions[i].data();
            dst.colors8 = compactCloudColors[i].empty()
                ? nullptr : compactCloudColors[i].data();
            dst.widths = compactCloudWidths[i].empty()
                ? nullptr : compactCloudWidths[i].data();
            dst.bounding_box_min[0] = dst.bounding_box_min[1] = dst.bounding_box_min[2] = 0.0f;
            dst.bounding_box_max[0] = dst.bounding_box_max[1] = dst.bounding_box_max[2] = 0.0f;
            dst.lidar_points = nullptr;
            dst.lidar_point_stride = 0;
            dst.lidar_point_version = 0;
        }

        if (cloudCount > 0 &&
            !FillUSDFullCompact_C(parsed.handle, nullptr, 0,
                                  compactClouds.data(), compactClouds.size())) {
            fail("compact point-cloud fill failed");
        } else {
            for (size_t i = 0; i < cloudCount; ++i) {
                const CPointCloudData& src = fullClouds.data[i];
                const size_t np = std::min(src.points_count,
                                           compactCloudPositions[i].size() / 3);
                for (size_t p = 0; p < np; ++p) {
                    const size_t o = p * 3;
                    if (compactCloudPositions[i][o + 0] != src.positions[o + 0] ||
                        compactCloudPositions[i][o + 1] != src.positions[o + 2] ||
                        compactCloudPositions[i][o + 2] != -src.positions[o + 1]) {
                        fail("compact point positions differ from the UE Y-up convention");
                        break;
                    }
                }
                if (src.has_colors) {
                    const size_t nc = std::min(src.points_count * 4,
                                               compactCloudColors[i].size());
                    for (size_t n = 0; n < nc; ++n) {
                        if (compactCloudColors[i][n] != ColorToByte(src.colors[n])) {
                            fail("compact point colours differ from the view path");
                            break;
                        }
                    }
                }
                if (src.has_widths) {
                    const size_t nw = std::min(src.points_count,
                                               compactCloudWidths[i].size());
                    for (size_t n = 0; n < nw; ++n) {
                        if (compactCloudWidths[i][n] != src.widths[n]) {
                            fail("compact point widths differ from the view path");
                            break;
                        }
                    }
                }
            }
        }

        std::cout << "  " << (ok ? "ok   " : "FAIL ") << filename
                  << " meshes=" << full.count
                  << " clouds=" << fullClouds.count << std::endl;
        return ok;
    }

    std::vector<std::string> findUSDFiles(const std::string& directoryPath) {
        std::vector<std::string> usdFiles;
        try {
            for (const auto& entry : fs::recursive_directory_iterator(directoryPath)) {
                if (entry.is_regular_file()) {
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz") {
                        usdFiles.push_back(entry.path().string());
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "❌ Error scanning directory: " << e.what() << std::endl;
        }
        return usdFiles;
    }

    bool hashFileGeometry(const std::string& filePath) {
        const std::string filename = fs::path(filePath).filename().string();

        std::ifstream in(filePath, std::ios::binary);
        if (!in) {
            std::cerr << "❌ Cannot open " << filePath << std::endl;
            return false;
        }
        std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
        if (buffer.empty()) {
            std::cerr << "❌ Empty file " << filePath << std::endl;
            return false;
        }

        void* handle = nullptr;
        CMeshLayout* meshLayouts = nullptr;
        size_t meshCount = 0;
        CPointCloudLayout* cloudLayouts = nullptr;
        size_t cloudCount = 0;

        if (!QueryUSDFullLayout_C(buffer.data(), buffer.size(), filename.c_str(),
                                  &handle, &meshLayouts, &meshCount,
                                  &cloudLayouts, &cloudCount)) {
            std::cerr << "❌ Layout query failed for " << filename << std::endl;
            return false;
        }

        std::vector<CMeshDataFillCompact> fillMeshes(meshCount);
        std::vector<std::vector<float>> pointStore(meshCount);
        std::vector<std::vector<int32_t>> indexStore(meshCount);
        std::vector<std::vector<float>> normalStore(meshCount);
        std::vector<std::vector<float>> uvStore(meshCount);
        std::vector<std::vector<unsigned char>> colorStore(meshCount);

        for (size_t i = 0; i < meshCount; ++i) {
            const CMeshLayout& l = meshLayouts[i];
            // Defensive sizing: the fill loops are driven by the parsed arrays,
            // which mirror these counts.
            const size_t normalCount = std::max(l.normals_count, l.points_count);
            const size_t uvCount = std::max(l.uvs_count, l.points_count);
            const size_t colorCount = std::max(l.vertex_colors_count, l.points_count);

            pointStore[i].assign(l.points_count * 3, 0.0f);
            indexStore[i].assign(l.indices_count, 0);
            normalStore[i].assign(normalCount * 3, 0.0f);
            uvStore[i].assign(uvCount * 2, 0.0f);
            colorStore[i].assign(colorCount * 4, 0);

            CMeshDataFillCompact& f = fillMeshes[i];
            f.points = pointStore[i].data();
            f.indices = indexStore[i].data();
            f.normals = l.normals_count > 0 ? normalStore[i].data() : nullptr;
            f.uvs = l.uvs_count > 0 ? uvStore[i].data() : nullptr;
            f.vertex_colors8 = l.vertex_colors_count > 0 ? colorStore[i].data() : nullptr;
        }

        std::vector<CPointCloudDataFill> fillClouds(cloudCount);
        std::vector<std::vector<double>> cloudPosStore(cloudCount);
        std::vector<std::vector<unsigned char>> cloudColorStore(cloudCount);
        std::vector<std::vector<float>> cloudWidthStore(cloudCount);

        for (size_t i = 0; i < cloudCount; ++i) {
            const CPointCloudLayout& l = cloudLayouts[i];
            cloudPosStore[i].assign(l.point_count * 3, 0.0);
            cloudColorStore[i].assign(l.point_count * 4, 0);
            cloudWidthStore[i].assign(l.point_count, 0.0f);

            CPointCloudDataFill& f = fillClouds[i];
            f.positions = cloudPosStore[i].data();
            f.colors8 = l.has_colors ? cloudColorStore[i].data() : nullptr;
            f.widths = l.has_widths ? cloudWidthStore[i].data() : nullptr;
            f.bounding_box_min[0] = f.bounding_box_min[1] = f.bounding_box_min[2] = 0.0f;
            f.bounding_box_max[0] = f.bounding_box_max[1] = f.bounding_box_max[2] = 0.0f;
            f.lidar_points = nullptr;
            f.lidar_point_stride = 0;
            f.lidar_point_version = 0;
        }

        const int fillResult = FillUSDFullCompact_C(
            handle,
            fillMeshes.empty() ? nullptr : fillMeshes.data(), fillMeshes.size(),
            fillClouds.empty() ? nullptr : fillClouds.data(), fillClouds.size());

        if (!fillResult) {
            std::cerr << "❌ Geometry fill failed for " << filename << std::endl;
            FreeUSDFullLayouts_C(meshLayouts, meshCount, cloudLayouts, cloudCount);
            FreeParsedUSD_C(handle);
            return false;
        }

        // The Y-up fill must be byte-identical to the historical (x,-y,z) fill
        // followed by UE's component swap (X, Z, Y) -> (x, z, -y).
        bool bYUpOk = true;
        {
            std::vector<CMeshDataFillCompact> fillMeshesYUp(meshCount);
            std::vector<std::vector<float>> pointStoreYUp(meshCount);
            std::vector<std::vector<int32_t>> indexStoreYUp(meshCount);
            std::vector<std::vector<float>> normalStoreYUp(meshCount);
            std::vector<std::vector<float>> uvStoreYUp(meshCount);
            std::vector<std::vector<unsigned char>> colorStoreYUp(meshCount);

            for (size_t i = 0; i < meshCount; ++i) {
                const CMeshLayout& l = meshLayouts[i];
                const size_t normalCount = std::max(l.normals_count, l.points_count);
                const size_t uvCount = std::max(l.uvs_count, l.points_count);
                const size_t colorCount = std::max(l.vertex_colors_count, l.points_count);

                pointStoreYUp[i].assign(l.points_count * 3, 0.0f);
                indexStoreYUp[i].assign(l.indices_count, 0);
                normalStoreYUp[i].assign(normalCount * 3, 0.0f);
                uvStoreYUp[i].assign(uvCount * 2, 0.0f);
                colorStoreYUp[i].assign(colorCount * 4, 0);

                CMeshDataFillCompact& f = fillMeshesYUp[i];
                f.points = pointStoreYUp[i].data();
                f.indices = indexStoreYUp[i].data();
                f.normals = l.normals_count > 0 ? normalStoreYUp[i].data() : nullptr;
                f.uvs = l.uvs_count > 0 ? uvStoreYUp[i].data() : nullptr;
                f.vertex_colors8 = l.vertex_colors_count > 0 ? colorStoreYUp[i].data() : nullptr;
            }

            const int yupResult = FillUSDFullCompactYUp_C(
                handle,
                fillMeshesYUp.empty() ? nullptr : fillMeshesYUp.data(), fillMeshesYUp.size(),
                nullptr, 0);

            if (!yupResult) {
                std::cerr << "YUP fill failed for " << filename << std::endl;
                bYUpOk = false;
            } else {
                for (size_t i = 0; i < meshCount && bYUpOk; ++i) {
                    const CMeshLayout& l = meshLayouts[i];
                    for (size_t v = 0; v < l.points_count; ++v) {
                        const size_t o = v * 3;
                        if (pointStoreYUp[i][o + 0] != pointStore[i][o + 0] ||
                            pointStoreYUp[i][o + 1] != pointStore[i][o + 2] ||
                            pointStoreYUp[i][o + 2] != pointStore[i][o + 1]) {
                            std::cerr << "YUP point v=" << v
                                      << " base=(" << pointStore[i][o + 0] << "," << pointStore[i][o + 1] << "," << pointStore[i][o + 2] << ")"
                                      << " yup=(" << pointStoreYUp[i][o + 0] << "," << pointStoreYUp[i][o + 1] << "," << pointStoreYUp[i][o + 2] << ")"
                                      << std::endl;
                            bYUpOk = false;
                            break;
                        }
                    }
                    for (size_t v = 0; v < l.normals_count && bYUpOk; ++v) {
                        const size_t o = v * 3;
                        if (normalStoreYUp[i][o + 0] != normalStore[i][o + 0] ||
                            normalStoreYUp[i][o + 1] != normalStore[i][o + 2] ||
                            normalStoreYUp[i][o + 2] != normalStore[i][o + 1]) {
                            bYUpOk = false;
                            break;
                        }
                    }
                    if (indexStoreYUp[i] != indexStore[i] ||
                        uvStoreYUp[i] != uvStore[i] ||
                        colorStoreYUp[i] != colorStore[i]) {
                        bYUpOk = false;
                    }
                }
            }
        }
        if (!bYUpOk) {
            std::cerr << "YUP mismatch for " << filename << std::endl;
        }

        const uint64_t seed = 1469598103934665603ull;
        std::cout << "H file=" << filename
                  << " meshes=" << meshCount << " clouds=" << cloudCount
                  << " yup=" << (bYUpOk ? "ok" : "FAIL") << std::endl;

        for (size_t i = 0; i < meshCount; ++i) {
            uint64_t h = seed;
            h = HashBytes(h, pointStore[i].data(), pointStore[i].size() * sizeof(float));
            h = HashBytes(h, indexStore[i].data(), indexStore[i].size() * sizeof(int32_t));
            h = HashBytes(h, normalStore[i].data(), normalStore[i].size() * sizeof(float));
            h = HashBytes(h, uvStore[i].data(), uvStore[i].size() * sizeof(float));
            h = HashBytes(h, colorStore[i].data(), colorStore[i].size());

            std::cout << "M file=" << filename << " name=" << meshLayouts[i].element_name
                      << " v=" << meshLayouts[i].points_count
                      << " i=" << meshLayouts[i].indices_count
                      << " n=" << meshLayouts[i].normals_count
                      << " u=" << meshLayouts[i].uvs_count
                      << " c=" << meshLayouts[i].vertex_colors_count
                      << " h=" << std::hex << h << std::dec << std::endl;
        }

        for (size_t i = 0; i < cloudCount; ++i) {
            uint64_t h = seed;
            h = HashBytes(h, cloudPosStore[i].data(), cloudPosStore[i].size() * sizeof(double));
            h = HashBytes(h, cloudColorStore[i].data(), cloudColorStore[i].size());
            h = HashBytes(h, cloudWidthStore[i].data(), cloudWidthStore[i].size() * sizeof(float));
            h = HashBytes(h, fillClouds[i].bounding_box_min, sizeof(float) * 3);
            h = HashBytes(h, fillClouds[i].bounding_box_max, sizeof(float) * 3);

            std::cout << "C file=" << filename << " name=" << cloudLayouts[i].element_name
                      << " pts=" << cloudLayouts[i].point_count
                      << " h=" << std::hex << h << std::dec << std::endl;
        }

        FreeUSDFullLayouts_C(meshLayouts, meshCount, cloudLayouts, cloudCount);
        FreeParsedUSD_C(handle);
        return bYUpOk;
    }

    void processFileBasic(const std::string& filePath) {        std::string filename = fs::path(filePath).filename().string();
        std::cout << " Testing " << filename << "... ";

        // Set up output filtering
        std::ostringstream cleanOutput;
        FilteredStreamBuf filteredCout(std::cout.rdbuf(), &cleanOutput);
        auto originalCout = std::cout.rdbuf(&filteredCout);

        CMeshData* meshes = nullptr;
        size_t meshCount = 0;
        int result = LoadUSDFromDiskWithCollision_C(filePath.c_str(), COLLISION_SIMPLE, &meshes, &meshCount);

        std::cout.rdbuf(originalCout);

        if (result == 1 && meshes && meshCount > 0) {
            size_t vertices = 0, triangles = 0;
            for (size_t i = 0; i < meshCount; ++i) {
                vertices += meshes[i].points_count / 3;
                triangles += meshes[i].indices_count / 3;
            }

            std::cout << "✅" << std::endl;
            results.addSuccess(filename, meshCount, vertices, triangles);
            FreeMeshData_C(meshes, meshCount);
        } else {
            std::cout << "❌" << std::endl;
            results.addFailure(filename, "Failed to load USD file");
        }
    }

    void processFileCollision(const std::string& filePath, int collisionType) {
        std::string filename = fs::path(filePath).filename().string();
        std::cout << " Testing " << filename << " collision... ";

        CMeshData* meshes = nullptr;
        size_t meshCount = 0;
        int result = LoadUSDFromDiskWithCollision_C(filePath.c_str(), collisionType, &meshes, &meshCount);

        if (result == 1 && meshes && meshCount > 0) {
            std::cout << "✅" << std::endl;
            results.addSuccess(filename, meshCount, 0, 0);
            FreeMeshData_C(meshes, meshCount);
        } else {
            std::cout << "❌" << std::endl;
            results.addFailure(filename, "Collision generation failed");
        }
    }

    void processFilePerformance(const std::string& filePath) {
        auto start = std::chrono::high_resolution_clock::now();

        CMeshData* meshes = nullptr;
        size_t meshCount = 0;
        LoadUSDFromDiskWithCollision_C(filePath.c_str(), COLLISION_SIMPLE, &meshes, &meshCount);

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::string filename = fs::path(filePath).filename().string();
        std::cout << "   " << filename << ": " << duration.count() << "ms" << std::endl;

        if (meshes) {
            FreeMeshData_C(meshes, meshCount);
        }
    }

    fs::path findMiddlewareLib(const std::string& baseName) {
        std::string libExt;
#ifdef _WIN32
        libExt = ".dll";
#elif __APPLE__
        libExt = ".dylib";
#else
        libExt = ".so";
#endif

        std::string libName = baseName + libExt;

        try {
            for (auto& p : fs::recursive_directory_iterator(fs::current_path())) {
                if (p.is_regular_file() && p.path().filename() == libName) {
                    return p.path();
                }
            }
        } catch (...) {}

        return fs::path();
    }

    bool checkExecutableLinking() {
#ifdef _WIN32
        return true;  // Skip on Windows
#else
        // Check for missing dependencies using ldd
        std::string cmd = "ldd " + fs::current_path().string() + "/usd_validation_tool 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return true;

        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            if (strstr(buffer, "not found")) {
                pclose(pipe);
                return false;
            }
        }
        pclose(pipe);
        return true;
#endif
    }

    void createMinimalUSDTest(const std::string& dir) {
        fs::create_directories(dir);
        std::ofstream minimal_file(fs::path(dir) / "minimal.usda");
        minimal_file << "#usda 1.0\n"
                    "def Mesh \"TestMesh\" {\n"
                    "  int[] faceVertexCounts = [3, 3]\n"
                    "  int[] faceVertexIndices = [0, 1, 2, 0, 2, 3]\n"
                    "  point3f[] points = [(0,0,0), (1,0,0), (1,1,0), (0,1,0)]\n"
                    "}\n";
        minimal_file.close();
    }

    int parseCollisionType(const std::string& type) {
        if (type == "simple") return COLLISION_SIMPLE;
        if (type == "complex") return COLLISION_COMPLEX;
        if (type == "convex") return COLLISION_CONVEX_HULL;
        return COLLISION_SIMPLE;
    }
};

// Parse command line arguments
TestMode parseMode(int argc, char* argv[], std::string& collisionType, std::string& explicitFile) {
    TestMode mode = FULL;
    collisionType = "simple";
    explicitFile.clear();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--mode=static-check") mode = STATIC_CHECK;
        else if (arg == "--mode=usd-basic") mode = USD_BASIC;
        else if (arg == "--mode=geometry-hash") mode = GEOMETRY_HASH;
        else if (arg == "--mode=texture-hash") mode = TEXTURE_HASH;
        else if (arg == "--mode=file-view") mode = FILE_VIEW;
        else if (arg == "--mode=core-api" || arg == "--mode=api-parity") mode = CORE_API;
        else if (arg == "--mode=collision") mode = COLLISION;
        else if (arg == "--mode=error-handling") mode = ERROR_HANDLING;
        else if (arg == "--mode=performance") mode = PERFORMANCE;
        else if (arg == "--mode=integration") mode = INTEGRATION;
        else if (arg.substr(0, 7) == "--type=") collisionType = arg.substr(7);
        else if (arg.substr(0, 2) != "--") explicitFile = arg;
    }

    return mode;
}

fs::path findTestDataDir(int argc, char* argv[]) {
    // Check if last argument is a path (not a flag)
    if (argc > 1 && std::string(argv[argc-1]).substr(0, 2) != "--") {
        fs::path p = argv[argc-1];
        // The validator scans a directory of USD files. CTest may pass a single
        // sample file, so fall back to its parent directory in that case.
        if (fs::is_regular_file(p)) {
            return p.parent_path();
        }
        return p;
    }

    // Try common test data locations
    std::vector<std::string> possible = {
        "tests/data/usd_samples",
        "../tests/data/usd_samples",
        "./tests/data/usd_samples",
        "/workspace/tests/data/usd_samples"
    };

    for (const auto& dir : possible) {
        if (fs::exists(dir)) return dir;
    }

    return "/tmp/usd_test_minimal";
}

int main(int argc, char* argv[]) {
    std::string collisionType;
    std::string explicitFile;
    TestMode mode = parseMode(argc, argv, collisionType, explicitFile);
    fs::path test_data_dir = findTestDataDir(argc, argv);

    std::cout << "🚀 USD Middleware Test Runner" << std::endl;
    std::cout << "Mode: " << ModeName(mode) << std::endl;
    std::cout << "Test Data Dir: " << test_data_dir << std::endl;
    if (!explicitFile.empty()) {
        std::cout << "Explicit File: " << explicitFile << std::endl;
    }
    std::cout << std::string(60, '-') << std::endl;

    USDValidator validator;
    validator.explicitFile = explicitFile;
    int result = 0;

    switch (mode) {
        case STATIC_CHECK:
            result = validator.runStaticChecks();
            break;

        case USD_BASIC:
            result = validator.runUSDBasicTests(test_data_dir.string());
            break;

        case GEOMETRY_HASH:
            result = validator.runGeometryHashTests(test_data_dir.string());
            break;

        case TEXTURE_HASH:
            result = validator.runTextureHashTests();
            break;

        case FILE_VIEW:
            result = validator.runFileViewTests();
            break;

        case CORE_API:
            result = validator.runCoreApiTests(test_data_dir.string());
            break;

        case COLLISION:
            result = validator.runCollisionTests(test_data_dir.string(), collisionType);
            break;

        case ERROR_HANDLING:
            result = validator.runErrorHandlingTests();
            break;

        case PERFORMANCE:
            result = validator.runPerformanceTests(test_data_dir.string());
            break;

        case INTEGRATION:
            result = validator.runIntegrationTests(test_data_dir.string());
            break;

        case FULL:
        default:
            // Run all tests in sequence
            result |= validator.runStaticChecks();
            result |= validator.runUSDBasicTests(test_data_dir.string());
            result |= validator.runCoreApiTests(test_data_dir.string());
            result |= validator.runCollisionTests(test_data_dir.string(), "simple");
            result |= validator.runErrorHandlingTests();
            result |= validator.runPerformanceTests(test_data_dir.string());
            break;
    }

    if (mode != STATIC_CHECK) {
        validator.printResults(mode == FULL);
    }

    if (result == 0) {
        std::cout << "\n🎉 ALL TESTS PASSED!" << std::endl;
    } else {
        std::cout << "\n⚠️ SOME TESTS FAILED!" << std::endl;
    }

    return result;
}
