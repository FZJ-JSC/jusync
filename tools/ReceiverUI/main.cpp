#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <map>

// OpenGL includes
#include <GL/glew.h>
#include <GLFW/glfw3.h>

// ImGui includes
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// Your C middleware interface
#include "AnariUsdMiddleware_C.h"

// Debug and 3D viewer windows
#include "CollisionDebugWindow.h"
#include "3DViewer.h"

// Forward declarations
class MiddlewareTestApp;

static void fileReceivedCallback(const CFileData* file_data);
static void messageReceivedCallback(const char* message);

// Global instance for callbacks
static MiddlewareTestApp* g_app = nullptr;

// ============================================================================
// MAIN APPLICATION CLASS WITH COLLISION DEBUG AND 3D VIEWER SUPPORT
// ============================================================================

class MiddlewareTestApp {
public:
    // Statistics (public for callback access)
    std::atomic<int> filesReceived{0};
    std::atomic<int> meshesExtracted{0};
    std::atomic<size_t> totalBytesReceived{0};

    // UI state (public for callbacks)
    std::vector<std::string> receivedFiles;
    std::vector<std::string> meshInfo;
    std::vector<CMeshData> currentMeshes;

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

    // Debug and 3D viewer windows
    std::unique_ptr<CollisionDebugWindow> collisionDebugWindow;
    std::unique_ptr<Viewer3D> viewer3D;

    // Window visibility flags
    bool showMainWindow = true;
    bool showLogWindow = true;
    bool showStatsWindow = true;
    bool showFileListWindow = true;
    bool showMeshInfoWindow = true;
    bool showCollisionDebugWindow = true;
    bool show3DViewer = true;

public:
    MiddlewareTestApp() {
        logMessages.reserve(1000);
        receivedFiles.reserve(100);
        meshInfo.reserve(100);
        collisionDebugWindow = std::make_unique<CollisionDebugWindow>();
        viewer3D = std::make_unique<Viewer3D>();
    }

    ~MiddlewareTestApp() {
        cleanup();
    }

    bool initialize() {
        addLog("🚀 Initializing ANARI-USD Middleware Test Application with Collision Debug and 3D Viewer...");

        // Initialize GLFW
        if (!glfwInit()) {
            addLog("❌ ERROR: Failed to initialize GLFW");
            return false;
        }

        // Create window with better settings
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        glfwWindowHint(GLFW_SAMPLES, 4); // Enable 4x MSAA

        window = glfwCreateWindow(1600, 1000, "ANARI-USD Middleware Test with Collision Debug & 3D Viewer", nullptr, nullptr);
        if (!window) {
            addLog("❌ ERROR: Failed to create GLFW window");
            glfwTerminate();
            return false;
        }

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1); // Enable vsync

        // Initialize GLEW
        if (glewInit() != GLEW_OK) {
            addLog("❌ ERROR: Failed to initialize GLEW");
            return false;
        }

        // Setup ImGui with better configuration
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        // Setup ImGui style
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 5.0f;
        style.FrameRounding = 3.0f;
        style.ScrollbarRounding = 3.0f;
        style.GrabRounding = 3.0f;

        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330");

        // Initialize 3D viewer
        if (!viewer3D->initialize()) {
            addLog("⚠️ Warning: Failed to initialize 3D viewer - OpenGL features may be limited");
            show3DViewer = false;
        } else {
            addLog("✅ 3D viewer initialized successfully");
        }

        // Register middleware callbacks
        RegisterUpdateCallback_C(fileReceivedCallback);
        RegisterMessageCallback_C(messageReceivedCallback);

        addLog("✅ Application initialized successfully with collision debug and 3D viewer support");
        addLog("✅ Callbacks registered and ready");
        addLog("🔍 Collision Debug Window available for detailed mesh analysis");
        addLog("🎮 3D Viewer available for interactive mesh visualization");

