#include "AnariUsdMiddleware.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <csignal>  // ✅ ADD: For signal handling

using namespace std::chrono;

class ZMQUSDProcessor {
private:
    anari_usd_middleware::AnariUsdMiddleware middleware;
    std::atomic<size_t> totalFilesReceived{0};
    std::atomic<size_t> totalMeshesExtracted{0};
    std::atomic<size_t> totalBytesProcessed{0};
    std::atomic<bool> shutdownRequested{false};

public:
    ZMQUSDProcessor() {
        std::cout << "🚀 ZMQ USD Processor initialized with collision support" << std::endl;
    }

    ~ZMQUSDProcessor() {
        std::cout << "📊 Final Statistics:" << std::endl;
        std::cout << "   Files received: " << totalFilesReceived.load() << std::endl;
        std::cout << "   Meshes extracted: " << totalMeshesExtracted.load() << std::endl;
        std::cout << "   Bytes processed: " << formatBytes(totalBytesProcessed.load()) << std::endl;
    }

    bool initialize(const char* endpoint = nullptr) {
        std::cout << "🔌 Initializing middleware..." << std::endl;

        if (!middleware.initialize(endpoint)) {
            std::cerr << "❌ Failed to initialize middleware" << std::endl;
            return false;
        }

        // ✅ FIXED: Correct type reference for callback
        int callbackId = middleware.registerUpdateCallback(
            [this](const anari_usd_middleware::FileData& fileData) {
                processReceivedFile(fileData);
            }
        );

        if (callbackId < 0) {
            std::cerr << "❌ Failed to register update callback" << std::endl;
            return false;
        }

        std::cout << "✅ Middleware initialized successfully" << std::endl;
        return true;
    }

    void shutdown() {
        std::cout << "🛑 Shutting down..." << std::endl;
        shutdownRequested.store(true);
        middleware.shutdown();
    }

    bool startReceiving() {
        std::cout << "📡 Starting receiver..." << std::endl;

        if (!middleware.startReceiving()) {
            std::cerr << "❌ Failed to start receiver" << std::endl;
            return false;
        }

        std::cout << "✅ Receiver started successfully" << std::endl;
        return true;
    }

    void stopReceiving() {
        std::cout << "⏹️ Stopping receiver..." << std::endl;
        middleware.stopReceiving();
    }

 void run() {
    std::cout << "🔄 ZMQ USD Processor running... Press 'q' + Enter to quit" << std::endl;
    std::string line;
    while (std::getline(std::cin, line) && !shutdownRequested.load()) {  // ✅ READ FULL LINE
        std::istringstream iss(line);
        std::string command;
        iss >> command;  // Get first word

        if (command == "q") {
            break;
        } else if (command == "status") {
            printStatus();
        } else if (command == "stats") {
            printStatistics();
        } else if (command == "test") {
            std::string filepath;
            iss >> filepath;  // ✅ GET FILEPATH FROM SAME LINE
            if (!filepath.empty()) {
                if (testDiskLoading(filepath)) {
                    std::cout << "✅ Test completed successfully" << std::endl;
                } else {
                    std::cout << "❌ Test failed" << std::endl;
                }
            } else {
                std::cout << "Usage: test <filepath>" << std::endl;
            }
        } else {
            std::cout << "Commands: status, stats, test <filepath>, q" << std::endl;
        }
    }
}


