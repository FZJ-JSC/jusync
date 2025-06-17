#include "AnariUsdMiddleware.h"
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <future>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <map>
#include <set>
#include <functional>
#include <regex>
#include <cmath>

// Font Awesome icon definitions - proper UTF-8 encoding
#define ICON_FA_SEARCH "\xef\x80\x82"
#define ICON_FA_SITEMAP "\xef\x83\xa8"
#define ICON_FA_CUBE "\xef\x86\xb2"
#define ICON_FA_CHART_BAR "\xef\x80\x81"
#define ICON_FA_SAVE "\xef\x83\x87"
#define ICON_FA_FOLDER "\xef\x81\xc1"
#define ICON_FA_FILE "\xef\x85\x9b"
#define ICON_FA_FILM "\xef\x80\x88"
#define ICON_FA_IMAGE "\xef\x80\xbe"
#define ICON_FA_RULER "\xef\x95\x45"
#define ICON_FA_HASHTAG "\xef\x8a\x92"
#define ICON_FA_TAG "\xef\x80\x8b"
#define ICON_FA_SPINNER "\xef\x84\x90"
#define ICON_FA_CHECK "\xef\x80\x8c"
#define ICON_FA_TIMES "\xef\x80\x8d"
#define ICON_FA_EXCLAMATION "\xef\x84\xaa"
#define ICON_FA_CIRCLE "\xef\x84\x91"
#define ICON_FA_INFO "\xef\x84\xa9"
#define ICON_FA_HOME "\xef\x80\x95"
#define ICON_FA_COG "\xef\x80\x93"
#define ICON_FA_CAMERA "\xef\x80\x93"
#define ICON_FA_PALETTE "\xef\x94\x8e"
#define ICON_FA_BOLT "\xef\x83\xa7"
#define ICON_FA_BOX "\xef\x91\xa6"

// Font ranges for Font Awesome
#define ICON_MIN_FA 0xf000
#define ICON_MAX_FA 0xf8ff

using namespace anari_usd_middleware;

// Application state
struct AppState {
    AnariUsdMiddleware middleware;
    std::vector<AnariUsdMiddleware::FileData> receivedFiles;
    std::vector<std::string> receivedMessages;
    std::atomic<bool> isConnected{false};
    std::atomic<bool> isReceiving{false};
    std::mutex dataMutex;

    // Connection settings
    char ipAddress[256] = "127.0.0.1";
    int port = 5556;

    // Metrics
    int totalFilesReceived = 0;
    int totalMessagesReceived = 0;
    size_t totalBytesReceived = 0;
    std::chrono::steady_clock::time_point startTime;

    // UI state
    bool showMetrics = true;
    bool showFileList = true;
    bool showMessages = true;
    int selectedFileIndex = -1;

    // Callback IDs
    int fileCallbackId = -1;
    int messageCallbackId = -1;

    // Connection mode
    enum class ConnectionMode {
        ZMQ_NETWORK,
        DISK_LOADING
    };
    ConnectionMode connectionMode = ConnectionMode::ZMQ_NETWORK;
    char diskFilePath[512] = "";
    bool showFileDialog = false;

    // Store loaded mesh data
    std::vector<AnariUsdMiddleware::MeshData> loadedMeshData;
    std::mutex meshDataMutex;
    bool hasMeshData = false;

    // Async analysis state
    std::atomic<bool> isAnalyzing{false};
    std::future<void> analysisFuture;
    std::atomic<bool> analysisComplete{false};
    std::vector<AnariUsdMiddleware::MeshData> analysisResults;
    std::mutex analysisResultsMutex;
    std::string analysisFileName;
    std::string analysisError;
    std::atomic<bool> cancelAnalysis{false};
};

static AppState appState;

// Utility functions
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

std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
    return ss.str();
}

// Callbacks
void fileCallback(const AnariUsdMiddleware::FileData& file) {
    std::lock_guard<std::mutex> lock(appState.dataMutex);
    appState.receivedFiles.push_back(file);
    appState.totalFilesReceived++;
    appState.totalBytesReceived += file.data.size();
}

void messageCallback(const std::string& message) {
    std::lock_guard<std::mutex> lock(appState.dataMutex);
    std::string timestampedMessage = "[" + getCurrentTimestamp() + "] " + message;
    appState.receivedMessages.push_back(timestampedMessage);
    appState.totalMessagesReceived++;
}

// Async analysis function
void performAsyncAnalysis(const AnariUsdMiddleware::FileData& fileData) {
    std::vector<AnariUsdMiddleware::MeshData> meshData;
    bool success = false;
    std::string error;

    try {
        // Check if we should use pre-loaded data
        if (appState.connectionMode == AppState::ConnectionMode::DISK_LOADING && appState.hasMeshData) {
            std::lock_guard<std::mutex> lock(appState.meshDataMutex);
            meshData = appState.loadedMeshData;
            success = true;
        } else {
            // Process the file buffer
            success = appState.middleware.LoadUSDBuffer(fileData.data, fileData.filename, meshData);
            if (!success) {
                error = "Failed to load USD buffer";
            }
        }

        // Check for cancellation
        if (appState.cancelAnalysis) {
            return;
        }

    } catch (const std::exception& e) {
        success = false;
        error = std::string("Exception during analysis: ") + e.what();
    }

    // Store results
    {
        std::lock_guard<std::mutex> lock(appState.analysisResultsMutex);
        appState.analysisResults = meshData;
        appState.analysisError = error;
    }

    // Mark as complete
    appState.analysisComplete = true;
    appState.isAnalyzing = false;

    // Add completion message
    std::string msg = success ?
        "Analysis complete: " + std::to_string(meshData.size()) + " meshes found" :
        "Analysis failed: " + error;

    std::lock_guard<std::mutex> lock(appState.dataMutex);
    appState.receivedMessages.push_back("[" + getCurrentTimestamp() + "] " + msg);
    appState.totalMessagesReceived++;
}

// Forward declarations for helper functions
void drawSummaryTab(const std::vector<AnariUsdMiddleware::MeshData>& meshes);
void drawHierarchyTab(const std::vector<AnariUsdMiddleware::MeshData>& meshes);
void drawMeshDetailsTab(const std::vector<AnariUsdMiddleware::MeshData>& meshes);
void drawExportTab(const std::vector<AnariUsdMiddleware::MeshData>& meshes, const std::string& fileName);

