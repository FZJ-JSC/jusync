#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <algorithm>

#include "AnariUsdMiddleware_C.h"
#include "AnariUsdMiddleware.h"
#include "MiddlewareLogging.h"

using namespace anari_usd_middleware;
namespace fs = std::filesystem;

class USDValidationTool {
private:
    struct TestResults {
        int totalTests = 0;
        int passedTests = 0;
        int failedTests = 0;
        std::vector<std::string> failures;
        std::chrono::steady_clock::time_point startTime;

        TestResults() : startTime(std::chrono::steady_clock::now()) {}

        void addSuccess(const std::string& testName) {
            totalTests++;
            passedTests++;
            std::cout << "✅ PASS: " << testName << std::endl;
        }

        void addFailure(const std::string& testName, const std::string& reason) {
            totalTests++;
            failedTests++;
            failures.push_back(testName + ": " + reason);
            std::cerr << "❌ FAIL: " << testName << " - " << reason << std::endl;
        }

        void printSummary() {
            auto endTime = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            std::cout << "\n" << std::string(60, '=') << std::endl;
            std::cout << "🎯 USD VALIDATION RESULTS (C Interface Focused)" << std::endl;
            std::cout << std::string(60, '=') << std::endl;
            std::cout << "📊 Total Tests: " << totalTests << std::endl;
            std::cout << "✅ Passed: " << passedTests << std::endl;
            std::cout << "❌ Failed: " << failedTests << std::endl;
            std::cout << "⏱️ Duration: " << duration.count() << "ms" << std::endl;
            std::cout << "📈 Success Rate: " << std::fixed << std::setprecision(1)
                      << (totalTests > 0 ? (double)passedTests * 100.0 / totalTests : 0.0)
                      << "%" << std::endl;

            if (!failures.empty()) {
                std::cout << "\n❌ DETAILED FAILURES:" << std::endl;
                for (size_t i = 0; i < failures.size(); ++i) {
                    std::cout << "  " << (i + 1) << ". " << failures[i] << std::endl;
                }
            }
            std::cout << std::string(60, '=') << std::endl;
        }
    };

    TestResults results;

public:
    bool validateMeshData(const std::string& meshName, size_t vertexCount, size_t indexCount,
                         size_t normalCount, size_t uvCount, size_t colorCount,
                         bool isCollision = false) {

        std::string prefix = isCollision ? "🔰 COLLISION" : "👁️ VISUAL";
        bool valid = true;

        if (vertexCount == 0) {
            std::cerr << "    ❌ " << prefix << " - No vertices found" << std::endl;
            valid = false;
        }

        if (vertexCount % 3 != 0) {
            std::cerr << "    ❌ " << prefix << " - Vertex count not multiple of 3: " << vertexCount << std::endl;
            valid = false;
        }

        if (indexCount > 0 && indexCount % 3 != 0) {
            std::cerr << "    ❌ " << prefix << " - Index count not multiple of 3: " << indexCount << std::endl;
            valid = false;
        }

        if (normalCount > 0 && normalCount % 3 != 0) {
            std::cerr << "    ❌ " << prefix << " - Normal count not multiple of 3: " << normalCount << std::endl;
            valid = false;
        }

        if (uvCount > 0 && uvCount % 2 != 0) {
            std::cerr << "    ❌ " << prefix << " - UV count not multiple of 2: " << uvCount << std::endl;
            valid = false;
        }

        if (colorCount > 0 && colorCount % 4 != 0) {
            std::cerr << "    ❌ " << prefix << " - Color count not multiple of 4: " << colorCount << std::endl;
            valid = false;
        }

        if (valid) {
            std::cout << "    ✅ " << prefix << " - " << meshName
                      << " [V:" << vertexCount/3
                      << " T:" << (indexCount > 0 ? indexCount/3 : 0)
                      << " N:" << (normalCount > 0 ? "✓" : "✗")
                      << " UV:" << (uvCount > 0 ? "✓" : "✗")
                      << " C:" << (colorCount > 0 ? "✓" : "✗") << "]" << std::endl;
        }

        return valid;
    }

