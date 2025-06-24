#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <atomic>

// ImGui includes
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GL/glew.h>
#include <GLFW/glfw3.h>

// Your C middleware interface
#include "AnariUsdMiddleware_C.h"

// Forward declarations
class MiddlewareTestApp;
static void fileReceivedCallback(const CFileData* file_data);
static void messageReceivedCallback(const char* message);

// CRITICAL FIX: Global instance declared BEFORE the class
static MiddlewareTestApp* g_app = nullptr;

class MiddlewareTestApp {
public:  // CRITICAL FIX: Make these public for callback access
    // Statistics
    std::atomic<int> filesReceived{0};
    std::atomic<int> meshesExtracted{0};
    std::atomic<size_t> totalBytesReceived{0};

    // UI state (also make public for callbacks)
    std::vector<std::string> receivedFiles;
    std::vector<std::string> meshInfo;

private:
    // Application state
    std::atomic<bool> isRunning{true};
    std::atomic<bool> middlewareInitialized{false};
    std::atomic<bool> isReceiving{false};

    // UI state
    char endpointBuffer[256] = "tcp://localhost:13456";
    std::vector<std::string> logMessages;

    // OpenGL/ImGui state
    GLFWwindow* window = nullptr;

public:
    MiddlewareTestApp() {
        logMessages.reserve(1000);
        receivedFiles.reserve(100);
        meshInfo.reserve(100);
    }

    ~MiddlewareTestApp() {
        cleanup();
    }

    bool initialize() {
        addLog("Initializing ANARI-USD Middleware Test Application...");

        // Initialize GLFW
        if (!glfwInit()) {
            addLog("ERROR: Failed to initialize GLFW");
            return false;
        }

        // Create window
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        window = glfwCreateWindow(1200, 800, "ANARI-USD Middleware Test", nullptr, nullptr);
        if (!window) {
            addLog("ERROR: Failed to create GLFW window");
            glfwTerminate();
            return false;
        }

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1); // Enable vsync

        // Initialize GLEW
        if (glewInit() != GLEW_OK) {
            addLog("ERROR: Failed to initialize GLEW");
            return false;
        }

        // Setup ImGui
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImGui::StyleColorsDark();

        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330");

        // CRITICAL FIX: Register middleware callbacks (now properly declared)
        RegisterUpdateCallback_C(fileReceivedCallback);
        RegisterMessageCallback_C(messageReceivedCallback);

        addLog("✅ Application initialized successfully");
        addLog("✅ Callbacks registered and ready");
        return true;
    }

    void run() {
        while (!glfwWindowShouldClose(window) && isRunning.load()) {
            glfwPollEvents();

            // Start ImGui frame
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            // Render UI
            renderMainWindow();
            renderLogWindow();
            renderStatsWindow();
            renderFileListWindow();
            renderMeshInfoWindow();

            // Render ImGui
            ImGui::Render();
            int display_w, display_h;
            glfwGetFramebufferSize(window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);
            glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(window);

            // Small delay to prevent CPU spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    // Method to add logs from callbacks
    void addLog(const std::string& message) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);

        char timestamp[32];
        std::strftime(timestamp, sizeof(timestamp), "[%H:%M:%S] ", &tm);

        logMessages.push_back(std::string(timestamp) + message);

        // Keep only last 1000 messages
        if (logMessages.size() > 1000) {
            logMessages.erase(logMessages.begin());
        }

        // Also print to console
        std::cout << timestamp << message << std::endl;
    }