// UI Functions
void drawConnectionPanel() {
    ImGui::Begin("Connection Control");

    // Connection mode dropdown
    ImGui::Text("Connection Mode:");
    ImGui::SetNextItemWidth(200);
    const char* modes[] = { "ZMQ Network", "Load from Disk" };
    int currentMode = static_cast<int>(appState.connectionMode);
    if (ImGui::Combo("##connection_mode", &currentMode, modes, IM_ARRAYSIZE(modes))) {
        appState.connectionMode = static_cast<AppState::ConnectionMode>(currentMode);
        // Reset connection state when switching modes
        if (appState.isConnected) {
            appState.middleware.stopReceiving();
            appState.middleware.shutdown();
            appState.isConnected = false;
            appState.isReceiving = false;
        }
    }

    ImGui::Separator();

    if (appState.connectionMode == AppState::ConnectionMode::ZMQ_NETWORK) {
        // ZMQ Network Mode UI
        ImGui::Text("ZMQ Network Settings");
        ImGui::Text("IP Address:");
        ImGui::SameLine();
        ImGui::InputText("##ip", appState.ipAddress, sizeof(appState.ipAddress));

        ImGui::Text("Port:");
        ImGui::SameLine();
        ImGui::InputInt("##port", &appState.port);

        ImGui::Spacing();

        // Connection status with Font Awesome icons
        if (appState.isConnected) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), ICON_FA_CIRCLE " Connected");
        } else {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), ICON_FA_CIRCLE " Disconnected");
        }

        ImGui::SameLine();
        if (appState.isReceiving) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "| Receiving");
        } else {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "| Stopped");
        }

        ImGui::Spacing();

        // ZMQ Control buttons
        if (!appState.isConnected) {
            if (ImGui::Button("Start ZMQ Connection", ImVec2(200, 30))) {
                std::string endpoint = "tcp://" + std::string(appState.ipAddress) + ":" + std::to_string(appState.port);
                if (appState.middleware.initialize(endpoint.c_str())) {
                    appState.isConnected = true;
                    appState.fileCallbackId = appState.middleware.registerUpdateCallback(fileCallback);
                    appState.messageCallbackId = appState.middleware.registerMessageCallback(messageCallback);
                    if (appState.middleware.startReceiving()) {
                        appState.isReceiving = true;
                        appState.startTime = std::chrono::steady_clock::now();
                    }
                }
            }
        } else {
            if (ImGui::Button("Stop ZMQ Connection", ImVec2(200, 30))) {
                appState.middleware.stopReceiving();
                appState.middleware.shutdown();
                appState.isReceiving = false;
                appState.isConnected = false;
                if (appState.fileCallbackId >= 0) {
                    appState.middleware.unregisterUpdateCallback(appState.fileCallbackId);
                    appState.fileCallbackId = -1;
                }
                if (appState.messageCallbackId >= 0) {
                    appState.middleware.unregisterMessageCallback(appState.messageCallbackId);
                    appState.messageCallbackId = -1;
                }
            }
        }

        ImGui::Spacing();
        ImGui::Text("Current Endpoint: tcp://%s:%d", appState.ipAddress, appState.port);

    } else {
        // Disk Loading Mode UI
        ImGui::Text("Disk Loading Settings");
        ImGui::Text("USD File Path:");
        ImGui::InputText("##disk_path", appState.diskFilePath, sizeof(appState.diskFilePath));
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            appState.showFileDialog = true;
        }

        // Simple file dialog
        if (appState.showFileDialog) {
            ImGui::OpenPopup("Select USD File");
            appState.showFileDialog = false;
        }

        if (ImGui::BeginPopupModal("Select USD File", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter the full path to your USD file:");
            ImGui::InputText("##file_path_input", appState.diskFilePath, sizeof(appState.diskFilePath));
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Spacing();

        // File validation
        bool fileExists = strlen(appState.diskFilePath) > 0 && std::filesystem::exists(appState.diskFilePath);
        if (strlen(appState.diskFilePath) > 0) {
            if (fileExists) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), ICON_FA_CHECK " File found");
            } else {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), ICON_FA_TIMES " File not found");
            }
        }

        ImGui::Spacing();

        // Load from disk button
        if (ImGui::Button("Load USD File", ImVec2(200, 30))) {
            if (fileExists) {
                // Initialize middleware for disk loading (no ZMQ needed)
                if (appState.middleware.initialize(nullptr)) {
                    std::vector<AnariUsdMiddleware::MeshData> meshData;
                    if (appState.middleware.LoadUSDFromDisk(appState.diskFilePath, meshData)) {
                        // Store mesh data for GUI access
                        {
                            std::lock_guard<std::mutex> lock(appState.meshDataMutex);
                            appState.loadedMeshData = meshData;
                            appState.hasMeshData = true;
                        }

                        // Create a FileData structure to simulate received file
                        AnariUsdMiddleware::FileData fileData;
                        fileData.filename = std::filesystem::path(appState.diskFilePath).filename().string();
                        fileData.fileType = "USD";

                        // Read file into data buffer
                        std::ifstream file(appState.diskFilePath, std::ios::binary);
                        if (file) {
                            file.seekg(0, std::ios::end);
                            size_t fileSize = file.tellg();
                            file.seekg(0, std::ios::beg);
                            fileData.data.resize(fileSize);
                            file.read(reinterpret_cast<char*>(fileData.data.data()), fileSize);
                            fileData.hash = "disk_loaded";
                        }

                        // Add to received files list
                        {
                            std::lock_guard<std::mutex> lock(appState.dataMutex);
                            appState.receivedFiles.push_back(fileData);
                            appState.totalFilesReceived++;
                            appState.totalBytesReceived += fileData.data.size();
                        }

                        // Add success message with detailed info
                        std::string successMsg = "Successfully loaded " + std::to_string(meshData.size()) + " meshes from disk";
                        size_t totalVertices = 0, totalTriangles = 0;
                        for (const auto& mesh : meshData) {
                            totalVertices += mesh.points.size() / 3;
                            totalTriangles += mesh.indices.size() / 3;
                        }
                        successMsg += " (" + std::to_string(totalVertices) + " vertices, " + std::to_string(totalTriangles) + " triangles)";

                        {
                            std::lock_guard<std::mutex> lock(appState.dataMutex);
                            appState.receivedMessages.push_back("[" + getCurrentTimestamp() + "] " + successMsg);
                            appState.totalMessagesReceived++;
                        }

                        appState.isConnected = true;
                    } else {
                        // Add error message
                        std::lock_guard<std::mutex> lock(appState.dataMutex);
                        appState.receivedMessages.push_back("[" + getCurrentTimestamp() + "] Failed to load USD file: " + appState.diskFilePath);
                        appState.totalMessagesReceived++;
                    }
                }
            }
        }

        if (!fileExists && strlen(appState.diskFilePath) > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "(File not found)");
        }

        ImGui::Spacing();
        ImGui::Text("Selected File: %s", strlen(appState.diskFilePath) > 0 ? appState.diskFilePath : "None");
    }

    ImGui::Separator();

    // Clear data button (common to both modes)
    if (ImGui::Button("Clear All Data", ImVec2(200, 30))) {
        std::lock_guard<std::mutex> lock(appState.dataMutex);
        appState.receivedFiles.clear();
        appState.receivedMessages.clear();
        appState.totalFilesReceived = 0;
        appState.totalMessagesReceived = 0;
        appState.totalBytesReceived = 0;
        appState.selectedFileIndex = -1;
    }

    ImGui::End();
}

void drawMetricsPanel() {
    if (!appState.showMetrics) return;

    ImGui::Begin("Metrics & Statistics", &appState.showMetrics);
    ImGui::Text("Session Statistics");
    ImGui::Separator();
    ImGui::Text("Files Received: %d", appState.totalFilesReceived);
    ImGui::Text("Messages Received: %d", appState.totalMessagesReceived);
    ImGui::Text("Total Data: %s", formatBytes(appState.totalBytesReceived).c_str());

    if (appState.isReceiving) {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - appState.startTime);
        ImGui::Text("Session Time: %ld seconds", duration.count());

        if (duration.count() > 0) {
            double filesPerSecond = static_cast<double>(appState.totalFilesReceived) / duration.count();
            double bytesPerSecond = static_cast<double>(appState.totalBytesReceived) / duration.count();
            ImGui::Text("Files/sec: %.2f", filesPerSecond);
            ImGui::Text("Data Rate: %s/sec", formatBytes(static_cast<size_t>(bytesPerSecond)).c_str());
        }
    }

    ImGui::Spacing();
    ImGui::Text("Current Endpoint: tcp://%s:%d", appState.ipAddress, appState.port);
    ImGui::End();
}