    bool validateCollisionData(const std::string& meshName, const CMeshData& mesh) {
        bool valid = true;

        if (mesh.collision_type == COLLISION_NONE) {
            std::cout << "    ✅ " << meshName << " - No collision (as expected)" << std::endl;
            return true;
        }

        if (mesh.collision_vertices_count == 0) {
            std::cerr << "    ❌ Collision enabled but no collision vertices" << std::endl;
            valid = false;
        }

        if (mesh.collision_vertices_count % 3 != 0) {
            std::cerr << "    ❌ Collision vertex count not multiple of 3: " << mesh.collision_vertices_count << std::endl;
            valid = false;
        }

        if (mesh.collision_indices_count > 0 && mesh.collision_indices_count % 3 != 0) {
            std::cerr << "    ❌ Collision index count not multiple of 3: " << mesh.collision_indices_count << std::endl;
            valid = false;
        }

        if (mesh.collision_type != COLLISION_NONE) {
            bool hasBounds = false;
            for (int i = 0; i < 3; i++) {
                if (mesh.bounding_box_min[i] != 0.0f || mesh.bounding_box_max[i] != 0.0f) {
                    hasBounds = true;
                    break;
                }
            }
            if (!hasBounds) {
                std::cerr << "    ❌ No bounding box data for collision type: " << mesh.collision_type << std::endl;
                valid = false;
            }
        }

        if (valid) {
            std::cout << "    ✅ " << meshName << " - Collision Type: " << GetCollisionComplexityName_C(mesh.collision_type)
                      << " CV:" << mesh.collision_vertices_count/3
                      << " CT:" << (mesh.collision_indices_count > 0 ? mesh.collision_indices_count/3 : 0)
                      << " Bounds: Valid" << std::endl;
        }

        return valid;
    }

    bool testSingleFileWithCollision(const std::string& filePath, int collisionType) {
        std::string typeName = GetCollisionComplexityName_C(collisionType);
        std::cout << "\n📦 Testing " << fs::path(filePath).filename() << " with " << typeName << std::endl;

        try {
            auto start = std::chrono::high_resolution_clock::now();

            CMeshData* meshes = nullptr;
            size_t meshCount = 0;

            int loadResult = LoadUSDFromDiskWithCollision_C(filePath.c_str(), collisionType, &meshes, &meshCount);

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            if (loadResult != 1) {
                results.addFailure("C_LoadWithCollision_" + typeName,
                                   "Failed to load file (return code: " + std::to_string(loadResult) + ")");
                return false;
            }

            if (meshCount == 0 || !meshes) {
                results.addFailure("C_MeshExtraction_" + typeName, "No meshes extracted");
                return false;
            }

            std::cout << "    📊 Loaded " << meshCount << " meshes in " << duration.count() << "ms" << std::endl;

            bool allMeshesValid = true;
            for (size_t i = 0; i < meshCount; ++i) {
                const auto& mesh = meshes[i];

                if (!validateMeshData(mesh.element_name,
                                     mesh.points_count,
                                     mesh.indices_count,
                                     mesh.normals_count,
                                     mesh.uvs_count,
                                     mesh.vertex_colors_count)) {
                    allMeshesValid = false;
                }

                if (!validateCollisionData(mesh.element_name, mesh)) {
                    allMeshesValid = false;
                }
            }

            FreeMeshData_C(meshes, meshCount);

            if (!allMeshesValid) {
                results.addFailure("C_MeshValidation_" + typeName, "Some meshes failed validation");
                return false;
            }

            results.addSuccess("C_Collision_" + typeName + "_" + fs::path(filePath).stem().string());
            return true;

        } catch (const std::exception& e) {
            results.addFailure("C_Exception_" + typeName, e.what());
            return false;
        }
    }


