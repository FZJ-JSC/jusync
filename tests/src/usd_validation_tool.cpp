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
    COLLISION,
    ERROR_HANDLING,
    PERFORMANCE,
    INTEGRATION
};

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

    void processFileBasic(const std::string& filePath) {
        std::string filename = fs::path(filePath).filename().string();
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
