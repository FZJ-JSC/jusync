#include "AnariUsdMiddleware.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <thread>
#include <iomanip>
#include <sstream>


using namespace anari_usd_middleware;

class ZMQUSDProcessor {
private:
    AnariUsdMiddleware middleware;
    bool isRunning;
    std::string saveDirectory;
    int totalFilesReceived;
    int totalMeshesExtracted;

public:
    ZMQUSDProcessor(const std::string& saveDir = "received")
        : isRunning(false), saveDirectory(saveDir), totalFilesReceived(0), totalMeshesExtracted(0) {
        std::filesystem::create_directories(saveDirectory);
    }

    bool initialize(const char* endpoint) {
        std::cout << "=== ANARI USD Middleware Processor ===" << std::endl;
        std::cout << "Initializing ZMQ connection..." << std::endl;
        std::cout << "Endpoint: " << (endpoint ? endpoint : "tcp://*:5556") << std::endl;

        if (!middleware.initialize(endpoint)) {
            std::cerr << "❌ Failed to initialize middleware" << std::endl;
            return false;
        }

        std::cout << "✅ Middleware initialized successfully" << std::endl;

        // Register comprehensive file update callback
        int callbackId = middleware.registerUpdateCallback(
            [this](const AnariUsdMiddleware::FileData& fileData) {
                this->processReceivedFile(fileData);
            }
        );

        if (callbackId < 0) {
            std::cerr << "❌ Failed to register file update callback" << std::endl;
            return false;
        }

        // Register message callback for debugging
        int msgCallbackId = middleware.registerMessageCallback(
            [this](const std::string& message) {
                this->processReceivedMessage(message);
            }
        );

        if (msgCallbackId < 0) {
            std::cerr << "❌ Failed to register message callback" << std::endl;
            return false;
        }

        std::cout << "✅ Callbacks registered successfully" << std::endl;
        return true;
    }

    bool startReceiving() {
        if (!middleware.isConnected()) {
            std::cerr << "❌ Middleware not connected" << std::endl;
            return false;
        }

        if (!middleware.startReceiving()) {
            std::cerr << "❌ Failed to start receiving data" << std::endl;
            return false;
        }

        isRunning = true;
        std::cout << "🚀 ZMQ USD Processor is running..." << std::endl;
        std::cout << "📥 Waiting for USD files via ZMQ..." << std::endl;
        std::cout << "💾 Files will be saved to: " << saveDirectory << std::endl;
        return true;
    }

    void stopReceiving() {
        isRunning = false;
        middleware.stopReceiving();
        middleware.shutdown();
        printSummary();
        std::cout << "🛑 ZMQ USD Processor stopped" << std::endl;
    }

    void waitForExit() {
        std::cout << "\n⌨️  Press Enter to exit..." << std::endl;
        std::cin.get();
        stopReceiving();
    }

    // Test disk loading functionality
    bool testDiskLoading(const std::string& filePath) {
        std::cout << "\n=== Testing Disk Loading ===" << std::endl;
        std::cout << "Loading file: " << filePath << std::endl;

        std::vector<AnariUsdMiddleware::MeshData> meshData;
        bool success = middleware.LoadUSDFromDisk(filePath, meshData);

        if (success && !meshData.empty()) {
            std::cout << "✅ Successfully loaded USD from disk!" << std::endl;
            printDetailedMeshInfo(meshData, filePath);
            saveMeshDataToFile(meshData, filePath);
            return true;
        } else {
            std::cerr << "❌ Failed to load USD from disk: " << filePath << std::endl;
            return false;
        }
    }

private:
    void processReceivedFile(const AnariUsdMiddleware::FileData& fileData) {
        totalFilesReceived++;

        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "📁 RECEIVED FILE #" << totalFilesReceived << std::endl;
        std::cout << std::string(50, '=') << std::endl;
        std::cout << "📄 Filename: " << fileData.filename << std::endl;
        std::cout << "📊 Size: " << formatBytes(fileData.data.size()) << std::endl;
        std::cout << "🏷️  Type: " << fileData.fileType << std::endl;
        std::cout << "🔐 Hash: " << fileData.hash.substr(0, 16) << "..." << std::endl;

        // Save the received file with timestamp
        std::string timestamp = getCurrentTimestamp();
        std::string filename = std::filesystem::path(fileData.filename).filename().string();
        std::string savePath = saveDirectory + "/" + timestamp + "_" + filename;

        if (saveFileToPath(fileData.data, savePath)) {
            std::cout << "💾 Saved to: " << savePath << std::endl;
        } else {
            std::cerr << "❌ Failed to save file: " << savePath << std::endl;
            return;
        }

        // Process based on file type
        if (isUSDFile(filename)) {
            processUSDFile(fileData, filename, timestamp);
        } else if (isImageFile(filename)) {
            processImageFile(fileData, filename, timestamp);
        } else {
            std::cout << "❓ Unknown file type, saved but not processed" << std::endl;
        }
    }

