#include "CollisionDebugWindow.h"
#include <imgui.h>
#include <iostream>
#include <iomanip>
#include <sstream>

CollisionDebugWindow::CollisionDebugWindow() {
    // Initialize with default settings
    showCollisionMeshes = true;
    showBoundingBoxes = true;
    showSpheres = true;
    showConvexHulls = true;
    selectedMeshIndex = -1;
    colorizeByComplexity = true;
}

void CollisionDebugWindow::render(const std::vector<CMeshData>& meshes) {
    if (!isVisible) return;

    ImGui::Begin("🔍 Collision Debug Viewer", &isVisible, ImGuiWindowFlags_AlwaysAutoResize);

    renderOverviewStats(meshes);
    ImGui::Separator();

    renderDisplayControls();
    ImGui::Separator();

    renderMeshList(meshes);
    ImGui::Separator();

    if (selectedMeshIndex >= 0 && selectedMeshIndex < static_cast<int>(meshes.size())) {
        renderSelectedMeshDetails(meshes[selectedMeshIndex]);
    }

    ImGui::End();
}

void CollisionDebugWindow::renderOverviewStats(const std::vector<CMeshData>& meshes) {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "📊 Collision Overview");

    // Count collision types
    std::map<int, int> collisionCounts;
    int totalCollisionVertices = 0;
    int totalCollisionTriangles = 0;

    for (const auto& mesh : meshes) {
        collisionCounts[mesh.collision_type]++;
        totalCollisionVertices += mesh.collision_vertices_count / 3;
        totalCollisionTriangles += mesh.collision_indices_count / 3;
    }

    ImGui::Text("📁 Total Meshes: %zu", meshes.size());
    ImGui::Text("🔺 Total Collision Vertices: %d", totalCollisionVertices);
    ImGui::Text("📐 Total Collision Triangles: %d", totalCollisionTriangles);

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Collision Type Distribution:");

    for (const auto& [type, count] : collisionCounts) {
        ImGui::Text("  %s: %d meshes", getCollisionTypeName(type).c_str(), count);
    }
}

void CollisionDebugWindow::renderDisplayControls() {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "🎨 Display Controls");

    ImGui::Checkbox("Show Collision Meshes", &showCollisionMeshes);
    ImGui::SameLine();
    ImGui::Checkbox("Show Bounding Boxes", &showBoundingBoxes);

    ImGui::Checkbox("Show Spheres", &showSpheres);
    ImGui::SameLine();
    ImGui::Checkbox("Show Convex Hulls", &showConvexHulls);

    ImGui::Checkbox("Colorize by Complexity", &colorizeByComplexity);

    if (ImGui::Button("🔄 Reset View")) {
        selectedMeshIndex = -1;
    }
}

void CollisionDebugWindow::renderMeshList(const std::vector<CMeshData>& meshes) {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "📋 Mesh List");

    ImGui::BeginChild("MeshScrolling", ImVec2(0, 200), true);

    for (size_t i = 0; i < meshes.size(); ++i) {
        const auto& mesh = meshes[i];

        // Color code based on collision complexity
        ImVec4 textColor = getCollisionTypeColor(mesh.collision_type);

        bool isSelected = (selectedMeshIndex == static_cast<int>(i));
        if (ImGui::Selectable(("##mesh_" + std::to_string(i)).c_str(), isSelected)) {
            selectedMeshIndex = static_cast<int>(i);
        }

        ImGui::SameLine();
        ImGui::TextColored(textColor, "%zu: %s", i, mesh.element_name);

        ImGui::SameLine(250);
        ImGui::TextColored(textColor, "[%s]", getCollisionTypeName(mesh.collision_type).c_str());

        // Show basic stats
        ImGui::SameLine(350);
        ImGui::Text("V:%zu T:%zu",
                   mesh.collision_vertices_count / 3,
                   mesh.collision_indices_count / 3);
    }

    ImGui::EndChild();
}

void CollisionDebugWindow::renderSelectedMeshDetails(const CMeshData& mesh) {
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "🔍 Selected Mesh Details");

    ImGui::Text("📛 Name: %s", mesh.element_name);
    ImGui::Text("🏷️  Type: %s", mesh.type_name);

    ImGui::Spacing();

    // Visual mesh info
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "👁️  Visual Mesh:");
    ImGui::Text("   Vertices: %zu", mesh.points_count / 3);
    ImGui::Text("   Triangles: %zu", mesh.indices_count / 3);
    ImGui::Text("   Normals: %zu", mesh.normals_count / 3);
    ImGui::Text("   UVs: %zu", mesh.uvs_count / 2);
    ImGui::Text("   Colors: %zu", mesh.vertex_colors_count / 4);

    ImGui::Spacing();

    // Collision info
    ImVec4 collisionColor = getCollisionTypeColor(mesh.collision_type);
    ImGui::TextColored(collisionColor, "💥 Collision Data:");
    ImGui::Text("   Type: %s (%d)", getCollisionTypeName(mesh.collision_type).c_str(), mesh.collision_type);

    if (mesh.collision_type != 0) { // Not COLLISION_NONE
        ImGui::Text("   Collision Vertices: %zu", mesh.collision_vertices_count / 3);
        ImGui::Text("   Collision Triangles: %zu", mesh.collision_indices_count / 3);

        // Bounding box info
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "📦 Bounding Box:");
        ImGui::Text("   Min: (%.3f, %.3f, %.3f)",
                   mesh.bounding_box_min[0], mesh.bounding_box_min[1], mesh.bounding_box_min[2]);
        ImGui::Text("   Max: (%.3f, %.3f, %.3f)",
                   mesh.bounding_box_max[0], mesh.bounding_box_max[1], mesh.bounding_box_max[2]);

        // Calculate bounding box dimensions
        float width = mesh.bounding_box_max[0] - mesh.bounding_box_min[0];
        float height = mesh.bounding_box_max[1] - mesh.bounding_box_min[1];
        float depth = mesh.bounding_box_max[2] - mesh.bounding_box_min[2];
        ImGui::Text("   Dimensions: %.3f × %.3f × %.3f", width, height, depth);

        // Bounding sphere info
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 1.0f, 1.0f), "🔮 Bounding Sphere:");
        ImGui::Text("   Center: (%.3f, %.3f, %.3f)",
                   mesh.sphere_center[0], mesh.sphere_center[1], mesh.sphere_center[2]);
        ImGui::Text("   Radius: %.3f", mesh.sphere_radius);

        // Complexity analysis
        ImGui::Spacing();
        renderComplexityAnalysis(mesh);

        // Memory usage estimation
        ImGui::Spacing();
        renderMemoryUsage(mesh);
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "   No collision data generated");
    }
}