void drawFileListPanel() {
    if (!appState.showFileList) return;

    // Static variables at function scope - accessible throughout the entire function
    static std::vector<AnariUsdMiddleware::MeshData> lastAnalyzedMeshes;
    static std::string lastAnalyzedFile;
    static bool showAnalysis = false;

    ImGui::Begin("Received Files", &appState.showFileList);

    std::lock_guard<std::mutex> lock(appState.dataMutex);

    if (appState.receivedFiles.empty()) {
        ImGui::Text("No files received yet...");
        ImGui::Text(ICON_FA_INFO " Connect via ZMQ or load a USD file from disk to get started");
    } else {
        ImGui::Text("Files (%zu):", appState.receivedFiles.size());
        ImGui::Separator();

        // File list with improved selection handling
        for (int i = 0; i < static_cast<int>(appState.receivedFiles.size()); i++) {
            const auto& file = appState.receivedFiles[i];
            bool isSelected = (appState.selectedFileIndex == i);

            // Add file type icon
            std::string icon = ICON_FA_FILE;
            if (file.fileType == "USD") icon = ICON_FA_FILM;
            else if (file.fileType == "IMAGE") icon = ICON_FA_IMAGE;

            std::string displayName = icon + std::string(" ") + file.filename;

            if (ImGui::Selectable(displayName.c_str(), isSelected)) {
                appState.selectedFileIndex = i;
            }

            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(ICON_FA_FOLDER " Type: %s", file.fileType.c_str());
                ImGui::Text(ICON_FA_RULER " Size: %s", formatBytes(file.data.size()).c_str());
                ImGui::Text(ICON_FA_HASHTAG " Hash: %s", file.hash.substr(0, 16).c_str());
                ImGui::EndTooltip();
            }
        }

        ImGui::Spacing();

        // File details panel
        if (appState.selectedFileIndex >= 0 &&
            appState.selectedFileIndex < static_cast<int>(appState.receivedFiles.size())) {

            const auto& selectedFile = appState.receivedFiles[appState.selectedFileIndex];

            ImGui::Separator();
            ImGui::Text(ICON_FA_INFO " FILE DETAILS");
            ImGui::Spacing();

            // File information in a more organized layout
            if (ImGui::BeginTable("FileDetails", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text(ICON_FA_FOLDER " Name");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%s", selectedFile.filename.c_str());

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text(ICON_FA_TAG " Type");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%s", selectedFile.fileType.c_str());

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text(ICON_FA_RULER " Size");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%s", formatBytes(selectedFile.data.size()).c_str());

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text(ICON_FA_HASHTAG " Hash");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%s", selectedFile.hash.c_str());

                ImGui::EndTable();
            }

            ImGui::Spacing();

            // USD Analysis Section
            if (selectedFile.fileType == "USD") {
                ImGui::Separator();
                ImGui::Text(ICON_FA_SEARCH " USD ANALYSIS");
                ImGui::Spacing();

                if (!appState.isAnalyzing) {
                    if (ImGui::Button(ICON_FA_SEARCH " Analyze USD File", ImVec2(200, 35))) {
                        // Start async analysis
                        appState.isAnalyzing = true;
                        appState.analysisComplete = false;
                        appState.cancelAnalysis = false;
                        appState.analysisFileName = selectedFile.filename;

                        // Launch analysis in background thread
                        appState.analysisFuture = std::async(std::launch::async, performAsyncAnalysis, selectedFile);
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "Click to extract mesh data and hierarchy");
                } else {
                    // Show progress while analyzing
                    ImGui::Button(ICON_FA_SPINNER " Analyzing... Please wait", ImVec2(200, 35));
                    ImGui::SameLine();

                    // Animated spinner
                    static float spinner = 0.0f;
                    spinner += 0.15f;
                    if (spinner > 6.28f) spinner = 0.0f;
                    ImGui::Text(ICON_FA_SPINNER);

                    // Cancel option
                    if (ImGui::Button(ICON_FA_TIMES " Cancel Analysis", ImVec2(150, 25))) {
                        appState.cancelAnalysis = true;
                        appState.isAnalyzing = false;
                        appState.analysisComplete = false;
                    }
                }
            }
        }
    }

    // Handle analysis completion
    if (appState.analysisComplete) {
        std::lock_guard<std::mutex> analysisLock(appState.analysisResultsMutex);

        if (!appState.analysisError.empty()) {
            // Show error message
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 0, 0, 1), ICON_FA_TIMES " Analysis Error: %s", appState.analysisError.c_str());
        } else if (!appState.analysisResults.empty()) {
            // Store analysis results in static variables
            lastAnalyzedMeshes = appState.analysisResults;
            lastAnalyzedFile = appState.analysisFileName;
            showAnalysis = true;
        }

        // Reset analysis state
        appState.analysisComplete = false;
        appState.analysisResults.clear();
        appState.analysisError.clear();
    }

    // Display detailed USD analysis results
    if (showAnalysis && !lastAnalyzedMeshes.empty()) {
        ImGui::Separator();
        ImGui::Text(ICON_FA_SEARCH " USD ANALYSIS RESULTS FOR: %s", lastAnalyzedFile.c_str());

        if (ImGui::Button(ICON_FA_TIMES " Hide Analysis")) {
            showAnalysis = false;
        }

        ImGui::Spacing();

        // Create tabs for different analysis views
        if (ImGui::BeginTabBar("AnalysisTabs")) {

            // TAB 1: SUMMARY & STATISTICS
            if (ImGui::BeginTabItem(ICON_FA_CHART_BAR " Summary")) {
                drawSummaryTab(lastAnalyzedMeshes);
                ImGui::EndTabItem();
            }

            // TAB 2: USD HIERARCHY
            if (ImGui::BeginTabItem(ICON_FA_SITEMAP " USD Hierarchy")) {
                drawHierarchyTab(lastAnalyzedMeshes);
                ImGui::EndTabItem();
            }

            // TAB 3: DETAILED MESH DATA
            if (ImGui::BeginTabItem(ICON_FA_CUBE " Mesh Details")) {
                drawMeshDetailsTab(lastAnalyzedMeshes);
                ImGui::EndTabItem();
            }

            // TAB 4: EXPORT OPTIONS
            if (ImGui::BeginTabItem(ICON_FA_SAVE " Export")) {
                drawExportTab(lastAnalyzedMeshes, lastAnalyzedFile);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }

    ImGui::End();
}

void drawMessagesPanel() {
    if (!appState.showMessages) return;

    ImGui::Begin("Messages", &appState.showMessages);

    std::lock_guard<std::mutex> lock(appState.dataMutex);

    if (appState.receivedMessages.empty()) {
        ImGui::Text("No messages received yet...");
    } else {
        ImGui::Text("Messages (%zu):", appState.receivedMessages.size());
        ImGui::Separator();

        ImGui::BeginChild("MessageList", ImVec2(0, 200), true);
        for (const auto& message : appState.receivedMessages) {
            ImGui::TextWrapped("%s", message.c_str());
        }

        // Auto-scroll to bottom
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    ImGui::End();
}
// Forward declarations for helper functions
void drawSummaryTab(const std::vector<AnariUsdMiddleware::MeshData>& meshes);
void drawHierarchyTab(const std::vector<AnariUsdMiddleware::MeshData>& meshes);
void drawMeshDetailsTab(const std::vector<AnariUsdMiddleware::MeshData>& meshes);
void drawExportTab(const std::vector<AnariUsdMiddleware::MeshData>& meshes, const std::string& fileName);

// ADD THESE MISSING DECLARATIONS:
void ModernDarkTheme();
void ModernLightTheme();
void ApplyAccentColor();
void drawAdvancedColorPicker();
void toggleTheme();


void ModernDarkTheme() {
    auto& style = ImGui::GetStyle();
    style.ChildRounding = 0;
    style.GrabRounding = 0;
    style.FrameRounding = 2;
    style.PopupRounding = 0;
    style.ScrollbarRounding = 0;
    style.TabRounding = 2;
    style.WindowRounding = 0;
    style.FramePadding = { 4, 4 };

    style.WindowTitleAlign = { 0.0, 0.5 };
    style.ColorButtonPosition = ImGuiDir_Left;

    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_Text] = { 1.0f, 1.0f, 1.0f, 1.00f };
    colors[ImGuiCol_TextDisabled] = { 0.25f, 0.25f, 0.25f, 1.00f };
    colors[ImGuiCol_WindowBg] = { 0.09f, 0.09f, 0.09f, 0.94f };
    colors[ImGuiCol_ChildBg] = { 0.11f, 0.11f, 0.11f, 1.00f };
    colors[ImGuiCol_PopupBg] = { 0.11f, 0.11f, 0.11f, 0.94f };
    colors[ImGuiCol_Border] = { 0.07f, 0.08f, 0.08f, 1.00f };
    colors[ImGuiCol_BorderShadow] = { 0.00f, 0.00f, 0.00f, 0.00f };
    colors[ImGuiCol_FrameBg] = { 0.35f, 0.35f, 0.35f, 0.54f };
    colors[ImGuiCol_FrameBgHovered] = { 0.31f, 0.29f, 0.27f, 1.00f };
    colors[ImGuiCol_FrameBgActive] = { 0.40f, 0.36f, 0.33f, 0.67f };
    colors[ImGuiCol_TitleBg] = { 0.1f, 0.1f, 0.1f, 1.00f };
    colors[ImGuiCol_TitleBgActive] = { 0.3f, 0.3f, 0.3f, 1.00f };
    colors[ImGuiCol_TitleBgCollapsed] = { 0.0f, 0.0f, 0.0f, 0.61f };
    colors[ImGuiCol_MenuBarBg] = { 0.18f, 0.18f, 0.18f, 0.94f };
    colors[ImGuiCol_ScrollbarBg] = { 0.00f, 0.00f, 0.00f, 0.16f };
    colors[ImGuiCol_ScrollbarGrab] = { 0.24f, 0.22f, 0.21f, 1.00f };
    colors[ImGuiCol_ScrollbarGrabHovered] = { 0.31f, 0.29f, 0.27f, 1.00f };
    colors[ImGuiCol_ScrollbarGrabActive] = { 0.40f, 0.36f, 0.33f, 1.00f };
    colors[ImGuiCol_CheckMark] = { 0.84f, 0.84f, 0.84f, 1.0f };
    colors[ImGuiCol_SliderGrab] = { 0.8f, 0.8f, 0.8f, 1.0f };
    colors[ImGuiCol_SliderGrabActive] = { 0.55f, 0.55f, 0.55f, 1.00f };
    colors[ImGuiCol_Button] = { 0.55f, 0.55f, 0.55f, 0.40f };
    colors[ImGuiCol_ButtonHovered] = { 0.15f, 0.15f, 0.15f, 0.62f };
    colors[ImGuiCol_ButtonActive] = { 0.60f, 0.60f, 0.60f, 1.00f };
    colors[ImGuiCol_Header] = { 0.84f, 0.36f, 0.05f, 0.0f };
    colors[ImGuiCol_HeaderHovered] = { 0.25f, 0.25f, 0.25f, 0.80f };
    colors[ImGuiCol_HeaderActive] = { 0.42f, 0.42f, 0.42f, 1.00f };
    colors[ImGuiCol_Separator] = { 0.35f, 0.35f, 0.35f, 0.50f };
    colors[ImGuiCol_SeparatorHovered] = { 0.31f, 0.29f, 0.27f, 0.78f };
    colors[ImGuiCol_SeparatorActive] = { 0.40f, 0.36f, 0.33f, 1.00f };
    colors[ImGuiCol_ResizeGrip] = { 1.0f, 1.0f, 1.0f, 0.25f };
    colors[ImGuiCol_ResizeGripHovered] = { 1.00f, 1.0f, 1.0f, 0.4f };
    colors[ImGuiCol_ResizeGripActive] = { 1.00f, 1.00f, 1.0f, 0.95f };
    colors[ImGuiCol_Tab] = { 0.18f, 0.18f, 0.18f, 1.0f };
    colors[ImGuiCol_PlotLines] = { 0.66f, 0.60f, 0.52f, 1.00f };
    colors[ImGuiCol_PlotLinesHovered] = { 0.98f, 0.29f, 0.20f, 1.00f };
    colors[ImGuiCol_PlotHistogram] = { 0.60f, 0.59f, 0.10f, 1.00f };
    colors[ImGuiCol_PlotHistogramHovered] = { 0.72f, 0.73f, 0.15f, 1.00f };
    colors[ImGuiCol_TextSelectedBg] = { 0.27f, 0.52f, 0.53f, 0.35f };
    colors[ImGuiCol_DragDropTarget] = { 0.60f, 0.59f, 0.10f, 0.90f };
    colors[ImGuiCol_NavHighlight] = { 0.51f, 0.65f, 0.60f, 1.00f };
    colors[ImGuiCol_NavWindowingHighlight] = { 1.00f, 1.00f, 1.00f, 0.70f };
    colors[ImGuiCol_NavWindowingDimBg] = { 0.80f, 0.80f, 0.80f, 0.20f };
    colors[ImGuiCol_ModalWindowDimBg] = { 0.11f, 0.13f, 0.13f, 0.35f };
}
// Add accent color functionality
static ImVec4 accentColor = ImVec4(0.26f, 0.59f, 0.98f, 1.00f); // Default blue accent

void ModernLightTheme() {
    auto& style = ImGui::GetStyle();
    style.ChildRounding = 0;
    style.GrabRounding = 0;
    style.FrameRounding = 2;
    style.PopupRounding = 0;
    style.ScrollbarRounding = 0;
    style.TabRounding = 2;
    style.WindowRounding = 0;
    style.FramePadding = { 4, 4 };
    style.WindowTitleAlign = { 0.0, 0.5 };
    style.ColorButtonPosition = ImGuiDir_Left;

    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_Text] = { 0.00f, 0.00f, 0.00f, 1.00f };
    colors[ImGuiCol_TextDisabled] = { 0.60f, 0.60f, 0.60f, 1.00f };
    colors[ImGuiCol_WindowBg] = { 0.94f, 0.94f, 0.94f, 1.00f };
    colors[ImGuiCol_ChildBg] = { 0.00f, 0.00f, 0.00f, 0.00f };
    colors[ImGuiCol_PopupBg] = { 1.00f, 1.00f, 1.00f, 0.98f };
    colors[ImGuiCol_Border] = { 0.00f, 0.00f, 0.00f, 0.30f };
    colors[ImGuiCol_BorderShadow] = { 0.00f, 0.00f, 0.00f, 0.00f };
    colors[ImGuiCol_FrameBg] = { 1.00f, 1.00f, 1.00f, 1.00f };
    colors[ImGuiCol_FrameBgHovered] = { 0.26f, 0.59f, 0.98f, 0.40f };
    colors[ImGuiCol_FrameBgActive] = { 0.26f, 0.59f, 0.98f, 0.67f };
    colors[ImGuiCol_TitleBg] = { 0.96f, 0.96f, 0.96f, 1.00f };
    colors[ImGuiCol_TitleBgActive] = { 0.82f, 0.82f, 0.82f, 1.00f };
    colors[ImGuiCol_TitleBgCollapsed] = { 1.00f, 1.00f, 1.00f, 0.51f };
    colors[ImGuiCol_MenuBarBg] = { 0.86f, 0.86f, 0.86f, 1.00f };
    colors[ImGuiCol_ScrollbarBg] = { 0.98f, 0.98f, 0.98f, 0.53f };
    colors[ImGuiCol_ScrollbarGrab] = { 0.69f, 0.69f, 0.69f, 0.80f };
    colors[ImGuiCol_ScrollbarGrabHovered] = { 0.49f, 0.49f, 0.49f, 0.80f };
    colors[ImGuiCol_ScrollbarGrabActive] = { 0.49f, 0.49f, 0.49f, 1.00f };
    colors[ImGuiCol_CheckMark] = { 0.26f, 0.59f, 0.98f, 1.00f };
    colors[ImGuiCol_SliderGrab] = { 0.26f, 0.59f, 0.98f, 0.78f };
    colors[ImGuiCol_SliderGrabActive] = { 0.26f, 0.59f, 0.98f, 1.00f };
    colors[ImGuiCol_Button] = { 0.26f, 0.59f, 0.98f, 0.40f };
    colors[ImGuiCol_ButtonHovered] = { 0.26f, 0.59f, 0.98f, 1.00f };
    colors[ImGuiCol_ButtonActive] = { 0.06f, 0.53f, 0.98f, 1.00f };
    colors[ImGuiCol_Header] = { 0.26f, 0.59f, 0.98f, 0.31f };
    colors[ImGuiCol_HeaderHovered] = { 0.26f, 0.59f, 0.98f, 0.80f };
    colors[ImGuiCol_HeaderActive] = { 0.26f, 0.59f, 0.98f, 1.00f };
    colors[ImGuiCol_Separator] = { 0.39f, 0.39f, 0.39f, 0.62f };
    colors[ImGuiCol_SeparatorHovered] = { 0.14f, 0.44f, 0.80f, 0.78f };
    colors[ImGuiCol_SeparatorActive] = { 0.14f, 0.44f, 0.80f, 1.00f };
    colors[ImGuiCol_ResizeGrip] = { 0.35f, 0.35f, 0.35f, 0.17f };
    colors[ImGuiCol_ResizeGripHovered] = { 0.26f, 0.59f, 0.98f, 0.67f };
    colors[ImGuiCol_ResizeGripActive] = { 0.26f, 0.59f, 0.98f, 0.95f };
    colors[ImGuiCol_Tab] = { 0.95f, 0.95f, 0.95f, 0.93f };
    colors[ImGuiCol_TabHovered] = { 0.26f, 0.59f, 0.98f, 0.80f };
    colors[ImGuiCol_TabActive] = { 0.85f, 0.90f, 0.95f, 1.00f };
    colors[ImGuiCol_TabUnfocused] = { 0.98f, 0.98f, 0.98f, 0.99f };
    colors[ImGuiCol_TabUnfocusedActive] = { 0.92f, 0.92f, 0.92f, 1.00f };
    colors[ImGuiCol_PlotLines] = { 0.39f, 0.39f, 0.39f, 1.00f };
    colors[ImGuiCol_PlotLinesHovered] = { 1.00f, 0.43f, 0.35f, 1.00f };
    colors[ImGuiCol_PlotHistogram] = { 0.90f, 0.70f, 0.00f, 1.00f };
    colors[ImGuiCol_PlotHistogramHovered] = { 1.00f, 0.45f, 0.00f, 1.00f };
    colors[ImGuiCol_TextSelectedBg] = { 0.26f, 0.59f, 0.98f, 0.35f };
    colors[ImGuiCol_DragDropTarget] = { 0.26f, 0.59f, 0.98f, 0.95f };
    colors[ImGuiCol_NavHighlight] = { 0.26f, 0.59f, 0.98f, 0.80f };
    colors[ImGuiCol_NavWindowingHighlight] = { 0.70f, 0.70f, 0.70f, 0.70f };
    colors[ImGuiCol_NavWindowingDimBg] = { 0.20f, 0.20f, 0.20f, 0.20f };
    colors[ImGuiCol_ModalWindowDimBg] = { 0.20f, 0.20f, 0.20f, 0.35f };
}