    void processReceivedMessage(const std::string& message) {
        std::cout << "\n" << std::string(30, '-') << std::endl;
        std::cout << "💬 RECEIVED MESSAGE" << std::endl;
        std::cout << std::string(30, '-') << std::endl;
        std::cout << "Content: " << message << std::endl;

        // Check message format
        if ((message.find('{') == 0 && message.rfind('}') == message.length() - 1) ||
            (message.find('[') == 0 && message.rfind(']') == message.length() - 1)) {
            std::cout << "📋 Format: JSON" << std::endl;
        } else {
            std::cout << "📋 Format: Plain text" << std::endl;
        }
    }

    void processUSDFile(const AnariUsdMiddleware::FileData& fileData,
                   const std::string& filename, const std::string& timestamp) {
        std::cout << "\n🔧 PROCESSING USD FILE" << std::endl;
        std::cout << std::string(30, '-') << std::endl;

        std::vector<AnariUsdMiddleware::MeshData> meshData;
        auto startTime = std::chrono::high_resolution_clock::now();
        bool success = middleware.LoadUSDBuffer(fileData.data, filename, meshData);
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        if (success && !meshData.empty()) {
            totalMeshesExtracted += meshData.size();
            std::cout << "✅ Successfully extracted mesh data!" << std::endl;
            std::cout << "⏱️ Processing time: " << duration.count() << "ms" << std::endl;

            printDetailedMeshInfo(meshData, filename);

            // Pass timing and file size data to the analysis report
            saveMeshDataToFile(meshData, filename, timestamp, duration, fileData.data.size());
        } else {
            std::cerr << "❌ Failed to extract mesh data from: " << filename << std::endl;
            std::cerr << "⏱️ Failed after: " << duration.count() << "ms" << std::endl;
            diagnoseUSDFile(fileData.data, filename);
        }
    }
    void processImageFile(const AnariUsdMiddleware::FileData& fileData,
                         const std::string& filename, const std::string& timestamp) {
        std::cout << "\n🖼️  PROCESSING IMAGE FILE" << std::endl;
        std::cout << std::string(30, '-') << std::endl;

        // Create texture data
        auto textureData = middleware.CreateTextureFromBuffer(fileData.data);
        if (!textureData.data.empty()) {
            std::cout << "✅ Texture created successfully!" << std::endl;
            std::cout << "📐 Dimensions: " << textureData.width << "x" << textureData.height << std::endl;
            std::cout << "🎨 Channels: " << textureData.channels << std::endl;
        }

        // Extract gradient line as PNG
        std::vector<uint8_t> pngBuffer;
        bool success = middleware.GetGradientLineAsPNGBuffer(fileData.data, pngBuffer);

        if (success) {
            std::cout << "✅ Generated gradient PNG: " << formatBytes(pngBuffer.size()) << std::endl;

            std::string gradientPngPath = saveDirectory + "/" + timestamp + "_gradient_" + filename;
            if (saveFileToPath(pngBuffer, gradientPngPath)) {
                std::cout << "💾 Saved gradient PNG: " << gradientPngPath << std::endl;
            }
        } else {
            std::cerr << "❌ Failed to generate gradient PNG for: " << filename << std::endl;
        }
    }

