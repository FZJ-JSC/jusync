#pragma once

#include <vector>
#include <map>
#include <string>
#include <imgui.h>
#include "AnariUsdMiddleware_C.h" // For CMeshData

class CollisionDebugWindow {
public:
    CollisionDebugWindow();
    ~CollisionDebugWindow() = default;

    // Main render function
    void render(const std::vector<CMeshData>& meshes);

    // Visibility control
    void setVisible(bool visible);
    bool getVisible() const;

private:
    bool isVisible = true;

    // Display options
    bool showCollisionMeshes;
    bool showBoundingBoxes;
    bool showSpheres;
    bool showConvexHulls;
    bool colorizeByComplexity;

    // Selection state
    int selectedMeshIndex;

    // Rendering functions
    void renderOverviewStats(const std::vector<CMeshData>& meshes);
    void renderDisplayControls();
    void renderMeshList(const std::vector<CMeshData>& meshes);
    void renderSelectedMeshDetails(const CMeshData& mesh);
    void renderComplexityAnalysis(const CMeshData& mesh);
    void renderMemoryUsage(const CMeshData& mesh);

    // Helper functions
    std::string getCollisionTypeName(int type);
    ImVec4 getCollisionTypeColor(int type);
    std::string getPerformanceEstimate(int collisionType, size_t triangleCount);
    std::string formatBytes(size_t bytes);
};