static bool isDarkMode = true;

void toggleTheme() {
    isDarkMode = !isDarkMode;
    if (isDarkMode) {
        ModernDarkTheme();
    } else {
        ModernLightTheme();
    }
    ApplyAccentColor(); // Reapply accent colors after theme change
}

void ApplyAccentColor() {
    ImVec4* colors = ImGui::GetStyle().Colors;

    // Adjust accent color intensity based on theme
    float intensity = isDarkMode ? 1.0f : 0.8f;
    ImVec4 adjustedAccent = ImVec4(
        accentColor.x * intensity,
        accentColor.y * intensity,
        accentColor.z * intensity,
        accentColor.w
    );

    // Apply accent color to key UI elements
    colors[ImGuiCol_Button] = ImVec4(adjustedAccent.x, adjustedAccent.y, adjustedAccent.z, 0.40f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(adjustedAccent.x, adjustedAccent.y, adjustedAccent.z, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(adjustedAccent.x * 0.8f, adjustedAccent.y * 0.8f, adjustedAccent.z * 0.8f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(adjustedAccent.x, adjustedAccent.y, adjustedAccent.z, 0.31f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(adjustedAccent.x, adjustedAccent.y, adjustedAccent.z, 0.80f);
    colors[ImGuiCol_HeaderActive] = ImVec4(adjustedAccent.x, adjustedAccent.y, adjustedAccent.z, 1.00f);

    // Enhanced tab colors
    colors[ImGuiCol_Tab] = ImVec4(adjustedAccent.x * 0.3f, adjustedAccent.y * 0.3f, adjustedAccent.z * 0.3f, 0.86f);
    colors[ImGuiCol_TabHovered] = ImVec4(adjustedAccent.x, adjustedAccent.y, adjustedAccent.z, 0.80f);
    colors[ImGuiCol_TabActive] = ImVec4(adjustedAccent.x, adjustedAccent.y, adjustedAccent.z, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(adjustedAccent.x * 0.15f, adjustedAccent.y * 0.15f, adjustedAccent.z * 0.15f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(adjustedAccent.x * 0.6f, adjustedAccent.y * 0.6f, adjustedAccent.z * 0.6f, 1.00f);

    // Other UI elements
    colors[ImGuiCol_CheckMark] = adjustedAccent;
    colors[ImGuiCol_SliderGrab] = adjustedAccent;
    colors[ImGuiCol_SliderGrabActive] = ImVec4(adjustedAccent.x * 0.8f, adjustedAccent.y * 0.8f, adjustedAccent.z * 0.8f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(adjustedAccent.x, adjustedAccent.y, adjustedAccent.z, 0.20f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(adjustedAccent.x, adjustedAccent.y, adjustedAccent.z, 0.30f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(adjustedAccent.x, adjustedAccent.y, adjustedAccent.z, 0.35f);
}


void drawAdvancedColorPicker() {
    // Add theme toggle at the top
    ImGui::Text(ICON_FA_PALETTE " Theme Mode:");
    ImGui::Spacing();

    // Theme toggle button
    if (ImGui::Button(isDarkMode ? ICON_FA_CIRCLE " Dark Mode" : ICON_FA_CIRCLE " Light Mode", ImVec2(150, 30))) {
        toggleTheme();
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "Click to switch theme");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Your existing color picker code...
    ImGui::Text(ICON_FA_PALETTE " Custom Accent Color:");
    ImGui::Spacing();

    // Color swatches for quick selection
    ImGui::Text(ICON_FA_PALETTE " Quick Color Swatches:");

    struct ColorSwatch {
        const char* name;
        ImVec4 color;
    };

    ColorSwatch swatches[] = {
        {"Material Blue", ImVec4(0.13f, 0.59f, 0.95f, 1.00f)},
        {"GitHub Purple", ImVec4(0.41f, 0.24f, 0.68f, 1.00f)},
        {"VS Code Blue", ImVec4(0.00f, 0.47f, 0.84f, 1.00f)},
        {"Spotify Green", ImVec4(0.11f, 0.73f, 0.33f, 1.00f)},
        {"Discord Purple", ImVec4(0.35f, 0.39f, 0.96f, 1.00f)},
        {"Orange", ImVec4(1.00f, 0.60f, 0.00f, 1.00f)},
        {"Pink", ImVec4(0.91f, 0.12f, 0.39f, 1.00f)},
        {"Teal", ImVec4(0.00f, 0.59f, 0.53f, 1.00f)},
        {"Red", ImVec4(0.96f, 0.26f, 0.21f, 1.00f)},
        {"Amber", ImVec4(1.00f, 0.76f, 0.03f, 1.00f)},
        {"Cyan", ImVec4(0.00f, 0.74f, 0.83f, 1.00f)},
        {"Indigo", ImVec4(0.25f, 0.32f, 0.71f, 1.00f)}
    };

    // Draw swatches in a 4x3 grid
    for (int i = 0; i < 12; i++) {
        if (i % 4 != 0) ImGui::SameLine();

        ImGui::PushID(i);
        if (ImGui::ColorButton(swatches[i].name, swatches[i].color,
                              ImGuiColorEditFlags_NoTooltip, ImVec2(45, 35))) {
            accentColor = swatches[i].color;
            ApplyAccentColor();
        }
        ImGui::PopID();

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\nRGB: %.0f, %.0f, %.0f",
                             swatches[i].name,
                             swatches[i].color.x * 255,
                             swatches[i].color.y * 255,
                             swatches[i].color.z * 255);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Preset themes section
    ImGui::Text(ICON_FA_COG " Theme Presets:");

    struct ThemePreset {
        const char* name;
        ImVec4 accent;
        const char* description;
    };

    ThemePreset themes[] = {
        {"Professional Blue", ImVec4(0.26f, 0.59f, 0.98f, 1.00f), "Clean corporate look"},
        {"Creative Purple", ImVec4(0.60f, 0.26f, 0.98f, 1.00f), "Artistic and modern"},
        {"Gaming Green", ImVec4(0.26f, 0.98f, 0.26f, 1.00f), "High-tech gaming style"},
        {"Warning Orange", ImVec4(1.00f, 0.60f, 0.00f, 1.00f), "Attention-grabbing"},
        {"Elegant Pink", ImVec4(0.91f, 0.12f, 0.39f, 1.00f), "Sophisticated design"},
        {"Ocean Teal", ImVec4(0.00f, 0.59f, 0.53f, 1.00f), "Calm and professional"}
    };

    for (int i = 0; i < 6; i++) {
        if (i % 2 != 0) ImGui::SameLine();

        ImGui::PushID(100 + i);

        // Color preview button
        if (ImGui::ColorButton("##theme_preview", themes[i].accent,
                              ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20))) {
            accentColor = themes[i].accent;
            ApplyAccentColor();
        }

        ImGui::SameLine();

        // Theme name button
        if (ImGui::Button(themes[i].name, ImVec2(120, 20))) {
            accentColor = themes[i].accent;
            ApplyAccentColor();
        }

        ImGui::PopID();

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n%s", themes[i].name, themes[i].description);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Current color info
    ImGui::Text(ICON_FA_INFO " Current Accent Color:");
    ImGui::Text("RGB: %.0f, %.0f, %.0f",
               accentColor.x * 255, accentColor.y * 255, accentColor.z * 255);

    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(accentColor.x, accentColor.y, accentColor.z, h, s, v);
    ImGui::Text("HSV: %.0f°, %.0f%%, %.0f%%", h * 360, s * 100, v * 100);

    // Hex color display
    ImGui::Text("Hex: #%02X%02X%02X",
               (int)(accentColor.x * 255),
               (int)(accentColor.y * 255),
               (int)(accentColor.z * 255));

    // Reset button
    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_TIMES " Reset to Default", ImVec2(150, 25))) {
        accentColor = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        ApplyAccentColor();
    }
}



void drawMenuBar() {
    static bool showSettings = false;

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Metrics", nullptr, &appState.showMetrics);
            ImGui::MenuItem("File List", nullptr, &appState.showFileList);
            ImGui::MenuItem("Messages", nullptr, &appState.showMessages);
            ImGui::Separator();
            ImGui::MenuItem(ICON_FA_COG " Settings", nullptr, &showSettings);
            ImGui::Separator();
            // Add theme toggle to menu
            if (ImGui::MenuItem(isDarkMode ? ICON_FA_CIRCLE " Switch to Light Mode" : ICON_FA_CIRCLE " Switch to Dark Mode")) {
                toggleTheme();
            }
            ImGui::EndMenu();

        }

        ImGui::EndMenuBar();
    }

    // Add the settings window here
    if (showSettings) {
        ImGui::SetNextWindowSize(ImVec2(500, 600), ImGuiCond_FirstUseEver);
        ImGui::Begin(ICON_FA_COG " Settings", &showSettings);

        ImGui::Text(ICON_FA_PALETTE " Theme Customization");
        ImGui::Separator();
        ImGui::Spacing();

        // Create tabs for different settings
        if (ImGui::BeginTabBar("SettingsTabs")) {

            if (ImGui::BeginTabItem(ICON_FA_PALETTE " Colors")) {
                drawAdvancedColorPicker();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(ICON_FA_COG " General")) {
                ImGui::Text("General application settings");
                ImGui::Spacing();

                static bool enableTooltips = true;
                ImGui::Checkbox("Enable Tooltips", &enableTooltips);

                static bool enableAnimations = true;
                ImGui::Checkbox("Enable Animations", &enableAnimations);

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(ICON_FA_INFO " About")) {
                ImGui::Text("USD Analysis Tool");
                ImGui::Separator();
                ImGui::Text("Version: 1.0.0");
                ImGui::Text("Built with Dear ImGui and TinyUSDZ");
                ImGui::Spacing();
                ImGui::Text("Features:");
                ImGui::BulletText("USD file analysis and visualization");
                ImGui::BulletText("Mesh hierarchy exploration");
                ImGui::BulletText("Real-time ZMQ data reception");
                ImGui::BulletText("Export capabilities (CSV, JSON, TXT)");
                ImGui::BulletText("Customizable themes and colors");

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();
    }
}


// Helper function implementations
void drawSummaryTab(const std::vector<AnariUsdMiddleware::MeshData>& meshes) {
    ImGui::Spacing();

    // Calculate summary statistics
    size_t totalVertices = 0, totalTriangles = 0, totalNormals = 0, totalUVs = 0;
    size_t meshesWithData = 0;

    for (const auto& mesh : meshes) {
        size_t numVertices = mesh.points.size() / 3;
        size_t numTriangles = mesh.indices.size() / 3;
        size_t numNormals = mesh.normals.size() / 3;
        size_t numUVs = mesh.uvs.size() / 2;

        totalVertices += numVertices;
        totalTriangles += numTriangles;
        totalNormals += numNormals;
        totalUVs += numUVs;

        if (numVertices > 0) meshesWithData++;
    }

    // Display statistics
    ImGui::Text(ICON_FA_HASHTAG " Total Meshes: %zu", meshes.size());
    ImGui::Text(ICON_FA_CHECK " Meshes with Data: %zu", meshesWithData);
    ImGui::Text(ICON_FA_TIMES " Empty Meshes: %zu", meshes.size() - meshesWithData);
    ImGui::Text(ICON_FA_CUBE " Total Vertices: %zu", totalVertices);
    ImGui::Text(ICON_FA_CUBE " Total Triangles: %zu", totalTriangles);
    ImGui::Text(ICON_FA_CUBE " Total Normals: %zu", totalNormals);
    ImGui::Text(ICON_FA_CUBE " Total UVs: %zu", totalUVs);

    // Performance metrics
    if (totalVertices > 0) {
        double avgVerticesPerMesh = static_cast<double>(totalVertices) / meshesWithData;
        double avgTrianglesPerMesh = static_cast<double>(totalTriangles) / meshesWithData;

        ImGui::Spacing();
        ImGui::Text(ICON_FA_CHART_BAR " PERFORMANCE METRICS");
        ImGui::Separator();
        ImGui::Text(ICON_FA_CHART_BAR " Avg Vertices/Mesh: %.1f", avgVerticesPerMesh);
        ImGui::Text(ICON_FA_CHART_BAR " Avg Triangles/Mesh: %.1f", avgTrianglesPerMesh);

        // Memory estimation
        size_t totalMemory = (totalVertices * 3 * sizeof(float)) +     // positions
                           (totalTriangles * 3 * sizeof(uint32_t)) +   // indices
                           (totalNormals * 3 * sizeof(float)) +        // normals
                           (totalUVs * 2 * sizeof(float));             // UVs

        ImGui::Text(ICON_FA_SAVE " Est. Total Memory: %s", formatBytes(totalMemory).c_str());
    }

    ImGui::Spacing();

    // Quality indicators
    ImGui::Text(ICON_FA_INFO " QUALITY INDICATORS");
    ImGui::Separator();

    size_t meshesWithNormals = 0, meshesWithUVs = 0;
    for (const auto& mesh : meshes) {
        if (mesh.normals.size() > 0) meshesWithNormals++;
        if (mesh.uvs.size() > 0) meshesWithUVs++;
    }

    ImGui::TextColored(meshesWithNormals > 0 ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1),
                      "%s Meshes with Normals: %zu/%zu",
                      meshesWithNormals > 0 ? ICON_FA_CHECK : ICON_FA_TIMES, meshesWithNormals, meshes.size());

    ImGui::TextColored(meshesWithUVs > 0 ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1),
                      "%s Meshes with UVs: %zu/%zu",
                      meshesWithUVs > 0 ? ICON_FA_CHECK : ICON_FA_TIMES, meshesWithUVs, meshes.size());
}

void drawHierarchyTab(const std::vector<AnariUsdMiddleware::MeshData>& meshes) {
    ImGui::Spacing();

    // Build hierarchy from mesh data
    std::map<std::string, std::vector<std::string>> hierarchyMap;
    std::map<std::string, std::string> primTypes;
    std::map<std::string, const AnariUsdMiddleware::MeshData*> primMeshData;
    std::set<std::string> allPaths;

    // Build hierarchy from mesh element names
    for (const auto& mesh : meshes) {
        std::string fullPath = mesh.elementName;
        primTypes[fullPath] = mesh.typeName;
        primMeshData[fullPath] = &mesh;
        allPaths.insert(fullPath);

        // Extract parent paths
        size_t pos = 0;
        while ((pos = fullPath.find('/', pos + 1)) != std::string::npos) {
            std::string parentPath = fullPath.substr(0, pos);
            if (parentPath.empty()) parentPath = "/";
            std::string childPath = fullPath.substr(0, fullPath.find('/', pos + 1));
            if (childPath == parentPath) continue;
            hierarchyMap[parentPath].push_back(childPath);
            allPaths.insert(parentPath);
            allPaths.insert(childPath);
        }

        // Add the full path to its parent
        size_t lastSlash = fullPath.find_last_of('/');
        if (lastSlash != std::string::npos && lastSlash > 0) {
            std::string parentPath = fullPath.substr(0, lastSlash);
            hierarchyMap[parentPath].push_back(fullPath);
        } else if (lastSlash == 0) {
            hierarchyMap["/"].push_back(fullPath);
        }
    }

    // Recursive function to draw hierarchy
    std::function<void(const std::string&, int)> drawHierarchyNode =
        [&](const std::string& path, int depth) {

        std::string displayName = path;
        if (path == "/") {
            displayName = ICON_FA_HOME " Root";
        } else {
            size_t lastSlash = path.find_last_of('/');
            if (lastSlash != std::string::npos) {
                displayName = path.substr(lastSlash + 1);
            }
        }

        // Get prim type and choose icon
        std::string primType = primTypes.count(path) ? primTypes[path] : "Container";
        std::string icon = ICON_FA_FOLDER;
        if (primType == "Mesh") icon = ICON_FA_CUBE;
        else if (primType == "Material") icon = ICON_FA_PALETTE;
        else if (primType == "Shader") icon = ICON_FA_BOLT;
        else if (primType == "Camera") icon = ICON_FA_CAMERA;
        else if (primType == "Xform") icon = ICON_FA_COG;
        else if (primType == "Model") icon = ICON_FA_BOX;

        // Check if this prim has mesh data
        bool hasMeshData = primMeshData.count(path) > 0;
        size_t meshVertices = 0;
        if (hasMeshData) {
            meshVertices = primMeshData[path]->points.size() / 3;
        }

        // Create node label
        std::string nodeLabel = icon + " " + displayName + " (" + primType + ")";
        if (hasMeshData && meshVertices > 0) {
            nodeLabel += " [" + std::to_string(meshVertices) + " verts]";
        } else if (hasMeshData && meshVertices == 0) {
            nodeLabel += " [empty]";
        }

        // Check if this node has children
        bool hasChildren = hierarchyMap.count(path) && !hierarchyMap[path].empty();

        if (hasChildren) {
            if (ImGui::TreeNode(nodeLabel.c_str())) {
                // Show path info
                ImGui::Text(ICON_FA_INFO " Path: %s", path.c_str());
                ImGui::Text(ICON_FA_TAG " Type: %s", primType.c_str());

                if (hasMeshData) {
                    const auto* meshData = primMeshData[path];
                    size_t numTriangles = meshData->indices.size() / 3;
                    size_t numNormals = meshData->normals.size() / 3;
                    size_t numUVs = meshData->uvs.size() / 2;

                    ImGui::Text(ICON_FA_CHECK " Contains mesh data:");
                    ImGui::Text("  " ICON_FA_CUBE " Vertices: %zu", meshVertices);
                    ImGui::Text("  " ICON_FA_CUBE " Triangles: %zu", numTriangles);
                    ImGui::Text("  " ICON_FA_CUBE " Normals: %zu", numNormals);
                    ImGui::Text("  " ICON_FA_CUBE " UVs: %zu", numUVs);
                } else {
                    ImGui::Text(ICON_FA_FOLDER " Container/Transform node");
                }

                // Draw children
                for (const auto& childPath : hierarchyMap[path]) {
                    drawHierarchyNode(childPath, depth + 1);
                }

                ImGui::TreePop();
            }
        } else {
            // Leaf node
            ImGui::TreeNodeEx(nodeLabel.c_str(),
                            ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);

            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Path: %s", path.c_str());
                ImGui::Text("Type: %s", primType.c_str());
                if (hasMeshData) {
                    ImGui::Text("Vertices: %zu", meshVertices);
                }
                ImGui::EndTooltip();
            }
        }
    };

    // Create scrollable hierarchy view
    ImGui::BeginChild("USDHierarchy", ImVec2(0, 350), true);

    // Start from root or show all top-level items
    if (hierarchyMap.count("/") || hierarchyMap.count("")) {
        drawHierarchyNode("/", 0);
    } else {
        // If no explicit root, show all top-level items
        std::set<std::string> topLevel;
        for (const auto& path : allPaths) {
            if (path.find('/') == std::string::npos ||
                (path[0] == '/' && path.find('/', 1) == std::string::npos)) {
                topLevel.insert(path);
            }
        }

        for (const auto& path : topLevel) {
            drawHierarchyNode(path, 0);
        }
    }

    ImGui::EndChild();

    // Hierarchy statistics
    ImGui::Spacing();
    ImGui::Text(ICON_FA_CHART_BAR " HIERARCHY STATS");
    ImGui::Separator();
    ImGui::Text(ICON_FA_HASHTAG " Total Prims: %zu", allPaths.size());

    // Count by type
    std::map<std::string, int> typeCounts;
    for (const auto& [path, type] : primTypes) {
        typeCounts[type]++;
    }

    for (const auto& [type, count] : typeCounts) {
        ImGui::Text("  %s: %d", type.c_str(), count);
    }
}

void drawMeshDetailsTab(const std::vector<AnariUsdMiddleware::MeshData>& meshes) {
    ImGui::Spacing();

    // Create a scrollable region for the mesh list
    ImGui::BeginChild("MeshDetails", ImVec2(0, 400), true);

    for (size_t i = 0; i < meshes.size(); ++i) {
        const auto& mesh = meshes[i];
        size_t numVertices = mesh.points.size() / 3;
        size_t numTriangles = mesh.indices.size() / 3;
        size_t numNormals = mesh.normals.size() / 3;
        size_t numUVs = mesh.uvs.size() / 2;

        // Create a tree node for each mesh with status indicator
        std::string statusIcon = (numVertices > 0) ? ICON_FA_CHECK : ICON_FA_TIMES;
        std::string nodeLabel = statusIcon + " Mesh " + std::to_string(i + 1) + ": " +
                               mesh.elementName + " (" + mesh.typeName + ")";

        if (ImGui::TreeNode(nodeLabel.c_str())) {
            // Basic mesh info
            ImGui::Text(ICON_FA_INFO " Type: %s", mesh.typeName.c_str());
            ImGui::Text(ICON_FA_INFO " Path: %s", mesh.elementName.c_str());
            ImGui::Text(ICON_FA_CUBE " Vertices: %zu", numVertices);
            ImGui::Text(ICON_FA_CUBE " Triangles: %zu", numTriangles);
            ImGui::Text(ICON_FA_CUBE " Normals: %zu", numNormals);
            ImGui::Text(ICON_FA_CUBE " UVs: %zu", numUVs);

            // Mesh quality indicators
            if (numVertices > 0) {
                double triangleToVertexRatio = static_cast<double>(numTriangles) / numVertices;
                ImGui::Text(ICON_FA_RULER " Triangle/Vertex Ratio: %.2f", triangleToVertexRatio);

                bool hasNormals = numNormals > 0;
                bool hasUVs = numUVs > 0;

                ImGui::TextColored(hasNormals ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1),
                                  "%s Has Normals", hasNormals ? ICON_FA_CHECK : ICON_FA_TIMES);
                ImGui::TextColored(hasUVs ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1),
                                  "%s Has UVs", hasUVs ? ICON_FA_CHECK : ICON_FA_TIMES);

                // Memory usage estimation
                size_t vertexMemory = numVertices * 3 * sizeof(float);
                size_t indexMemory = numTriangles * 3 * sizeof(uint32_t);
                size_t normalMemory = numNormals * 3 * sizeof(float);
                size_t uvMemory = numUVs * 2 * sizeof(float);
                size_t totalMemory = vertexMemory + indexMemory + normalMemory + uvMemory;

                ImGui::Text(ICON_FA_SAVE " Est. Memory: %s", formatBytes(totalMemory).c_str());
            } else {
                ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), ICON_FA_EXCLAMATION " Empty mesh (no geometry data)");
            }

            // Show first few vertices for debugging
            if (numVertices > 0 && ImGui::TreeNode(ICON_FA_SEARCH " Vertex Data Preview")) {
                ImGui::Text("First few vertices:");
                size_t previewCount = std::min(numVertices, size_t(5));
                for (size_t v = 0; v < previewCount; ++v) {
                    size_t idx = v * 3;
                    ImGui::Text("  [%zu]: (%.3f, %.3f, %.3f)", v,
                               mesh.points[idx], mesh.points[idx + 1], mesh.points[idx + 2]);
                }

                if (numVertices > 5) {
                    ImGui::Text("  ... and %zu more vertices", numVertices - 5);
                }

                ImGui::TreePop();
            }

            // Show bounding box if we have vertices
            if (numVertices > 0 && ImGui::TreeNode(ICON_FA_BOX " Bounding Box")) {
                float minX = mesh.points[0], maxX = mesh.points[0];
                float minY = mesh.points[1], maxY = mesh.points[1];
                float minZ = mesh.points[2], maxZ = mesh.points[2];

                for (size_t v = 1; v < numVertices; ++v) {
                    size_t idx = v * 3;
                    minX = std::min(minX, mesh.points[idx]);
                    maxX = std::max(maxX, mesh.points[idx]);
                    minY = std::min(minY, mesh.points[idx + 1]);
                    maxY = std::max(maxY, mesh.points[idx + 1]);
                    minZ = std::min(minZ, mesh.points[idx + 2]);
                    maxZ = std::max(maxZ, mesh.points[idx + 2]);
                }

                ImGui::Text(ICON_FA_INFO " Min: (%.3f, %.3f, %.3f)", minX, minY, minZ);
                ImGui::Text(ICON_FA_INFO " Max: (%.3f, %.3f, %.3f)", maxX, maxY, maxZ);
                ImGui::Text(ICON_FA_RULER " Size: (%.3f, %.3f, %.3f)", maxX - minX, maxY - minY, maxZ - minZ);

                // Calculate center and diagonal
                float centerX = (minX + maxX) / 2.0f;
                float centerY = (minY + maxY) / 2.0f;
                float centerZ = (minZ + maxZ) / 2.0f;
                float diagonal = std::sqrt((maxX - minX) * (maxX - minX) +
                                         (maxY - minY) * (maxY - minY) +
                                         (maxZ - minZ) * (maxZ - minZ));

                ImGui::Text(ICON_FA_INFO " Center: (%.3f, %.3f, %.3f)", centerX, centerY, centerZ);
                ImGui::Text(ICON_FA_RULER " Diagonal: %.3f", diagonal);

                ImGui::TreePop();
            }

            ImGui::TreePop();
        }
    }

    ImGui::EndChild();
}