void CollisionDebugWindow::renderComplexityAnalysis(const CMeshData& mesh) {
    ImGui::TextColored(ImVec4(1.0f, 0.0f, 1.0f, 1.0f), "⚡ Complexity Analysis:");

    if (mesh.points_count > 0 && mesh.collision_vertices_count > 0) {
        float reductionRatio = static_cast<float>(mesh.collision_vertices_count) /
                              static_cast<float>(mesh.points_count);
        ImGui::Text("   Vertex Reduction: %.1f%% of original", reductionRatio * 100.0f);
    }

    if (mesh.indices_count > 0 && mesh.collision_indices_count > 0) {
        float triangleReduction = static_cast<float>(mesh.collision_indices_count) /
                                 static_cast<float>(mesh.indices_count);
        ImGui::Text("   Triangle Reduction: %.1f%% of original", triangleReduction * 100.0f);
    }

    // Performance estimate
    std::string performanceLevel = getPerformanceEstimate(mesh.collision_type, mesh.collision_indices_count / 3);
    ImGui::Text("   Performance: %s", performanceLevel.c_str());
}

void CollisionDebugWindow::renderMemoryUsage(const CMeshData& mesh) {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "💾 Memory Usage:");

    size_t visualMemory = (mesh.points_count + mesh.normals_count + mesh.uvs_count) * sizeof(float) +
                          mesh.vertex_colors_count * sizeof(float) +
                          mesh.indices_count * sizeof(unsigned int);

    size_t collisionMemory = mesh.collision_vertices_count * sizeof(float) +
                            mesh.collision_indices_count * sizeof(unsigned int);

    size_t totalMemory = visualMemory + collisionMemory;

    ImGui::Text("   Visual Mesh: %s", formatBytes(visualMemory).c_str());
    ImGui::Text("   Collision Mesh: %s", formatBytes(collisionMemory).c_str());
    ImGui::Text("   Total: %s", formatBytes(totalMemory).c_str());
}

std::string CollisionDebugWindow::getCollisionTypeName(int type) {
    switch (type) {
        case 0: return "None";
        case 1: return "Simple (Box)";
        case 2: return "Convex Hull";
        case 3: return "Complex";
        case 4: return "Simplified";
        case 5: return "Convex Decomp";
        default: return "Unknown";
    }
}

ImVec4 CollisionDebugWindow::getCollisionTypeColor(int type) {
    switch (type) {
        case 0: return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);    // Gray - None
        case 1: return ImVec4(0.0f, 1.0f, 0.0f, 1.0f);    // Green - Simple
        case 2: return ImVec4(0.0f, 0.5f, 1.0f, 1.0f);    // Blue - Convex Hull
        case 3: return ImVec4(1.0f, 0.0f, 0.0f, 1.0f);    // Red - Complex
        case 4: return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);    // Yellow - Simplified
        case 5: return ImVec4(1.0f, 0.0f, 1.0f, 1.0f);    // Magenta - Convex Decomp
        default: return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);   // White - Unknown
    }
}

std::string CollisionDebugWindow::getPerformanceEstimate(int collisionType, size_t triangleCount) {
    switch (collisionType) {
        case 0: return "⭐⭐⭐⭐⭐ Excellent (No collision)";
        case 1: return "⭐⭐⭐⭐⭐ Excellent (Primitive)";
        case 2:
            if (triangleCount < 100) return "⭐⭐⭐⭐ Very Good";
            else return "⭐⭐⭐ Good";
        case 3:
            if (triangleCount < 1000) return "⭐⭐⭐ Good";
            else if (triangleCount < 10000) return "⭐⭐ Fair";
            else return "⭐ Poor";
        case 4:
            if (triangleCount < 500) return "⭐⭐⭐⭐ Very Good";
            else return "⭐⭐⭐ Good";
        case 5: return "⭐⭐ Fair (Complex decomp)";
        default: return "❓ Unknown";
    }
}

std::string CollisionDebugWindow::formatBytes(size_t bytes) {
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

void CollisionDebugWindow::setVisible(bool visible) {
    isVisible = visible;
}

bool CollisionDebugWindow::getVisible() const {
    return isVisible;
}