private:
    void renderMainWindow() {
        ImGui::Begin("ANARI-USD Middleware Control", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        // Connection section
        ImGui::SeparatorText("Connection");

        ImGui::InputText("Endpoint", endpointBuffer, sizeof(endpointBuffer));
        ImGui::SameLine();
        if (ImGui::Button("Initialize")) {
            initializeMiddleware();
        }

        ImGui::SameLine();
        if (ImGui::Button("Shutdown")) {
            shutdownMiddleware();
        }

        // Status indicators
        ImGui::Text("Status: ");
        ImGui::SameLine();
        if (middlewareInitialized.load()) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "CONNECTED");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "DISCONNECTED");
        }

        // Receiving section
        ImGui::SeparatorText("Data Reception");

        if (ImGui::Button("Start Receiving")) {
            startReceiving();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop Receiving")) {
            stopReceiving();
        }

        ImGui::Text("Receiving: ");
        ImGui::SameLine();
        if (isReceiving.load()) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "ACTIVE");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "INACTIVE");
        }

        // Test section
        ImGui::SeparatorText("Testing");

        if (ImGui::Button("Test USD File")) {
            testUSDFile();
        }
        ImGui::SameLine();
        if (ImGui::Button("Test Texture")) {
            testTexture();
        }

        if (ImGui::Button("Clear All Data")) {
            clearAllData();
        }

        ImGui::End();
    }

    void renderLogWindow() {
        ImGui::Begin("Log Messages");

        if (ImGui::Button("Clear Log")) {
            logMessages.clear();
        }

        ImGui::BeginChild("LogScrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

        for (const auto& message : logMessages) {
            ImGui::TextUnformatted(message.c_str());
        }

        // Auto-scroll to bottom
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();
        ImGui::End();
    }

    void renderStatsWindow() {
        ImGui::Begin("Statistics", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("Files Received: %d", filesReceived.load());
        ImGui::Text("Meshes Extracted: %d", meshesExtracted.load());
        ImGui::Text("Total Bytes: %zu", totalBytesReceived.load());

        // Connection status from middleware
        if (middlewareInitialized.load()) {
            const char* statusInfo = GetStatusInfo_C();
            ImGui::SeparatorText("Middleware Status");
            ImGui::TextWrapped("%s", statusInfo);
        }

        ImGui::End();
    }

    void renderFileListWindow() {
        ImGui::Begin("Received Files");

        if (ImGui::Button("Clear File List")) {
            receivedFiles.clear();
        }

        ImGui::BeginChild("FileList");
        for (size_t i = 0; i < receivedFiles.size(); ++i) {
            ImGui::Text("%zu: %s", i + 1, receivedFiles[i].c_str());
        }
        ImGui::EndChild();

        ImGui::End();
    }

    void renderMeshInfoWindow() {
        ImGui::Begin("Mesh Information");

        if (ImGui::Button("Clear Mesh Info")) {
            meshInfo.clear();
        }

        ImGui::BeginChild("MeshInfo");
        for (const auto& info : meshInfo) {
            ImGui::TextWrapped("%s", info.c_str());
            ImGui::Separator();
        }
        ImGui::EndChild();

        ImGui::End();
    }

    void initializeMiddleware() {
        addLog("Initializing middleware with endpoint: " + std::string(endpointBuffer));
        // FIXED: Now g_app is accessible since it's declared globally above
        addLog("Global app instance: " + std::to_string(reinterpret_cast<uintptr_t>(g_app)));

        int result = InitializeMiddleware_C(endpointBuffer);
        if (result == 1) {
            middlewareInitialized.store(true);
            addLog("✅ Middleware initialized successfully");
            addLog("✅ Callbacks should now be registered and active");
        } else {
            middlewareInitialized.store(false);
            addLog("❌ Failed to initialize middleware");
        }
    }

    void shutdownMiddleware() {
        addLog("Shutting down middleware...");

        if (isReceiving.load()) {
            stopReceiving();
        }

        ShutdownMiddleware_C();
        middlewareInitialized.store(false);
        addLog("✅ Middleware shutdown complete");
    }

    void startReceiving() {
        if (!middlewareInitialized.load()) {
            addLog("❌ Cannot start receiving: middleware not initialized");
            return;
        }

        addLog("Starting data reception...");
        int result = StartReceiving_C();
        if (result == 1) {
            isReceiving.store(true);
            addLog("✅ Data reception started");
        } else {
            addLog("❌ Failed to start data reception");
        }
    }

    void stopReceiving() {
        addLog("Stopping data reception...");
        StopReceiving_C();
        isReceiving.store(false);
        addLog("✅ Data reception stopped");
    }

    void testUSDFile() {
        addLog("Testing USD file loading...");

        // Create a simple test USD content
        std::string testUSD = R"(#usda 1.0
def Mesh "TestCube"
{
    float3[] extent = [(-1, -1, -1), (1, 1, 1)]
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
    point3f[] points = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)]
}
)";

        CMeshData* meshes = nullptr;
        size_t meshCount = 0;

        int result = LoadUSDBuffer_C(
            reinterpret_cast<const unsigned char*>(testUSD.c_str()),
            testUSD.size(),
            "test_cube.usda",
            &meshes,
            &meshCount
        );

        if (result == 1 && meshes && meshCount > 0) {
            addLog("✅ USD test successful: " + std::to_string(meshCount) + " meshes loaded");

            for (size_t i = 0; i < meshCount; ++i) {
                std::string info = "Mesh " + std::to_string(i) + ": " +
                                 std::string(meshes[i].element_name) +
                                 " (" + std::to_string(meshes[i].points_count / 3) + " vertices, " +
                                 std::to_string(meshes[i].indices_count / 3) + " triangles)";
                meshInfo.push_back(info);
                addLog("  " + info);
            }

            meshesExtracted.fetch_add(meshCount);
            FreeMeshData_C(meshes, meshCount);
        } else {
            addLog("❌ USD test failed");
        }
    }

    void testTexture() {
        addLog("Testing texture creation...");

        // Create a simple test image (2x2 RGBA)
        std::vector<unsigned char> testImage = {
            255, 0, 0, 255,    // Red
            0, 255, 0, 255,    // Green
            0, 0, 255, 255,    // Blue
            255, 255, 0, 255   // Yellow
        };

        CTextureData texture = CreateTextureFromBuffer_C(testImage.data(), testImage.size());

        if (texture.data && texture.width > 0 && texture.height > 0) {
            addLog("✅ Texture test successful: " +
                   std::to_string(texture.width) + "x" +
                   std::to_string(texture.height) + " (" +
                   std::to_string(texture.channels) + " channels)");

            FreeTextureData_C(&texture);
        } else {
            addLog("❌ Texture test failed");
        }
    }

    void clearAllData() {
        receivedFiles.clear();
        meshInfo.clear();
        filesReceived.store(0);
        meshesExtracted.store(0);
        totalBytesReceived.store(0);
        addLog("✅ All data cleared");
    }

    void cleanup() {
        if (middlewareInitialized.load()) {
            shutdownMiddleware();
        }

        if (window) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();

            glfwDestroyWindow(window);
            glfwTerminate();
        }
    }
};