    void printDetailedMeshInfo(const std::vector<AnariUsdMiddleware::MeshData>& meshes,
                              const std::string& filename) {
        std::cout << "\n📊 MESH ANALYSIS" << std::endl;
        std::cout << std::string(40, '-') << std::endl;
        std::cout << "🗂️  File: " << filename << std::endl;
        std::cout << "🔢 Total meshes: " << meshes.size() << std::endl;

        size_t totalVertices = 0, totalTriangles = 0;

        for (size_t i = 0; i < meshes.size(); ++i) {
            const auto& mesh = meshes[i];
            size_t numVertices = mesh.points.size() / 3;
            size_t numTriangles = mesh.indices.size() / 3;
            size_t numNormals = mesh.normals.size() / 3;
            size_t numUVs = mesh.uvs.size() / 2;

            totalVertices += numVertices;
            totalTriangles += numTriangles;

            std::cout << "\n  🔸 Mesh " << (i + 1) << ": " << mesh.elementName << std::endl;
            std::cout << "    📝 Type: " << mesh.typeName << std::endl;
            std::cout << "    🔺 Vertices: " << numVertices << std::endl;
            std::cout << "    🔻 Triangles: " << numTriangles << std::endl;
            std::cout << "    ➡️  Normals: " << numNormals << std::endl;
            std::cout << "    🗺️  UVs: " << numUVs << std::endl;

            // Print first few vertices for debugging
            if (numVertices > 0) {
                std::cout << "    📍 First vertex: ("
                         << std::fixed << std::setprecision(3)
                         << mesh.points[0] << ", "
                         << mesh.points[1] << ", "
                         << mesh.points[2] << ")" << std::endl;
            }
        }

        std::cout << "\n📈 SUMMARY:" << std::endl;
        std::cout << "  🔢 Total vertices: " << totalVertices << std::endl;
        std::cout << "  🔢 Total triangles: " << totalTriangles << std::endl;
    }