        return true;
    }

    void run() {
        while (!glfwWindowShouldClose(window) && isRunning.load()) {
            glfwPollEvents();

            // Start ImGui frame
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            // Enable docking
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->Pos);
            ImGui::SetNextWindowSize(viewport->Size);
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

            ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
            window_flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

            ImGui::Begin("DockSpace Demo", nullptr, window_flags);
            ImGui::PopStyleVar(3);

            // DockSpace
            ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

            // Menu bar
            if (ImGui::BeginMenuBar()) {
                if (ImGui::BeginMenu("Windows")) {
                    ImGui::MenuItem("Main Control", nullptr, &showMainWindow);
                    ImGui::MenuItem("Log Messages", nullptr, &showLogWindow);
                    ImGui::MenuItem("Statistics", nullptr, &showStatsWindow);
                    ImGui::MenuItem("File List", nullptr, &showFileListWindow);
                    ImGui::MenuItem("Mesh Info", nullptr, &showMeshInfoWindow);
                    ImGui::MenuItem("Collision Debug", nullptr, &showCollisionDebugWindow);
                    ImGui::MenuItem("3D Viewer", nullptr, &show3DViewer);
                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Tools")) {
                    if (ImGui::MenuItem("Clear All Data")) {
                        clearAllData();
                    }
                    if (ImGui::MenuItem("Reset Statistics")) {
                        resetStatistics();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Test All Collision Types")) {
                        testAllCollisionTypes();
                    }
                    if (ImGui::MenuItem("Focus 3D Camera")) {
                        if (viewer3D && !currentMeshes.empty()) {
                            // Focus camera on current meshes
                            addLog("🎯 Focusing 3D camera on loaded meshes");
                        }
                    }
                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Help")) {
                    if (ImGui::MenuItem("About")) {
                        showAboutDialog = true;
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }

            ImGui::End();

            // Render individual windows
            if (showMainWindow) renderMainWindow();
            if (showLogWindow) renderLogWindow();
            if (showStatsWindow) renderStatsWindow();
            if (showFileListWindow) renderFileListWindow();
            if (showMeshInfoWindow) renderMeshInfoWindow();

            // Render collision debug window
            if (showCollisionDebugWindow && collisionDebugWindow) {
                collisionDebugWindow->render(currentMeshes);
            }

            // Render 3D viewer
            if (show3DViewer && viewer3D) {
                viewer3D->render(currentMeshes);
            }

            // About dialog
            if (showAboutDialog) {
                renderAboutDialog();
            }

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

        std::string logEntry = std::string(timestamp) + message;
        logMessages.push_back(logEntry);

        // Keep only last 1000 messages
        if (logMessages.size() > 1000) {
            logMessages.erase(logMessages.begin());
        }

        // Also print to console
        std::cout << timestamp << message << std::endl;
    }

private:
    bool showAboutDialog = false;

    void renderMainWindow() {
        ImGui::Begin("🎛️ ANARI-USD Middleware Control", &showMainWindow, ImGuiWindowFlags_AlwaysAutoResize);

        // Connection section
        ImGui::SeparatorText("🔌 Connection");
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("Endpoint", endpointBuffer, sizeof(endpointBuffer));

        ImGui::SameLine();
        if (ImGui::Button("🚀 Initialize")) {
            initializeMiddleware();
        }

        ImGui::SameLine();
        if (ImGui::Button("🛑 Shutdown")) {
            shutdownMiddleware();
        }

        // Status indicators with better visuals
        ImGui::Text("Status: ");
        ImGui::SameLine();
        if (middlewareInitialized.load()) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "🟢 CONNECTED");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "🔴 DISCONNECTED");
        }

        // Data reception section
        ImGui::SeparatorText("📡 Data Reception");
        if (ImGui::Button("▶️ Start Receiving")) {
            startReceiving();
        }

        ImGui::SameLine();
        if (ImGui::Button("⏸️ Stop Receiving")) {
            stopReceiving();
        }

        ImGui::Text("Receiving: ");
        ImGui::SameLine();
        if (isReceiving.load()) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "🟢 ACTIVE");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "🟡 INACTIVE");
        }

        // Testing section with collision options
        ImGui::SeparatorText("🧪 Testing & Collision");

        // Basic tests
        if (ImGui::Button("📄 Test USD File")) {
            testUSDFile();
        }
        ImGui::SameLine();
        if (ImGui::Button("🖼️ Test Texture")) {
            testTexture();
        }

        // Collision-specific tests
        if (ImGui::Button("💥 Test Simple Collision")) {
            testCollisionType(COLLISION_SIMPLE, "Simple (Bounding Box)");
        }
        ImGui::SameLine();
        if (ImGui::Button("🔺 Test Convex Hull")) {
            testCollisionType(COLLISION_CONVEX_HULL, "Convex Hull");
        }

        if (ImGui::Button("🔶 Test Complex Collision")) {
            testCollisionType(COLLISION_COMPLEX, "Complex (Full Mesh)");
        }
        ImGui::SameLine();
        if (ImGui::Button("⚡ Test Simplified")) {
            testCollisionType(COLLISION_SIMPLIFIED, "Simplified (Decimated)");
        }

        if (ImGui::Button("🧩 Test All Collision Types")) {
            testAllCollisionTypes();
        }
        ImGui::SameLine();
        if (ImGui::Button("🎯 Load Sample USD with Collision")) {
            loadSampleUSDWithCollision();
        }

        // 3D Visualization section
        ImGui::SeparatorText("🎮 3D Visualization");
        if (ImGui::Button("🖥️ Open 3D Viewer")) {
            show3DViewer = true;
            if (viewer3D) {
                viewer3D->setVisible(true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("🔍 Open Collision Debug")) {
            showCollisionDebugWindow = true;
            if (collisionDebugWindow) {
                collisionDebugWindow->setVisible(true);
            }
        }

        // Show mesh count for 3D viewer
        if (!currentMeshes.empty()) {
            ImGui::Text("📊 Loaded Meshes: %zu (ready for 3D viewing)", currentMeshes.size());
        }

        // Collision configuration
        ImGui::SeparatorText("⚙️ Collision Configuration");

        static int defaultCollisionType = COLLISION_COMPLEX;
        const char* collisionTypes[] = {
            "None", "Simple (Box)", "Convex Hull", "Complex", "Simplified", "Convex Decomp"
        };

        if (ImGui::Combo("Default Collision Type", &defaultCollisionType, collisionTypes, 6)) {
            SetDefaultCollisionComplexity_C(defaultCollisionType);
            addLog("🔧 Default collision complexity set to: " + std::string(GetCollisionComplexityName_C(defaultCollisionType)));
        }

        // Advanced collision parameters
        static float simplificationRatio = 0.25f;
        static float convexHullPrecision = 0.001f;
        static int maxConvexHulls = 32;

        ImGui::SliderFloat("Simplification Ratio", &simplificationRatio, 0.1f, 0.9f, "%.2f");
        ImGui::SliderFloat("Convex Hull Precision", &convexHullPrecision, 0.001f, 0.1f, "%.3f");
        ImGui::SliderInt("Max Convex Hulls", &maxConvexHulls, 1, 64);

        if (ImGui::Button("🔧 Apply Collision Parameters")) {
            SetCollisionParameters_C(simplificationRatio, convexHullPrecision, maxConvexHulls);
            addLog("🔧 Collision parameters updated");
        }

        // Data management
        ImGui::SeparatorText("🗂️ Data Management");
        if (ImGui::Button("🗑️ Clear All Data")) {
            clearAllData();
        }
        ImGui::SameLine();
        if (ImGui::Button("📊 Reset Statistics")) {
            resetStatistics();
        }

        ImGui::End();
    }

    void renderLogWindow() {
        ImGui::Begin("📋 Log Messages", &showLogWindow);

        if (ImGui::Button("🗑️ Clear Log")) {
            logMessages.clear();
        }
        ImGui::SameLine();

        static bool autoScroll = true;
        ImGui::Checkbox("Auto Scroll", &autoScroll);

        ImGui::Separator();
        ImGui::BeginChild("LogScrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

        for (const auto& message : logMessages) {
            // Color code messages based on content
            ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // Default white
            if (message.find("ERROR") != std::string::npos || message.find("❌") != std::string::npos) {
                color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); // Red
            } else if (message.find("WARNING") != std::string::npos || message.find("⚠️") != std::string::npos) {
                color = ImVec4(1.0f, 1.0f, 0.3f, 1.0f); // Yellow
            } else if (message.find("✅") != std::string::npos) {
                color = ImVec4(0.3f, 1.0f, 0.3f, 1.0f); // Green
            } else if (message.find("🔍") != std::string::npos || message.find("collision") != std::string::npos) {
                color = ImVec4(0.3f, 0.8f, 1.0f, 1.0f); // Light blue
            } else if (message.find("🎮") != std::string::npos || message.find("3D") != std::string::npos) {
                color = ImVec4(1.0f, 0.3f, 1.0f, 1.0f); // Magenta for 3D viewer
            }

            ImGui::TextColored(color, "%s", message.c_str());
        }

        // Auto-scroll to bottom
        if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();
        ImGui::End();
    }

    void renderStatsWindow() {
        ImGui::Begin("📊 Statistics", &showStatsWindow, ImGuiWindowFlags_AlwaysAutoResize);

        // Basic statistics
        ImGui::Text("📁 Files Received: %d", filesReceived.load());
        ImGui::Text("🔺 Meshes Extracted: %d", meshesExtracted.load());
        ImGui::Text("💾 Total Bytes: %s", formatBytes(totalBytesReceived.load()).c_str());

        // Collision statistics
        ImGui::Separator();
        ImGui::Text("💥 Collision Statistics:");

        std::map<int, int> collisionCounts;
        for (const auto& mesh : currentMeshes) {
            collisionCounts[mesh.collision_type]++;
        }

        for (const auto& [type, count] : collisionCounts) {
            if (count > 0) {
                ImGui::Text("  %s: %d meshes", GetCollisionComplexityName_C(type), count);
            }
        }

        // 3D Viewer statistics
        if (viewer3D && !currentMeshes.empty()) {
            ImGui::Separator();
            ImGui::Text("🎮 3D Viewer:");
            ImGui::Text("  Ready to display %zu meshes", currentMeshes.size());
            if (show3DViewer) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "  Status: Active");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "  Status: Hidden");
            }
        }

        // Connection status from middleware
        if (middlewareInitialized.load()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "🔌 Middleware Status:");
            const char* statusInfo = GetStatusInfo_C();
            ImGui::TextWrapped("%s", statusInfo);
        }

        ImGui::End();
    }

    void renderFileListWindow() {
        ImGui::Begin("📁 Received Files", &showFileListWindow);

        if (ImGui::Button("🗑️ Clear File List")) {
            receivedFiles.clear();
        }

        ImGui::Separator();
        ImGui::BeginChild("FileList");

        for (size_t i = 0; i < receivedFiles.size(); ++i) {
            if (i % 2 == 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
            }
            ImGui::Text("%zu: %s", i + 1, receivedFiles[i].c_str());
            if (i % 2 == 0) {
                ImGui::PopStyleColor();
            }
        }

        ImGui::EndChild();
        ImGui::End();
    }

    void renderMeshInfoWindow() {
        ImGui::Begin("🔺 Mesh Information", &showMeshInfoWindow);

        if (ImGui::Button("🗑️ Clear Mesh Info")) {
            meshInfo.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("🔍 Open Collision Debug")) {
            showCollisionDebugWindow = true;
            if (collisionDebugWindow) {
                collisionDebugWindow->setVisible(true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("🎮 Open 3D Viewer")) {
            show3DViewer = true;
            if (viewer3D) {
                viewer3D->setVisible(true);
            }
        }

        ImGui::Separator();
        ImGui::BeginChild("MeshInfo");

        for (const auto& info : meshInfo) {
            ImGui::TextWrapped("%s", info.c_str());
            ImGui::Separator();
        }

        ImGui::EndChild();
        ImGui::End();
    }

    void renderAboutDialog() {
        ImGui::Begin("About", &showAboutDialog, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("ANARI-USD Middleware Test Application");
        ImGui::Text("Version: 3.0.0 with Collision Debug & 3D Viewer Support");
        ImGui::Separator();
        ImGui::Text("Features:");
        ImGui::BulletText("USD file loading and processing");
        ImGui::BulletText("Multiple collision complexity types");
        ImGui::BulletText("Real-time collision debug visualization");
        ImGui::BulletText("Interactive 3D mesh viewer");
        ImGui::BulletText("ZeroMQ network communication");
        ImGui::BulletText("Comprehensive mesh analysis");
        ImGui::BulletText("OpenGL-based 3D rendering");

        ImGui::Separator();
        ImGui::Text("Controls:");
        ImGui::BulletText("3D Viewer: Left mouse = orbit, Right mouse = pan, Scroll = zoom");
        ImGui::BulletText("Collision Debug: Click meshes to inspect details");

        if (ImGui::Button("Close")) {
            showAboutDialog = false;
        }

        ImGui::End();
    }

    // ... [Include all the existing methods from your current main.cpp:
    //      initializeMiddleware, shutdownMiddleware, startReceiving, stopReceiving,
    //      testUSDFile, testTexture, testCollisionType, testAllCollisionTypes,
    //      loadSampleUSDWithCollision, clearAllData, resetStatistics, formatBytes, cleanup]

    void initializeMiddleware() {
        addLog("🚀 Initializing middleware with endpoint: " + std::string(endpointBuffer));

        int result = InitializeMiddleware_C(endpointBuffer);
        if (result == 1) {
            middlewareInitialized.store(true);
            addLog("✅ Middleware initialized successfully");
            addLog("✅ Callbacks registered and active");
        } else {
            middlewareInitialized.store(false);
            addLog("❌ Failed to initialize middleware");
        }
    }

    void shutdownMiddleware() {
        addLog("🛑 Shutting down middleware...");
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

        addLog("📡 Starting data reception...");
        int result = StartReceiving_C();
        if (result == 1) {
            isReceiving.store(true);
            addLog("✅ Data reception started");
        } else {
            addLog("❌ Failed to start data reception");
        }
    }

    void stopReceiving() {
        addLog("⏸️ Stopping data reception...");
        StopReceiving_C();
        isReceiving.store(false);
        addLog("✅ Data reception stopped");
    }

    void testUSDFile() {
        addLog("🧪 Testing USD file loading...");

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
                std::string info = "📄 Mesh " + std::to_string(i) + ": " +
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
        addLog("🖼️ Testing texture creation...");

        // Create a simple test image (2x2 RGBA)
        std::vector<unsigned char> testImage = {
            255, 0, 0, 255,     // Red
            0, 255, 0, 255,     // Green
            0, 0, 255, 255,     // Blue
            255, 255, 0, 255    // Yellow
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

    void testCollisionType(int collisionType, const std::string& typeName) {
        addLog("💥 Testing " + typeName + " collision...");

        std::string testUSD = R"(#usda 1.0
def Mesh "CollisionTestCube"
{
    float3[] extent = [(-2, -2, -2), (2, 2, 2)]
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
    point3f[] points = [(-2, -2, 2), (2, -2, 2), (-2, 2, 2), (2, 2, 2), (-2, 2, -2), (2, 2, -2), (-2, -2, -2), (2, -2, -2)]
}
)";

        CMeshData* meshes = nullptr;
        size_t meshCount = 0;

        int result = LoadUSDBufferWithCollision_C(
            reinterpret_cast<const unsigned char*>(testUSD.c_str()),
            testUSD.size(),
            ("collision_test_" + typeName + ".usda").c_str(),
            collisionType,
            &meshes,
            &meshCount
        );

        if (result == 1 && meshes && meshCount > 0) {
            addLog("✅ " + typeName + " collision test successful: " + std::to_string(meshCount) + " meshes");

            // Clear current meshes and add new ones
            currentMeshes.clear();
            for (size_t i = 0; i < meshCount; ++i) {
                currentMeshes.push_back(meshes[i]);

                std::string info = "💥 " + typeName + " - Mesh " + std::to_string(i) + ": " +
                    std::string(meshes[i].element_name) +
                    " (Visual: " + std::to_string(meshes[i].points_count / 3) + " vertices, " +
                    std::to_string(meshes[i].indices_count / 3) + " triangles) " +
                    "(Collision: " + std::to_string(meshes[i].collision_vertices_count / 3) + " vertices, " +
                    std::to_string(meshes[i].collision_indices_count / 3) + " triangles)";
                meshInfo.push_back(info);
                addLog("  " + info);
            }

            meshesExtracted.fetch_add(meshCount);
            addLog("🔍 Open Collision Debug Window to analyze collision data");
            addLog("🎮 Open 3D Viewer to visualize the mesh interactively");

            // Don't free meshes yet - we need them for collision debug display and 3D viewer
        } else {
            addLog("❌ " + typeName + " collision test failed");
        }
    }

    void testAllCollisionTypes() {
        addLog("🧩 Testing all collision complexity types...");

        std::string testUSD = R"(#usda 1.0
def Mesh "CompleteCollisionTest"
{
    float3[] extent = [(-3, -3, -3), (3, 3, 3)]
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
    point3f[] points = [(-3, -3, 3), (3, -3, 3), (-3, 3, 3), (3, 3, 3), (-3, 3, -3), (3, 3, -3), (-3, -3, -3), (3, -3, -3)]
}
)";

        currentMeshes.clear();

        // Test all collision types
        for (int collisionType = 0; collisionType <= 5; ++collisionType) {
            CMeshData* meshes = nullptr;
            size_t meshCount = 0;

            std::string typeName = GetCollisionComplexityName_C(collisionType);
            addLog("🔄 Testing " + typeName + "...");

            int result = LoadUSDBufferWithCollision_C(
                reinterpret_cast<const unsigned char*>(testUSD.c_str()),
                testUSD.size(),
                ("complete_test_" + std::to_string(collisionType) + ".usda").c_str(),
                collisionType,
                &meshes,
                &meshCount
            );

            if (result == 1 && meshes && meshCount > 0) {
                addLog("✅ " + typeName + " successful: " + std::to_string(meshCount) + " meshes");

                for (size_t j = 0; j < meshCount; ++j) {
                    // Modify element name to include collision type
                    std::string modifiedName = std::string(meshes[j].element_name) + "_" + typeName;
                    strncpy(meshes[j].element_name, modifiedName.c_str(), 255);
                    meshes[j].element_name[255] = '\0';

                    currentMeshes.push_back(meshes[j]);

                    addLog("   📊 " + typeName + ": " +
                           std::to_string(meshes[j].collision_vertices_count / 3) + " collision vertices, " +
                           std::to_string(meshes[j].collision_indices_count / 3) + " collision triangles");
                }

                meshesExtracted.fetch_add(meshCount);
            } else {
                addLog("❌ " + typeName + " failed");
            }
        }

        addLog("🎉 Complete collision test finished!");
        addLog("🔍 Open Collision Debug Window to analyze all collision types");
        addLog("🎮 Open 3D Viewer to see all meshes with different collision types");
    }

    void loadSampleUSDWithCollision() {
        addLog("🎯 Loading sample USD with advanced collision...");

        // More complex test geometry
        std::string complexUSD = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Mesh "ComplexMesh"
    {
        float3[] extent = [(-5, -5, -5), (5, 5, 5)]
        int[] faceVertexCounts = [3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3]
        int[] faceVertexIndices = [
            0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5, 0, 5, 1,
            6, 2, 1, 6, 3, 2, 6, 4, 3, 6, 5, 4, 6, 1, 5,
            1, 5, 4, 1, 4, 3, 1, 3, 2
        ]
        point3f[] points = [
            (0, 5, 0), (3, 2, 3), (-3, 2, 3), (-3, 2, -3), (3, 2, -3), (3, 2, 3),
            (0, -5, 0)
        ]
        color3f[] primvars:displayColor = [(0.8, 0.2, 0.2)]
    }
}
)";

        CMeshData* meshes = nullptr;
        size_t meshCount = 0;

        int result = LoadUSDBufferWithCollision_C(
            reinterpret_cast<const unsigned char*>(complexUSD.c_str()),
            complexUSD.size(),
            "sample_complex.usda",
            COLLISION_CONVEX_HULL,
            &meshes,
            &meshCount
        );

        if (result == 1 && meshes && meshCount > 0) {
            addLog("✅ Sample USD loaded with convex hull collision: " + std::to_string(meshCount) + " meshes");

            currentMeshes.clear();
            for (size_t i = 0; i < meshCount; ++i) {
                currentMeshes.push_back(meshes[i]);

                std::string info = "🎯 Sample Mesh " + std::to_string(i) + ": " +
                    std::string(meshes[i].element_name) +
                    " | Visual: " + std::to_string(meshes[i].points_count / 3) + "v/" +
                    std::to_string(meshes[i].indices_count / 3) + "t" +
                    " | Collision: " + std::to_string(meshes[i].collision_vertices_count / 3) + "v/" +
                    std::to_string(meshes[i].collision_indices_count / 3) + "t";
                meshInfo.push_back(info);
                addLog("  " + info);
            }

            meshesExtracted.fetch_add(meshCount);
            addLog("🔍 Collision Debug Window updated with sample data");
            addLog("🎮 3D Viewer ready to display sample mesh");
        } else {
            addLog("❌ Failed to load sample USD");
        }
    }

    void clearAllData() {
        receivedFiles.clear();
        meshInfo.clear();
        currentMeshes.clear();
        filesReceived.store(0);
        meshesExtracted.store(0);
        totalBytesReceived.store(0);
        addLog("🗑️ All data cleared");
    }

    void resetStatistics() {
        filesReceived.store(0);
        meshesExtracted.store(0);
        totalBytesReceived.store(0);
        addLog("📊 Statistics reset");
    }

    std::string formatBytes(size_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB"};
        int unitIndex = 0;
        double size = static_cast<double>(bytes);

        while (size >= 1024.0 && unitIndex < 3) {
            size /= 1024.0;
            unitIndex++;
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << size << " " << units[unitIndex];
        return oss.str();
    }

    void cleanup() {
        if (middlewareInitialized.load()) {
            shutdownMiddleware();
        }

        // Cleanup 3D viewer
        if (viewer3D) {
            viewer3D->cleanup();
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

// ============================================================================
// CALLBACK IMPLEMENTATIONS (same as before)
// ============================================================================

static void fileReceivedCallback(const CFileData* file_data) {
    if (!file_data || !g_app) {
        std::cout << "❌ ERROR: Null file_data or app instance in callback" << std::endl;
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

        // Load with complex collision by default
        int result = LoadUSDBufferWithCollision_C(
            file_data->data,
            file_data->data_size,
            file_data->filename,
            COLLISION_COMPLEX,
            &meshes,
            &meshCount
        );

        if (result == 1 && meshes && meshCount > 0) {
            g_app->addLog("✅ USD processed with collision: " + std::to_string(meshCount) + " meshes extracted");

            // Update current meshes for collision debug and 3D viewer
            g_app->currentMeshes.clear();
            for (size_t i = 0; i < meshCount; ++i) {
                g_app->currentMeshes.push_back(meshes[i]);

                std::string info = "📡 Received Mesh: " + std::string(meshes[i].element_name) +
                    " (Visual: " + std::to_string(meshes[i].points_count / 3) + " vertices, " +
                    std::to_string(meshes[i].indices_count / 3) + " triangles)" +
                    " (Collision: " + std::to_string(meshes[i].collision_vertices_count / 3) + " vertices, " +
                    std::to_string(meshes[i].collision_indices_count / 3) + " triangles)";
                g_app->meshInfo.push_back(info);
            }

            g_app->meshesExtracted.fetch_add(meshCount);
            g_app->addLog("🔍 Collision data available in debug window");
            g_app->addLog("🎮 3D visualization ready");

            // Don't free meshes immediately - keep for collision debug and 3D viewer
        }
    }
}

static void messageReceivedCallback(const char* message) {
    if (!message || !g_app) {
        std::cout << "❌ ERROR: Null message or app instance in callback" << std::endl;
        return;
    }

    std::cout << "🎉 CALLBACK TRIGGERED: Message received!" << std::endl;
    g_app->addLog("💬 Message received: " + std::string(message));
}

// ============================================================================
// MAIN FUNCTION
// ============================================================================

int main() {
    MiddlewareTestApp app;
    g_app = &app; // Set global instance BEFORE initialize

    if (!app.initialize()) {
        std::cerr << "❌ Failed to initialize application" << std::endl;
        return -1;
    }

    std::cout << "🚀 ANARI-USD Middleware Test Application Started with Collision Debug & 3D Viewer Support" << std::endl;
    std::cout << "🔍 Collision Debug Window provides detailed mesh and collision analysis" << std::endl;
    std::cout << "🎮 3D Viewer provides interactive mesh visualization with collision overlay" << std::endl;
    std::cout << "💡 Use the GUI to test middleware functionality and collision generation" << std::endl;

    app.run();

    g_app = nullptr;
    return 0;
}