void drawExportTab(const std::vector<AnariUsdMiddleware::MeshData>& meshes, const std::string& fileName) {
    ImGui::Spacing();
    ImGui::Text("Export USD Analysis Data");
    ImGui::Separator();

    if (ImGui::Button(ICON_FA_FILE " Save Analysis Report", ImVec2(200, 30))) {
        // Save detailed analysis to file
        std::string reportPath = "usd_analysis_" + fileName + ".txt";
        std::ofstream report(reportPath);
        if (report.is_open()) {
            report << "USD Analysis Report\n";
            report << "==================\n";
            report << "File: " << fileName << "\n";
            report << "Analysis Date: " << getCurrentTimestamp() << "\n\n";

            // Summary
            size_t totalVertices = 0, totalTriangles = 0, totalNormals = 0, totalUVs = 0;
            size_t meshesWithData = 0;
            for (const auto& mesh : meshes) {
                size_t numVertices = mesh.points.size() / 3;
                totalVertices += numVertices;
                totalTriangles += mesh.indices.size() / 3;
                totalNormals += mesh.normals.size() / 3;
                totalUVs += mesh.uvs.size() / 2;
                if (numVertices > 0) meshesWithData++;
            }

            report << "SUMMARY\n";
            report << "-------\n";
            report << "Total Meshes: " << meshes.size() << "\n";
            report << "Meshes with Data: " << meshesWithData << "\n";
            report << "Total Vertices: " << totalVertices << "\n";
            report << "Total Triangles: " << totalTriangles << "\n";
            report << "Total Normals: " << totalNormals << "\n";
            report << "Total UVs: " << totalUVs << "\n\n";

            report << "DETAILED MESH INFORMATION\n";
            report << "========================\n";
            for (size_t i = 0; i < meshes.size(); ++i) {
                const auto& mesh = meshes[i];
                report << "Mesh " << (i + 1) << ": " << mesh.elementName << "\n";
                report << "  Type: " << mesh.typeName << "\n";
                report << "  Vertices: " << (mesh.points.size() / 3) << "\n";
                report << "  Triangles: " << (mesh.indices.size() / 3) << "\n";
                report << "  Normals: " << (mesh.normals.size() / 3) << "\n";
                report << "  UVs: " << (mesh.uvs.size() / 2) << "\n\n";
            }

            report.close();

            // Add success message
            std::lock_guard<std::mutex> lock(appState.dataMutex);
            appState.receivedMessages.push_back("[" + getCurrentTimestamp() + "] Analysis report saved to: " + reportPath);
            appState.totalMessagesReceived++;
        }
    }

    if (ImGui::Button(ICON_FA_CHART_BAR " Export CSV Data", ImVec2(200, 30))) {
        // Export mesh data in CSV format
        std::string csvPath = "mesh_data_" + fileName + ".csv";
        std::ofstream csv(csvPath);
        if (csv.is_open()) {
            csv << "Mesh Name,Type,Path,Vertices,Triangles,Normals,UVs,Has Geometry\n";
            for (const auto& mesh : meshes) {
                csv << mesh.elementName << ","
                    << mesh.typeName << ","
                    << mesh.elementName << ","
                    << (mesh.points.size() / 3) << ","
                    << (mesh.indices.size() / 3) << ","
                    << (mesh.normals.size() / 3) << ","
                    << (mesh.uvs.size() / 2) << ","
                    << (mesh.points.size() > 0 ? "Yes" : "No") << "\n";
            }

            csv.close();

            // Add success message
            std::lock_guard<std::mutex> lock(appState.dataMutex);
            appState.receivedMessages.push_back("[" + getCurrentTimestamp() + "] CSV data exported to: " + csvPath);
            appState.totalMessagesReceived++;
        }
    }

    if (ImGui::Button(ICON_FA_SITEMAP " Export Hierarchy JSON", ImVec2(200, 30))) {
        // Export hierarchy as JSON
        std::string jsonPath = "hierarchy_" + fileName + ".json";
        std::ofstream json(jsonPath);
        if (json.is_open()) {
            json << "{\n";
            json << "  \"file\": \"" << fileName << "\",\n";
            json << "  \"timestamp\": \"" << getCurrentTimestamp() << "\",\n";
            json << "  \"prims\": [\n";

            for (size_t i = 0; i < meshes.size(); ++i) {
                const auto& mesh = meshes[i];
                json << "    {\n";
                json << "      \"path\": \"" << mesh.elementName << "\",\n";
                json << "      \"type\": \"" << mesh.typeName << "\",\n";
                json << "      \"vertices\": " << (mesh.points.size() / 3) << ",\n";
                json << "      \"triangles\": " << (mesh.indices.size() / 3) << ",\n";
                json << "      \"hasGeometry\": " << (mesh.points.size() > 0 ? "true" : "false") << "\n";
                json << "    }" << (i < meshes.size() - 1 ? "," : "") << "\n";
            }

            json << "  ]\n";
            json << "}\n";
            json.close();

            // Add success message
            std::lock_guard<std::mutex> lock(appState.dataMutex);
            appState.receivedMessages.push_back("[" + getCurrentTimestamp() + "] Hierarchy JSON exported to: " + jsonPath);
            appState.totalMessagesReceived++;
        }
    }
}