    void saveMeshDataToFile(const std::vector<AnariUsdMiddleware::MeshData>& meshes,
                       const std::string& originalFilename,
                       const std::string& timestamp = "",
                       const std::chrono::milliseconds& processingTime = std::chrono::milliseconds(0),
                       size_t fileSize = 0) {
    std::string prefix = timestamp.empty() ? "" : timestamp + "_";
    std::string meshInfoPath = saveDirectory + "/" + prefix + originalFilename + "_analysis.txt";

    std::ofstream meshFile(meshInfoPath);
    if (meshFile.is_open()) {
        // Header
        meshFile << "ANARI USD Middleware - Performance Analysis Report" << std::endl;
        meshFile << "=================================================" << std::endl;
        meshFile << "Generated: " << getCurrentTimestamp() << std::endl;
        meshFile << "File: " << originalFilename << std::endl;
        meshFile << std::endl;

        // Performance Metrics Section
        meshFile << "PERFORMANCE METRICS" << std::endl;
        meshFile << "===================" << std::endl;
        meshFile << "File Size: " << formatBytes(fileSize) << std::endl;
        meshFile << "Processing Time: " << processingTime.count() << " ms" << std::endl;

        if (fileSize > 0 && processingTime.count() > 0) {
            double mbPerSecond = (static_cast<double>(fileSize) / (1024.0 * 1024.0)) /
                               (static_cast<double>(processingTime.count()) / 1000.0);
            meshFile << "Processing Speed: " << std::fixed << std::setprecision(2)
                    << mbPerSecond << " MB/s" << std::endl;
        }

        // Calculate total geometry metrics
        size_t totalVertices = 0, totalTriangles = 0, totalNormals = 0, totalUVs = 0;
        for (const auto& mesh : meshes) {
            totalVertices += mesh.points.size() / 3;
            totalTriangles += mesh.indices.size() / 3;
            totalNormals += mesh.normals.size() / 3;
            totalUVs += mesh.uvs.size() / 2;
        }

        if (processingTime.count() > 0) {
            double verticesPerMs = static_cast<double>(totalVertices) / processingTime.count();
            double trianglesPerMs = static_cast<double>(totalTriangles) / processingTime.count();

            meshFile << "Vertex Processing Rate: " << std::fixed << std::setprecision(0)
                    << (verticesPerMs * 1000) << " vertices/second" << std::endl;
            meshFile << "Triangle Processing Rate: " << std::fixed << std::setprecision(0)
                    << (trianglesPerMs * 1000) << " triangles/second" << std::endl;
        }

        meshFile << std::endl;

        // Geometry Summary
        meshFile << "GEOMETRY SUMMARY" << std::endl;
        meshFile << "================" << std::endl;
        meshFile << "Total Meshes: " << meshes.size() << std::endl;
        meshFile << "Total Vertices: " << totalVertices << std::endl;
        meshFile << "Total Triangles: " << totalTriangles << std::endl;
        meshFile << "Total Normals: " << totalNormals << std::endl;
        meshFile << "Total UVs: " << totalUVs << std::endl;
        meshFile << std::endl;

        // Detailed Mesh Information
        meshFile << "DETAILED MESH ANALYSIS" << std::endl;
        meshFile << "======================" << std::endl;

        for (size_t i = 0; i < meshes.size(); ++i) {
            const auto& mesh = meshes[i];
            size_t numVertices = mesh.points.size() / 3;
            size_t numTriangles = mesh.indices.size() / 3;
            size_t numNormals = mesh.normals.size() / 3;
            size_t numUVs = mesh.uvs.size() / 2;

            meshFile << "Mesh " << (i + 1) << ": " << mesh.elementName << std::endl;
            meshFile << "  Type: " << mesh.typeName << std::endl;
            meshFile << "  Vertices: " << numVertices << std::endl;
            meshFile << "  Triangles: " << numTriangles << std::endl;
            meshFile << "  Normals: " << numNormals << std::endl;
            meshFile << "  UVs: " << numUVs << std::endl;

            // Calculate mesh complexity metrics
            if (numVertices > 0) {
                double triangleToVertexRatio = static_cast<double>(numTriangles) / numVertices;
                meshFile << "  Triangle/Vertex Ratio: " << std::fixed << std::setprecision(2)
                        << triangleToVertexRatio << std::endl;

                bool hasNormals = numNormals > 0;
                bool hasUVs = numUVs > 0;
                meshFile << "  Has Normals: " << (hasNormals ? "Yes" : "No") << std::endl;
                meshFile << "  Has UVs: " << (hasUVs ? "Yes" : "No") << std::endl;
            }
            meshFile << std::endl;
        }

        meshFile.close();
        std::cout << "📄 Performance analysis saved to: " << meshInfoPath << std::endl;
    }
}


    void diagnoseUSDFile(const std::vector<uint8_t>& data, const std::string& filename) {
        std::cout << "\n🔍 DIAGNOSING USD FILE" << std::endl;
        std::cout << std::string(30, '-') << std::endl;

        if (data.size() < 8) {
            std::cout << "❌ File too small to be valid USD (< 8 bytes)" << std::endl;
            return;
        }

        // Check file format
        if (data.size() >= 8 && std::memcmp(data.data(), "PXR-USDC", 8) == 0) {
            std::cout << "🔍 Format: Binary USDC" << std::endl;
        } else {
            // Check if it looks like text USD/USDA
            std::string preview(reinterpret_cast<const char*>(data.data()),
                              std::min(data.size(), size_t(100)));
            if (preview.find("#usda") != std::string::npos ||
                preview.find("def ") != std::string::npos) {
                std::cout << "🔍 Format: Text USDA" << std::endl;
            } else {
                std::cout << "❓ Format: Unknown/Unrecognized" << std::endl;
            }
        }

        // Show hex dump of first bytes
        std::cout << "🔍 First 32 bytes (hex): ";
        for (size_t i = 0; i < std::min(data.size(), size_t(32)); ++i) {
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                     << static_cast<int>(data[i]) << " ";
        }
        std::cout << std::dec << std::endl;
    }