    bool testDiskLoading(const std::string& filePath) {
        std::cout << "🧪 Testing disk loading: " << filePath << std::endl;

        auto start = steady_clock::now();

        // ✅ FIXED: Correct type reference for MeshData vector
        std::vector<anari_usd_middleware::MeshData> meshData;
        bool success = middleware.LoadUSDFromDisk(filePath, meshData);

        auto duration = steady_clock::now() - start;

        if (success && !meshData.empty()) {
            std::cout << "✅ Successfully loaded " << meshData.size() << " meshes in "
                      << duration_cast<milliseconds>(duration).count() << "ms" << std::endl;
            printDetailedMeshInfo(meshData, "disk_test");
            return true;
        } else {
            std::cout << "❌ Failed to load USD file" << std::endl;
            return false;
        }
    }

private:
    // ✅ FIXED: Correct type reference for FileData parameter
    void processReceivedFile(const anari_usd_middleware::FileData& fileData) {
        totalFilesReceived.fetch_add(1);
        totalBytesProcessed.fetch_add(fileData.data.size());

        std::cout << "\n📄 Filename: " << fileData.filename << std::endl;
        std::cout << "📊 Size: " << formatBytes(fileData.data.size()) << std::endl;
        std::cout << "🏷️  Type: " << fileData.fileType << std::endl;
        std::cout << "🔐 Hash: " << fileData.hash.substr(0, 16) << "..." << std::endl;

        // Extract filename and create output directory
        std::string filename = std::filesystem::path(fileData.filename).filename().string();
        std::string outputDir = "./received_files";
        std::filesystem::create_directories(outputDir);
        std::string savePath = outputDir + "/" + filename;

        if (saveFileToPath(fileData.data, savePath)) {
            std::cout << "💾 Saved to: " << savePath << std::endl;

            // Get current timestamp
            auto timestamp = getCurrentTimestamp();

            // Process based on file type
            if (fileData.fileType == "USD") {
                processUSDFile(fileData, filename, timestamp);
            } else if (fileData.fileType == "IMAGE") {
                processImageFile(fileData, filename, timestamp);
            }

            std::cout << "----------------------------------------" << std::endl;
        } else {
            std::cerr << "❌ Failed to save file: " << filename << std::endl;
        }
    }

    // ✅ FIXED: Correct type reference for FileData parameter
    void processUSDFile(const anari_usd_middleware::FileData& fileData,
                       const std::string& filename,
                       const std::string& timestamp) {
        std::cout << "🎬 Processing USD file..." << std::endl;

        auto start = steady_clock::now();

        // ✅ FIXED: Correct type reference for MeshData vector
        std::vector<anari_usd_middleware::MeshData> meshData;

        bool success = middleware.LoadUSDBuffer(fileData.data, filename, meshData);
        // ✅ FIXED: Use duration_cast to convert nanoseconds to milliseconds
        auto duration = duration_cast<milliseconds>(steady_clock::now() - start);

        if (success && !meshData.empty()) {
            totalMeshesExtracted += meshData.size();
            std::cout << "✅ Extracted " << meshData.size() << " meshes in "
                      << duration.count() << "ms" << std::endl;

            // Print detailed info and save to file
            printDetailedMeshInfo(meshData, timestamp);
            saveMeshDataToFile(meshData, filename, timestamp, duration, fileData.data.size());
        } else {
            std::cout << "❌ Failed to extract meshes from USD" << std::endl;
            diagnoseUSDFile(fileData.data, filename);
        }
    }