// CRITICAL FIX: Callback functions defined AFTER class but BEFORE main()
static void fileReceivedCallback(const CFileData* file_data) {
    if (!file_data || !g_app) {
        std::cout << "ERROR: Null file_data or app instance in callback" << std::endl;
        return;
    }

    std::cout << "🎉 CALLBACK TRIGGERED: File received!" << std::endl;

    std::string filename = file_data->filename;
    size_t dataSize = file_data->data_size;
    std::string fileType = file_data->file_type;

    g_app->addLog("📁 File received: " + filename + " (" + std::to_string(dataSize) + " bytes, " + fileType + ")");
    g_app->receivedFiles.push_back(filename + " (" + std::to_string(dataSize) + " bytes)");
    g_app->filesReceived.fetch_add(1);
    g_app->totalBytesReceived.fetch_add(dataSize);

    // Try to process as USD if it's a USD file
    if (fileType == "USD" || filename.find(".usd") != std::string::npos) {
        CMeshData* meshes = nullptr;
        size_t meshCount = 0;

        int result = LoadUSDBuffer_C(file_data->data, file_data->data_size, file_data->filename, &meshes, &meshCount);

        if (result == 1 && meshes && meshCount > 0) {
            g_app->addLog("✅ USD processed: " + std::to_string(meshCount) + " meshes extracted");

            for (size_t i = 0; i < meshCount; ++i) {
                std::string info = "Mesh: " + std::string(meshes[i].element_name) +
                                 " (" + std::to_string(meshes[i].points_count / 3) + " vertices, " +
                                 std::to_string(meshes[i].indices_count / 3) + " triangles)";
                g_app->meshInfo.push_back(info);
            }

            g_app->meshesExtracted.fetch_add(meshCount);
            FreeMeshData_C(meshes, meshCount);
        }
    }
}

static void messageReceivedCallback(const char* message) {
    if (!message || !g_app) {
        std::cout << "ERROR: Null message or app instance in callback" << std::endl;
        return;
    }

    std::cout << "🎉 CALLBACK TRIGGERED: Message received!" << std::endl;
    g_app->addLog("💬 Message received: " + std::string(message));
}

int main() {
    MiddlewareTestApp app;
    g_app = &app;  // ✅ CRITICAL: Set global instance BEFORE initialize

    if (!app.initialize()) {
        std::cerr << "Failed to initialize application" << std::endl;
        return -1;
    }

    std::cout << "ANARI-USD Middleware Test Application Started" << std::endl;
    std::cout << "Global app instance set: " << g_app << std::endl;
    std::cout << "Use the GUI to test middleware functionality" << std::endl;

    app.run();

    g_app = nullptr;
    return 0;
}
