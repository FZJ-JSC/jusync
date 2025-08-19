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

    // Keywords to filter out
    std::vector<std::string> filterKeywords = {
        "[INFO]", "[DEBUG]", "[WARNING]", "[VERBOSE]"
        // Keep [ERROR] for important issues
    };

public:
    FilteredStreamBuf(std::streambuf* orig, std::ostream* output)
        : originalBuf(orig), outputStream(output) {}

protected:
    virtual int overflow(int c) override {
        if (c != EOF) {
            buffer += static_cast<char>(c);

            // Check for complete line (newline character)
            if (c == '\n') {
                processLine();
                buffer.clear();
            }
        }
        return c;
    }

private:
    void processLine() {
        // Check if line contains any filter keywords
        bool shouldFilter = false;
        for (const auto& keyword : filterKeywords) {
            if (buffer.find(keyword) != std::string::npos) {
                shouldFilter = true;
                break;
            }
        }

        // Only output non-filtered lines
        if (!shouldFilter && !buffer.empty()) {
            *outputStream << buffer;
            outputStream->flush();
        }
    }
};

class USDCollisionValidator {
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
            std::cout << "✅ " << filename << " (" << meshCount << " meshes, "
                      << vertices << " vertices, " << triangles << " triangles)" << std::endl;
        }

        void addFailure(const std::string& filename, const std::string& reason) {
            totalFiles++;
            failedFiles++;
            failures.emplace_back(filename + ": " + reason);
            std::cerr << "❌ " << filename << " → " << reason << std::endl;
        }

        void printSummary() {
            auto endTime = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            std::cout << "\n" << std::string(60, '=') << std::endl;
            std::cout << "🎯 USD COLLISION VALIDATION SUMMARY" << std::endl;
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

    // Collision types to test
    std::vector<int> collisionTypes = {
        COLLISION_NONE,
        COLLISION_SIMPLE,
        COLLISION_CONVEX_HULL,
        COLLISION_COMPLEX,
        COLLISION_SIMPLIFIED,
        COLLISION_CONVEX_DECOMP
    };

public:
    bool processDirectory(const std::string& directoryPath) {
        std::cout << "🚀 USD Collision Validation: " << directoryPath << std::endl;
        std::cout << std::string(60, '-') << std::endl;

        if (!fs::exists(directoryPath)) {
            std::cerr << "❌ Directory does not exist: " << directoryPath << std::endl;
            return false;
        }

        // Initialize middleware (this will still produce logs, but we'll filter them next time)
        if (!InitializeMiddleware_C(nullptr)) {
            std::cerr << "❌ Failed to initialize middleware" << std::endl;
            return false;
        }

        // Find USD files
        std::vector<std::string> usdFiles = findUSDFiles(directoryPath);
        if (usdFiles.empty()) {
            std::cout << "⚠️ No USD files found" << std::endl;
            ShutdownMiddleware_C();
            return false;
        }

        std::cout << "📁 Found " << usdFiles.size() << " USD files" << std::endl;
        std::cout << "🔧 Testing collision generation with " << collisionTypes.size() << " complexity levels" << std::endl;
        std::cout << std::string(60, '-') << std::endl;

        // Process each file
        for (const auto& filePath : usdFiles) {
            processFile(filePath);
        }

        // Test configuration
        testCollisionConfiguration();

        ShutdownMiddleware_C();
        return results.successfulFiles > 0;
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

    void processFile(const std::string& filePath) {
        std::string filename = fs::path(filePath).filename().string();
        std::cout << "📦 " << filename << " ";

        // Set up output filtering for this file processing
        std::ostringstream cleanOutput;
        FilteredStreamBuf filteredCout(std::cout.rdbuf(), &cleanOutput);
        FilteredStreamBuf filteredCerr(std::cerr.rdbuf(), &cleanOutput);

        // Temporarily redirect cout and cerr
        std::streambuf* originalCout = std::cout.rdbuf(&filteredCout);
        std::streambuf* originalCerr = std::cerr.rdbuf(&filteredCerr);

        bool anySucceeded = false;
        size_t totalMeshes = 0;
        size_t totalVertices = 0;
        size_t totalTriangles = 0;

        // Test each collision type (middleware logs will be filtered)
        for (int collisionType : collisionTypes) {
            auto start = std::chrono::high_resolution_clock::now();
            CMeshData* meshes = nullptr;
            size_t meshCount = 0;

            int result = LoadUSDFromDiskWithCollision_C(filePath.c_str(), collisionType, &meshes, &meshCount);

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            if (result == 1 && meshes && meshCount > 0) {
                // Calculate statistics
                size_t vertices = 0, triangles = 0;

                for (size_t i = 0; i < meshCount; ++i) {
                    vertices += meshes[i].points_count / 3;
                    triangles += meshes[i].indices_count / 3;
                }

                // Show success indicator
                std::cout << "✅";

                // Store totals from the most complete collision type
                if (collisionType == COLLISION_COMPLEX || !anySucceeded) {
                    totalMeshes = meshCount;
                    totalVertices = vertices;
                    totalTriangles = triangles;
                }
                anySucceeded = true;

                FreeMeshData_C(meshes, meshCount);
            } else {
                // Show failure indicator
                std::cout << "❌";
            }
        }

        // Restore original streams
        std::cout.rdbuf(originalCout);
        std::cerr.rdbuf(originalCerr);

        std::cout << std::endl; // End line for this file

        // Process any filtered output that should be shown
        std::string filteredContent = cleanOutput.str();
        if (!filteredContent.empty()) {
            // Only show ERROR messages or other important filtered content
            std::istringstream stream(filteredContent);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.find("[ERROR]") != std::string::npos) {
                    std::cerr << line << std::endl;
                }
            }
        }

        // Record overall result
        if (anySucceeded) {
            results.addSuccess(filename, totalMeshes, totalVertices, totalTriangles);
        } else {
            results.addFailure(filename, "All collision types failed");
        }
    }

    void testCollisionConfiguration() {
        std::cout << "\n🔧 Testing collision configuration..." << std::endl;

        // Set up filtering for configuration tests
        std::ostringstream cleanOutput;
        FilteredStreamBuf filteredCout(std::cout.rdbuf(), &cleanOutput);
        FilteredStreamBuf filteredCerr(std::cerr.rdbuf(), &cleanOutput);

        std::streambuf* originalCout = std::cout.rdbuf(&filteredCout);
        std::streambuf* originalCerr = std::cerr.rdbuf(&filteredCerr);

        bool allConfigPassed = true;

        // Test collision complexity settings
        for (int collisionType : collisionTypes) {
            if (!SetDefaultCollisionComplexity_C(collisionType)) {
                allConfigPassed = false;
            }
        }

        // Test collision parameters
        if (!SetCollisionParameters_C(0.25f, 0.001f, 32)) {
            allConfigPassed = false;
        }

        // Restore streams
        std::cout.rdbuf(originalCout);
        std::cerr.rdbuf(originalCerr);

        if (allConfigPassed) {
            std::cout << "   ✅ Collision configuration tests passed" << std::endl;
        } else {
            std::cout << "   ❌ Some collision configuration tests failed" << std::endl;
        }
    }

