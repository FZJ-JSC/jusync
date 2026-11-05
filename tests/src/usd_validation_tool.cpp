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

// Include both C and C++ interfaces
#include "AnariUsdMiddleware.h"
#include "AnariUsdMiddleware_C.h"
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
    COLLISION,
    ERROR_HANDLING,
    PERFORMANCE,
    INTEGRATION
};

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

    // Builds an uncompressed 24-bit BMP so the texture tests need no image
    // files on disk.  Rows are bottom-up, as BMP expects.
    static std::vector<unsigned char> makeTestBmp(int width, int height) {
        const int rowBytes = width * 3;
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
        put16(28, 24);
        put32(34, static_cast<uint32_t>(imageSize));

        int off = 54;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                bmp[off++] = static_cast<unsigned char>(x * 40 + y * 7);       // B
                bmp[off++] = static_cast<unsigned char>(200 - x * 13 + y * 3); // G
                bmp[off++] = static_cast<unsigned char>(50 + x * 11 + y * 20); // R
            }
            for (int p = 0; p < pad; ++p) {
                bmp[off++] = 0;
            }
        }
        return bmp;
    }

    // The handle API must return exactly the same pixels as the legacy
    // struct API, for both a normal image and the two-row gradient case.
    int runTextureHashTests() {
        std::cout << "🧪 Comparing legacy and handle texture decode..." << std::endl;

        if (!InitializeMiddleware_C(nullptr)) {
            std::cerr << "❌ Failed to initialize middleware" << std::endl;
            return 1;
        }

        int failures = 0;
        const struct { const char* name; int width; int height; } cases[] = {
            {"bmp_4x3", 4, 3},
            {"bmp_4x2_gradient", 4, 2},
        };

        for (const auto& c : cases) {
            const std::vector<unsigned char> bmp = makeTestBmp(c.width, c.height);

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

            std::cout << (ok ? "  ✅ " : "  ❌ ") << c.name
                      << " legacy=" << legacy.width << "x" << legacy.height
                      << " handle=" << hw << "x" << hh
                      << " bytes=" << (haveHandle ? hs : 0)
                      << " hash=" << std::hex << digest << std::dec << std::endl;

            if (!ok) {
                ++failures;
            }

            FreeTextureHandle_C(handle);
            FreeTextureData_C(&legacy);
        }

        ShutdownMiddleware_C();
        std::cout << "📊 Texture hash: " << (2 - failures) << " ok, "
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
    std::cout << "Mode: " << mode << std::endl;
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