int main() {
    // Setup GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1200, 800, "JUSYNC - USD Analysis Tool", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Initialize GLEW
    if (glewInit() != GLEW_OK) {
        std::cerr << "Failed to initialize GLEW" << std::endl;
        glfwTerminate();
        return 1;
    }

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Enable multi-viewport

    // Load default font first
    io.Fonts->AddFontDefault();

    // Try to load Font Awesome with better error handling
    ImFontConfig config;
    config.MergeMode = true;
    config.GlyphMinAdvanceX = 13.0f;
    static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };

    // Try different possible paths for Font Awesome
    const char* fontPaths[] = {
        "../../../fonts/fa-solid-900.ttf",  // From build_linux/tools/ReceiverUI/ to project root
        "fonts/fa-solid-900.ttf",           // If running from project root
        "../fonts/fa-solid-900.ttf",
        "../../fonts/fa-solid-900.ttf",
        "/mnt/d/JSC_Gitlab/anari-usd-middleware/fonts/fa-solid-900.ttf",  // Absolute path
        "/usr/share/fonts/truetype/font-awesome/fa-solid-900.ttf",
        nullptr
    };

    bool fontLoaded = false;
    for (int i = 0; fontPaths[i] != nullptr; ++i) {
        std::cout << "Checking font path: " << fontPaths[i] << std::endl;

        if (std::filesystem::exists(fontPaths[i])) {
            std::cout << "Font file found: " << fontPaths[i] << std::endl;
            if (io.Fonts->AddFontFromFileTTF(fontPaths[i], 13.0f, &config, icon_ranges)) {
                std::cout << "Font Awesome loaded successfully from: " << fontPaths[i] << std::endl;
                fontLoaded = true;
                break;
            } else {
                std::cout << "Failed to load font from: " << fontPaths[i] << std::endl;
            }
        } else {
            std::cout << "Font file not found: " << fontPaths[i] << std::endl;
        }
    }

    if (!fontLoaded) {
        std::cout << "Font Awesome not found - icons will display as text fallbacks" << std::endl;
    }

    // Apply modern dark theme and initial accent color
    ModernDarkTheme();
    if (isDarkMode) {
        ModernDarkTheme();
    } else {
        ModernLightTheme();
    }
    ApplyAccentColor();

    // Setup platform/renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Configure ImGui style for better appearance
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    std::cout << "JUSYNC USD Analysis Tool started successfully!" << std::endl;
    std::cout << "Font Awesome status: " << (fontLoaded ? "Loaded" : "Text fallbacks") << std::endl;

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        // Poll and handle events
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Create main dockspace
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
        window_flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        ImGui::Begin("DockSpace", nullptr, window_flags);
        ImGui::PopStyleVar(3);

        // Menu bar
        drawMenuBar();

        // Dockspace
        ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

        ImGui::End();

        // Draw application panels
        drawConnectionPanel();
        drawMetricsPanel();
        drawFileListPanel();
        drawMessagesPanel();

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Update and render additional platform windows
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }

        glfwSwapBuffers(window);
    }

    // Cleanup
    std::cout << "Shutting down JUSYNC..." << std::endl;

    // Clean up middleware
    if (appState.isConnected) {
        appState.middleware.stopReceiving();
        appState.middleware.shutdown();
    }

    // Clean up analysis thread if running
    if (appState.analysisFuture.valid()) {
        appState.cancelAnalysis = true;
        appState.analysisFuture.wait();
    }

    // Cleanup ImGui
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    // Cleanup GLFW
    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "JUSYNC shutdown complete." << std::endl;
    return 0;
}