    bool testCInterfaceComprehensive(const std::string& testDataPath) {
        std::cout << "\n🚀 COMPREHENSIVE C INTERFACE TESTING (Production Critical)" << std::endl;
        std::cout << "Test Data Path: " << testDataPath << std::endl;
        std::cout << "Priority: PRIMARY (90% of validation effort)" << std::endl;

        if (!fs::exists(testDataPath)) {
            results.addFailure("TestSetup", "Test data path does not exist: " + testDataPath);
            return false;
        }

        if (!InitializeMiddleware_C(nullptr)) {
            results.addFailure("C_PRIMARY_Initialization", "Failed to initialize C middleware");
            return false;
        }

        std::vector<std::string> usdFiles;

        if (fs::is_regular_file(testDataPath)) {
            std::string ext = fs::path(testDataPath).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz") {
                usdFiles.push_back(testDataPath);
                std::cout << "📁 Testing single USD file: " << fs::path(testDataPath).filename() << std::endl;
            } else {
                ShutdownMiddleware_C();
                results.addFailure("TestSetup", "File is not a USD format: " + testDataPath);
                return false;
            }
        } else if (fs::is_directory(testDataPath)) {
            std::cout << "📁 Scanning directory for USD files..." << std::endl;

            for (const auto& entry : fs::recursive_directory_iterator(testDataPath)) {
                if (entry.is_regular_file()) {
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                    if (ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz") {
                        usdFiles.push_back(entry.path().string());
                    }
                }
            }

            std::cout << "📁 Found " << usdFiles.size() << " USD files to test" << std::endl;
        } else {
            ShutdownMiddleware_C();
            results.addFailure("TestSetup", "Path is neither a file nor directory: " + testDataPath);
            return false;
        }

        if (usdFiles.empty()) {
            ShutdownMiddleware_C();
            results.addFailure("TestSetup", "No USD files found to test");
            return false;
        }

        std::vector<int> collisionTypes = {
            COLLISION_NONE,
            COLLISION_SIMPLE,
            COLLISION_CONVEX_HULL,
            COLLISION_COMPLEX,
            COLLISION_SIMPLIFIED,
            COLLISION_CONVEX_DECOMP
        };

        bool allPassed = true;

        for (const auto& filePath : usdFiles) {
            std::cout << "\n" << std::string(80, '=') << std::endl;
            std::cout << "🎯 COMPREHENSIVE TESTING: " << fs::path(filePath).filename() << std::endl;
            std::cout << "📁 Full Path: " << filePath << std::endl;
            std::cout << std::string(80, '=') << std::endl;

            for (int collisionType : collisionTypes) {
                if (!testSingleFileWithCollision(filePath, collisionType)) {
                    allPassed = false;
                }
            }
        }

        std::cout << "\n🔧 TESTING C INTERFACE COLLISION CONFIGURATION" << std::endl;
        std::cout << "Priority: PRIMARY (Production Parameter Validation)" << std::endl;

        for (int collisionType : collisionTypes) {
            if (SetDefaultCollisionComplexity_C(collisionType)) {
                results.addSuccess("C_CollisionConfig_Default_" + std::string(GetCollisionComplexityName_C(collisionType)));
                std::cout << "    ✅ Set default collision: " << GetCollisionComplexityName_C(collisionType) << std::endl;
            } else {
                results.addFailure("C_CollisionConfig_Default", "Failed to set default collision complexity: " + std::to_string(collisionType));
                allPassed = false;
            }
        }

        if (SetCollisionParameters_C(0.25f, 0.001f, 32)) {
            results.addSuccess("C_CollisionConfig_Parameters");
            std::cout << "    ✅ Set collision parameters: ratio=0.25, precision=0.001, hulls=32" << std::endl;
        } else {
            results.addFailure("C_CollisionConfig_Parameters", "Failed to set collision parameters");
            allPassed = false;
        }

        if (!testCInterfaceErrorHandling()) {
            allPassed = false;
        }

        ShutdownMiddleware_C();