    // ✅ FIXED: Correct type reference for FileData parameter
    void processImageFile(const anari_usd_middleware::FileData& fileData,
                         const std::string& filename,
                         const std::string& timestamp) {
        std::cout << "🖼️ Processing image file..." << std::endl;

        try {
            // Test texture creation
            auto textureData = middleware.CreateTextureFromBuffer(fileData.data);

            if (textureData.isValid()) {
                std::cout << "✅ Texture loaded: " << textureData.width << "x"
                          << textureData.height << " (" << textureData.channels << " channels)" << std::endl;
            }

            // Test gradient extraction
            std::vector<uint8_t> pngBuffer;
            bool success = middleware.GetGradientLineAsPNGBuffer(fileData.data, pngBuffer);

            if (success && !pngBuffer.empty()) {
                std::string pngPath = "./processed_gradients/" + timestamp + "_" + filename + "_gradient.png";
                std::filesystem::create_directories("./processed_gradients");

                if (saveFileToPath(std::vector<unsigned char>(pngBuffer.begin(), pngBuffer.end()), pngPath)) {
                    std::cout << "✅ Gradient line extracted: " << pngPath << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cout << "❌ Image processing failed: " << e.what() << std::endl;
        }
    }

    // ✅ FIXED: Correct type reference for MeshData vector parameter
    void printDetailedMeshInfo(const std::vector<anari_usd_middleware::MeshData>& meshes,
                              const std::string& context) {
        std::cout << "\n📋 Detailed Mesh Information (" << context << "):" << std::endl;
        std::cout << "🔢 Total meshes: " << meshes.size() << std::endl;

        size_t totalVertices = 0, totalTriangles = 0;

        for (size_t i = 0; i < meshes.size(); ++i) {
            const auto& mesh = meshes[i];

            size_t vertexCount = mesh.getVertexCount();
            size_t triangleCount = mesh.getTriangleCount();
            totalVertices += vertexCount;
            totalTriangles += triangleCount;

            std::cout << "  📐 Mesh " << (i + 1) << ": \"" << mesh.elementName << "\"" << std::endl;
            std::cout << "     📍 Type: " << mesh.typeName << std::endl;
            std::cout << "     🔺 Vertices: " << vertexCount << std::endl;
            std::cout << "     🔻 Triangles: " << triangleCount << std::endl;

            if (!mesh.normals.empty()) {
                std::cout << "     📏 Normals: " << mesh.normals.size() / 3 << std::endl;
            }
            if (!mesh.uvs.empty()) {
                std::cout << "     🗺️  UVs: " << mesh.uvs.size() / 2 << std::endl;
            }
            if (!mesh.vertex_colors.empty()) {
                std::cout << "     🎨 Vertex Colors: " << mesh.vertex_colors.size() / 4 << std::endl;
            }

            // ✅ NEW: Display collision information
            if (mesh.collision.collisionType != anari_usd_middleware::ECollisionComplexity::None) {
                std::cout << "     🛡️ Collision: " << anari_usd_middleware::CollisionProcessor::getComplexityName(mesh.collision.collisionType) << std::endl;
                if (!mesh.collision.vertices.empty()) {
                    std::cout << "        Collision vertices: " << mesh.collision.vertices.size() / 3 << std::endl;
                    std::cout << "        Collision triangles: " << mesh.collision.indices.size() / 3 << std::endl;
                }
            }

            // ✅ FIXED: Calculate bounds manually instead of using getBounds()
            if (mesh.isValid() && !mesh.points.empty()) {
                // Calculate bounding box manually
                float minX = mesh.points[0], maxX = mesh.points[0];
                float minY = mesh.points[1], maxY = mesh.points[1];
                float minZ = mesh.points[2], maxZ = mesh.points[2];

                for (size_t j = 3; j < mesh.points.size(); j += 3) {
                    minX = std::min(minX, mesh.points[j]);
                    maxX = std::max(maxX, mesh.points[j]);
                    minY = std::min(minY, mesh.points[j + 1]);
                    maxY = std::max(maxY, mesh.points[j + 1]);
                    minZ = std::min(minZ, mesh.points[j + 2]);
                    maxZ = std::max(maxZ, mesh.points[j + 2]);
                }

                std::cout << "     📦 Bounds: (" << minX << "," << minY << "," << minZ
                          << ") to (" << maxX << "," << maxY << "," << maxZ << ")" << std::endl;
            } else {
                std::cout << "     ⚠️  Warning: Invalid mesh geometry" << std::endl;
            }
        }

        std::cout << "📊 Totals: " << totalVertices << " vertices, " << totalTriangles << " triangles" << std::endl;
    }

    // ✅ FIXED: Correct type reference for MeshData vector parameter
    void saveMeshDataToFile(const std::vector<anari_usd_middleware::MeshData>& meshes,
                           const std::string& filename,
                           const std::string& timestamp,
                           const milliseconds& duration,
                           size_t originalFileSize) {
        try {
            std::string outputDir = "./processed_meshes";
            std::filesystem::create_directories(outputDir);
            std::string meshFilePath = outputDir + "/" + timestamp + "_" + filename + "_meshes.txt";

            std::ofstream meshFile(meshFilePath);
            if (!meshFile.is_open()) {
                std::cerr << "❌ Failed to create mesh data file: " << meshFilePath << std::endl;
                return;
            }

            // Write header information
            meshFile << "USD Mesh Data Report" << std::endl;
            meshFile << "Generated: " << timestamp << std::endl;
            meshFile << "Source File: " << filename << std::endl;
            meshFile << "Original Size: " << formatBytes(originalFileSize) << std::endl;
            meshFile << "Processing Time: " << duration.count() << "ms" << std::endl;
            meshFile << "Total Meshes: " << meshes.size() << std::endl;
            meshFile << "========================================" << std::endl;

            // Calculate totals
            size_t totalVertices = 0, totalTriangles = 0, totalNormals = 0, totalUVs = 0, totalColors = 0;

            for (const auto& mesh : meshes) {
                totalVertices += mesh.getVertexCount();
                totalTriangles += mesh.getTriangleCount();
                if (!mesh.normals.empty()) totalNormals += mesh.normals.size() / 3;
                if (!mesh.uvs.empty()) totalUVs += mesh.uvs.size() / 2;
                if (!mesh.vertex_colors.empty()) totalColors += mesh.vertex_colors.size() / 4;
            }

            meshFile << "Total Vertices: " << totalVertices << std::endl;
            meshFile << "Total Triangles: " << totalTriangles << std::endl;
            meshFile << "Total Normals: " << totalNormals << std::endl;
            meshFile << "Total UVs: " << totalUVs << std::endl;
            meshFile << "Total Vertex Colors: " << totalColors << std::endl;
            meshFile << "========================================" << std::endl;

            // Write detailed mesh information
            for (size_t i = 0; i < meshes.size(); ++i) {
                const auto& mesh = meshes[i];

                meshFile << "Mesh " << (i + 1) << ":" << std::endl;
                meshFile << "  Name: " << mesh.elementName << std::endl;
                meshFile << "  Type: " << mesh.typeName << std::endl;
                meshFile << "  Vertices: " << mesh.getVertexCount() << std::endl;
                meshFile << "  Triangles: " << mesh.getTriangleCount() << std::endl;

                if (!mesh.normals.empty()) {
                    meshFile << "  Normals: " << mesh.normals.size() / 3 << std::endl;
                }
                if (!mesh.uvs.empty()) {
                    meshFile << "  UVs: " << mesh.uvs.size() / 2 << std::endl;
                }
                if (!mesh.vertex_colors.empty()) {
                    meshFile << "  Vertex Colors: " << mesh.vertex_colors.size() / 4 << std::endl;
                }

                // ✅ NEW: Write collision information
                if (mesh.collision.collisionType != anari_usd_middleware::ECollisionComplexity::None) {
                    meshFile << "  Collision Type: " << anari_usd_middleware::CollisionProcessor::getComplexityName(mesh.collision.collisionType) << std::endl;
                    if (!mesh.collision.vertices.empty()) {
                        meshFile << "  Collision Vertices: " << mesh.collision.vertices.size() / 3 << std::endl;
                        meshFile << "  Collision Triangles: " << mesh.collision.indices.size() / 3 << std::endl;
                    }
                }

                // ✅ FIXED: Calculate bounds manually instead of using getBounds()
                if (mesh.isValid() && !mesh.points.empty()) {
                    float minX = mesh.points[0], maxX = mesh.points[0];
                    float minY = mesh.points[1], maxY = mesh.points[1];
                    float minZ = mesh.points[2], maxZ = mesh.points[2];

                    for (size_t j = 3; j < mesh.points.size(); j += 3) {
                        minX = std::min(minX, mesh.points[j]);
                        maxX = std::max(maxX, mesh.points[j]);
                        minY = std::min(minY, mesh.points[j + 1]);
                        maxY = std::max(maxY, mesh.points[j + 1]);
                        minZ = std::min(minZ, mesh.points[j + 2]);
                        maxZ = std::max(maxZ, mesh.points[j + 2]);
                    }

                    meshFile << "  Bounds: (" << minX << "," << minY << "," << minZ
                             << ") to (" << maxX << "," << maxY << "," << maxZ << ")" << std::endl;
                }

                meshFile << "  Valid: " << (mesh.isValid() ? "Yes" : "No") << std::endl;
                meshFile << "----------------------------------------" << std::endl;
            }

            meshFile.close();
            std::cout << "💾 Mesh data saved to: " << meshFilePath << std::endl;

        } catch (const std::exception& e) {
            std::cerr << "❌ Exception saving mesh data: " << e.what() << std::endl;
        }
    }

    void printStatus() {
        std::cout << "\n📊 Current Status:" << std::endl;
        std::cout << "  Middleware connected: " << (middleware.isConnected() ? "Yes" : "No") << std::endl;
        std::cout << "  Files received: " << totalFilesReceived.load() << std::endl;
        std::cout << "  Meshes extracted: " << totalMeshesExtracted.load() << std::endl;
        std::cout << "  Bytes processed: " << formatBytes(totalBytesProcessed.load()) << std::endl;
        std::cout << "  Default collision: " << anari_usd_middleware::CollisionProcessor::getComplexityName(middleware.getDefaultCollisionComplexity()) << std::endl;
    }

    void printStatistics() {
        std::cout << "\n📈 Processing Statistics:" << std::endl;
        std::cout << middleware.getStatusInfo() << std::endl;
    }

    void diagnoseUSDFile(const std::vector<uint8_t>& data, const std::string& filename) {
        std::cout << "🔍 Diagnosing USD file: " << filename << std::endl;

        if (data.size() < 100) {
            std::cout << "  ⚠️ File too small to analyze" << std::endl;
            return;
        }

        std::string content(reinterpret_cast<const char*>(data.data()),
                           std::min(data.size(), static_cast<size_t>(1000)));

        std::cout << "  📝 First 200 chars: " << content.substr(0, 200) << std::endl;

        if (content.find("#usda") != std::string::npos) {
            std::cout << "  ✅ USDA format detected" << std::endl;
        } else if (content.find("PXR-USDC") != std::string::npos) {
            std::cout << "  ✅ USDC binary format detected" << std::endl;
        } else {
            std::cout << "  ❓ Unknown USD format" << std::endl;
        }
    }

    bool saveFileToPath(const std::vector<unsigned char>& data, const std::string& path) {
        try {
            std::ofstream file(path, std::ios::binary);
            if (!file.is_open()) {
                return false;
            }

            file.write(reinterpret_cast<const char*>(data.data()), data.size());
            return file.good();

        } catch (const std::exception&) {
            return false;
        }
    }

    std::string formatBytes(size_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB"};
        double size = static_cast<double>(bytes);
        int unit = 0;

        while (size >= 1024.0 && unit < 3) {
            size /= 1024.0;
            unit++;
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return oss.str();
    }

    std::string getCurrentTimestamp() {
        auto now = system_clock::now();
        auto time_t = system_clock::to_time_t(now);

        std::ostringstream oss;
        oss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
        return oss.str();
    }
};

int main(int argc, char* argv[]) {
    std::cout << "🚀 Starting ZMQ USD Processor with Collision Support..." << std::endl;

    ZMQUSDProcessor processor;

    const char* endpoint = (argc > 1) ? argv[1] : nullptr;

    if (!processor.initialize(endpoint)) {
        std::cerr << "❌ Failed to initialize processor" << std::endl;
        return 1;
    }

    if (!processor.startReceiving()) {
        std::cerr << "❌ Failed to start receiving" << std::endl;
        return 1;
    }

    // ✅ FIXED: Use std::signal instead of std::signal and add proper include
    std::signal(SIGINT, [](int) {
        std::cout << "\n🛑 Shutdown requested..." << std::endl;
        exit(0);
    });

    processor.run();

    processor.stopReceiving();
    processor.shutdown();

    std::cout << "👋 ZMQ USD Processor stopped" << std::endl;
    return 0;
}