    void printSummary() {
        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "📊 SESSION SUMMARY" << std::endl;
        std::cout << std::string(50, '=') << std::endl;
        std::cout << "📁 Total files received: " << totalFilesReceived << std::endl;
        std::cout << "🔧 Total meshes extracted: " << totalMeshesExtracted << std::endl;
        std::cout << "💾 Files saved to: " << saveDirectory << std::endl;
    }

    // Utility functions
    std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
        return ss.str();
    }

    std::string formatBytes(size_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB"};
        int unit = 0;
        double size = static_cast<double>(bytes);

        while (size >= 1024 && unit < 3) {
            size /= 1024;
            unit++;
        }

        std::stringstream ss;
        ss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return ss.str();
    }

    bool saveFileToPath(const std::vector<uint8_t>& data, const std::string& path) {
        std::ofstream outfile(path, std::ios::binary);
        if (outfile.is_open()) {
            outfile.write(reinterpret_cast<const char*>(data.data()), data.size());
            outfile.close();
            return true;
        }
        return false;
    }

    bool isUSDFile(const std::string& filename) {
        return filename.find(".usd") != std::string::npos ||
               filename.find(".usda") != std::string::npos ||
               filename.find(".usdc") != std::string::npos ||
               filename.find(".usdz") != std::string::npos;
    }

    bool isImageFile(const std::string& filename) {
        return filename.find(".png") != std::string::npos ||
               filename.find(".jpg") != std::string::npos ||
               filename.find(".jpeg") != std::string::npos;
    }
};

int main(int argc, char** argv) {
    // Parse command-line arguments
    bool loadFromDisk = false;
    std::string diskFilePath;
    const char* endpoint = nullptr;
    std::string saveDir = "received";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--disk" && i + 1 < argc) {
            loadFromDisk = true;
            diskFilePath = argv[++i];
        } else if (arg == "--endpoint" && i + 1 < argc) {
            endpoint = argv[++i];
        } else if (arg == "--save-dir" && i + 1 < argc) {
            saveDir = argv[++i];
        } else if (arg == "--help") {
            std::cout << "ANARI USD Middleware Test Application" << std::endl;
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --disk <file>         Test disk loading with specified USD file" << std::endl;
            std::cout << "  --endpoint <address>  ZMQ endpoint (default: tcp://*:5556)" << std::endl;
            std::cout << "  --save-dir <path>     Directory to save received files (default: received)" << std::endl;
            std::cout << "  --help                Show this help message" << std::endl;
            return 0;
        }
    }

    // Create processor
    ZMQUSDProcessor processor(saveDir);

    if (loadFromDisk) {
        // Test disk loading mode
        if (!std::filesystem::exists(diskFilePath)) {
            std::cerr << "❌ File not found: " << diskFilePath << std::endl;
            return 1;
        }

        // Initialize middleware for disk testing
        if (!processor.initialize(nullptr)) {
            return 1;
        }

        bool success = processor.testDiskLoading(diskFilePath);
        return success ? 0 : 1;
    }

    // Network mode
    if (!processor.initialize(endpoint)) {
        std::cerr << "❌ Failed to initialize processor" << std::endl;
        return 1;
    }

    if (!processor.startReceiving()) {
        std::cerr << "❌ Failed to start receiving" << std::endl;
        return 1;
    }

    // Wait for user to exit
    processor.waitForExit();

    return 0;
}