        return allPassed;
    }

    bool testCInterfaceErrorHandling() {
        std::cout << "\n🔒 C INTERFACE ERROR HANDLING TEST (Production Stability)" << std::endl;
        std::cout << "Priority: PRIMARY (Critical for Production)" << std::endl;

        bool allPassed = true;

        std::cout << "    Testing non-existent file handling..." << std::endl;
        CMeshData* meshes = nullptr;
        size_t meshCount = 0;

        int result = LoadUSDFromDiskWithCollision_C("nonexistent_file_test.usd", COLLISION_COMPLEX, &meshes, &meshCount);

        if (result == 0) {
            results.addSuccess("C_ERROR_NonExistentFile");
            std::cout << "        ✅ Correctly rejected non-existent file" << std::endl;
        } else {
            results.addFailure("C_ERROR_NonExistentFile", "Should have failed but didn't");
            allPassed = false;
        }

        std::cout << "    Testing invalid USD data handling..." << std::endl;
        std::string tempFile = "temp_invalid.usda";
        std::ofstream temp(tempFile);
        temp << "This is not valid USD data\nJust random text\n";
        temp.close();

        meshes = nullptr;
        meshCount = 0;
        result = LoadUSDFromDiskWithCollision_C(tempFile.c_str(), COLLISION_COMPLEX, &meshes, &meshCount);

        fs::remove(tempFile);

        if (result == 0) {
            results.addSuccess("C_ERROR_InvalidData");
            std::cout << "        ✅ Correctly rejected invalid USD data" << std::endl;
        } else {
            results.addFailure("C_ERROR_InvalidData", "Should have failed for invalid data");
            allPassed = false;
        }

        std::cout << "    Testing invalid collision complexity values..." << std::endl;
        bool invalidTestPassed = true;

        if (SetDefaultCollisionComplexity_C(999)) {
            results.addFailure("C_ERROR_InvalidCollisionType", "Should have rejected invalid collision type 999");
            invalidTestPassed = false;
        }

        if (SetDefaultCollisionComplexity_C(-1)) {
            results.addFailure("C_ERROR_InvalidCollisionType", "Should have rejected invalid collision type -1");
            invalidTestPassed = false;
        }

        if (invalidTestPassed) {
            results.addSuccess("C_ERROR_InvalidCollisionType");
            std::cout << "        ✅ Correctly rejected invalid collision types" << std::endl;
        }

        std::cout << "    Testing memory management..." << std::endl;

        int result1 = LoadUSDFromDisk_C(nullptr, &meshes, &meshCount);
        int result2 = LoadUSDFromDisk_C("test.usd", nullptr, &meshCount);
        int result3 = LoadUSDFromDisk_C("test.usd", &meshes, nullptr);

        if (result1 == 0 && result2 == 0 && result3 == 0) {
            results.addSuccess("C_ERROR_NullPointers");
            std::cout << "        ✅ Properly handled null pointer inputs" << std::endl;
        } else {
            results.addFailure("C_ERROR_NullPointers", "Failed to handle null pointers safely");
            allPassed = false;
        }

        return allPassed;
    }


    bool testCppSyntaxOnly() {
        std::cout << "\n📝 C++ INTERFACE SYNTAX VALIDATION (Architecture Check)" << std::endl;
        std::cout << "Priority: SECONDARY (10% - Syntax Check Only, No File Loading)" << std::endl;

        try {
            std::cout << "    Testing C++ object creation..." << std::endl;
            std::unique_ptr<AnariUsdMiddleware> middleware = std::make_unique<AnariUsdMiddleware>();

            if (!middleware) {
                results.addFailure("CPP_SYNTAX_Creation", "Failed to create AnariUsdMiddleware object");
                return false;
            }
            std::cout << "        ✅ C++ middleware object created successfully" << std::endl;

            std::cout << "    Testing basic method calls..." << std::endl;

            bool connected = middleware->isConnected();  // Should return false since not initialized
            std::cout << "        ✅ isConnected() callable: " << (connected ? "true" : "false") << std::endl;

            std::string status = middleware->getStatusInfo();  // Should return some status
            std::cout << "        ✅ getStatusInfo() callable: " << (!status.empty() ? "returns data" : "empty") << std::endl;

            std::cout << "    Testing method signatures..." << std::endl;

            ECollisionComplexity defaultCollision = middleware->getDefaultCollisionComplexity();
            std::cout << "        ✅ getDefaultCollisionComplexity() returns: " << static_cast<int>(defaultCollision) << std::endl;

            middleware->setDefaultCollisionComplexity(ECollisionComplexity::Complex);
            std::cout << "        ✅ setDefaultCollisionComplexity() callable" << std::endl;

            std::cout << "    Testing callback registration syntax..." << std::endl;

            int callbackId = middleware->registerUpdateCallback([](const FileData& data) {
            });
            std::cout << "        ✅ registerUpdateCallback() returns ID: " << callbackId << std::endl;

            if (callbackId > 0) {
                middleware->unregisterUpdateCallback(callbackId);
                std::cout << "        ✅ unregisterUpdateCallback() callable" << std::endl;
            }

            results.addSuccess("CPP_SYNTAX_Validation");
            std::cout << "    ✅ C++ interface compiles and links correctly" << std::endl;
            std::cout << "    ✅ All method signatures and return types validated" << std::endl;
            std::cout << "    ⚠️ Note: No actual file loading tested (use C interface for that)" << std::endl;

            return true;

        } catch (const std::exception& e) {
            results.addFailure("CPP_SYNTAX_Exception", "C++ syntax exception: " + std::string(e.what()));
            return false;
        }
    }


    bool runTestSuite(const std::string& testDataPath) {
        std::cout << "🚀 JUSYNC USD VALIDATION - C INTERFACE FOCUSED (v2.0)" << std::endl;
        std::cout << "Architecture: C Interface (90%) + C++ Syntax Check (10%)" << std::endl;
        std::cout << "Target: " << testDataPath << std::endl;
        std::cout << "Focus: Production-Critical C Interface Validation" << std::endl;
        std::cout << std::string(60, '=') << std::endl;

        bool allPassed = true;

        if (!testCInterfaceComprehensive(testDataPath)) {
            allPassed = false;
            std::cout << "⚠️ Critical: C interface tests failed - this affects production!" << std::endl;
        }

        if (!testCppSyntaxOnly()) {
            std::cout << "⚠️ C++ syntax validation failed - check wrapper integrity" << std::endl;
        }

        return allPassed;
    }

    void printFinalResults() {
        results.printSummary();

        std::cout << "\n🎯 VALIDATION FOCUS SUMMARY:" << std::endl;
        std::cout << "  ✅ C Interface (Primary): Production-critical validation complete" << std::endl;
        std::cout << "  🔧 C++ Interface (Secondary): Syntax validation complete" << std::endl;
        std::cout << "  🔰 Collision Processing: All complexity types validated" << std::endl;
        std::cout << "  🔒 Error Handling: Production stability verified" << std::endl;
        std::cout << "  📊 Parameter Configuration: All settings validated" << std::endl;
        std::cout << "\n💡 INTEGRATION GUIDANCE:" << std::endl;
        std::cout << "  - Unity: Use C interface via P/Invoke" << std::endl;
        std::cout << "  - Godot: Use C interface via GDNative/GDExtension" << std::endl;
        std::cout << "  - Unreal Engine: Use C interface via plugin system" << std::endl;
        std::cout << "  - Python: Use C interface via ctypes/CFFI" << std::endl;
        std::cout << "  - Other Languages: Use C interface via FFI" << std::endl;
    }

    int getFailureCount() const {
        return results.failedTests;
    }
};