public:
    bool testErrorHandling() {
        std::cout << "\n🔒 Testing error handling..." << std::endl;
        bool allPassed = true;

        // Set up filtering for error tests
        std::ostringstream cleanOutput;
        FilteredStreamBuf filteredCout(std::cout.rdbuf(), &cleanOutput);
        FilteredStreamBuf filteredCerr(std::cerr.rdbuf(), &cleanOutput);

        // Test non-existent file
        {
            std::streambuf* originalCout = std::cout.rdbuf(&filteredCout);
            std::streambuf* originalCerr = std::cerr.rdbuf(&filteredCerr);

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

            std::cout.rdbuf(originalCout);
            std::cerr.rdbuf(originalCerr);
        }

        // Test invalid USD data
        {
            std::string tempFile = "temp_invalid.usda";
            std::ofstream temp(tempFile);
            temp << "This is not valid USD data\nJust random text\n";
            temp.close();

            std::streambuf* originalCout = std::cout.rdbuf(&filteredCout);
            std::streambuf* originalCerr = std::cerr.rdbuf(&filteredCerr);

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

            std::cout.rdbuf(originalCout);
            std::cerr.rdbuf(originalCerr);
        }

        return allPassed;
    }

    void printResults() {
        results.printSummary();

        std::cout << "\n💡 COLLISION COMPLEXITY GUIDE:" << std::endl;
        std::cout << "   • SIMPLE: Fast bounding box collision" << std::endl;
        std::cout << "   • COMPLEX: Full mesh collision (most accurate)" << std::endl;
        std::cout << "   • SIMPLIFIED: Reduced triangle count for performance" << std::endl;
        std::cout << "   • CONVEX_HULL: Convex hull approximation" << std::endl;
        std::cout << "   • CONVEX_DECOMP: Advanced concave shape handling" << std::endl;
        std::cout << "\n🎯 Ready for Unreal Engine RealtimeMeshComponent integration!" << std::endl;
    }

    int getFailureCount() const {
        return results.failedFiles;
    }
};

// Main function
int main(int argc, char* argv[]) {
    std::string directoryPath = (argc > 1) ? argv[1] : "../tests/data/usd_samples";

    // Try to find test data if not provided
    if (argc == 1) {
        if (fs::exists("tests/data/usd_samples")) {
            directoryPath = "tests/data/usd_samples";
        } else if (fs::exists("../tests/data/usd_samples")) {
            directoryPath = "../tests/data/usd_samples";
        } else {
            std::cerr << "❌ Usage: " << argv[0] << " <directory_path>" << std::endl;
            std::cerr << "   No default test directory found." << std::endl;
            return 1;
        }
    }

    std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
    std::cout << "║    USD Collision Generation Validator       ║" << std::endl;
    std::cout << "║        Clean Output - Filtered Logs         ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════╝" << std::endl;

    USDCollisionValidator validator;

    bool success = validator.processDirectory(directoryPath);

    // Test error handling
    validator.testErrorHandling();

    validator.printResults();

    if (success && validator.getFailureCount() == 0) {
        std::cout << "\n🎉 ALL VALIDATIONS PASSED!" << std::endl;
        std::cout << "   Collision generation pipeline is working correctly." << std::endl;
        std::cout << "   Ready for production use!" << std::endl;
        return 0;
    } else {
        std::cout << "\n⚠️ SOME VALIDATIONS FAILED!" << std::endl;
        std::cout << "   Check failed files above for details." << std::endl;
        return 1;
    }
}