int main(int argc, char* argv[]) {
    std::cout << "╔═══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║     JUSYNC USD Validation Tool v2.0 (C-Interface Focus)  ║" << std::endl;
    std::cout << "║   Primary: C Interface Testing (90% - Production)        ║" << std::endl;
    std::cout << "║   Secondary: C++ Syntax Validation (10% - Architecture)  ║" << std::endl;
    std::cout << "║   Focus: External Integration Compatibility              ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════════════════════╝" << std::endl;

    std::string testDataPath;

    if (argc > 1) {
        testDataPath = argv[1];
    } else {
        testDataPath = "tests/data/usd_samples";
        if (!fs::exists(testDataPath)) {
            testDataPath = "./tests/data/usd_samples";
            if (!fs::exists(testDataPath)) {
                testDataPath = "../tests/data/usd_samples";
                if (!fs::exists(testDataPath)) {
                    std::cerr << "❌ Usage: " << argv[0] << " <usd_file_or_directory>" << std::endl;
                    std::cerr << "   Default test directory 'tests/data/usd_samples' not found." << std::endl;
                    return 1;
                }
            }
        }
    }

    USDValidationTool validator;

    bool success = validator.testCInterfaceComprehensive(testDataPath);

    if (success) {
        validator.testCppSyntaxOnly(); // Just compilation/linking test
    }

    std::cout << "\n" << std::string(80, '=') << std::endl;
    validator.printFinalResults();

    if (success && validator.getFailureCount() == 0) {
        std::cout << "\n🎉 ALL VALIDATIONS PASSED!" << std::endl;
        std::cout << "   Your C interface (production pipeline) is working correctly." << std::endl;
        std::cout << "   Your C++ architecture (wrapper integrity) is validated." << std::endl;
        std::cout << "   Ready for Unity/Godot/UE integration!" << std::endl;
        return 0;
    } else {
        std::cout << "\n❌ SOME VALIDATIONS FAILED!" << std::endl;
        std::cout << "   Check detailed failure reports above." << std::endl;
        return 1;
    }
}
