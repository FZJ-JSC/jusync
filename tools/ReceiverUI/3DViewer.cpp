#include "3DViewer.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

// ============================================================================
// SHADER SOURCES
// ============================================================================

namespace Shaders {
    const char* vertexShaderSource = R"(
        #version 330 core
        layout (location = 0) in vec3 aPos;
        layout (location = 1) in vec3 aNormal;
        layout (location = 2) in vec4 aColor;

        out vec3 FragPos;
        out vec3 Normal;
        out vec4 VertexColor;

        uniform mat4 model;
        uniform mat4 view;
        uniform mat4 projection;

        void main() {
            FragPos = vec3(model * vec4(aPos, 1.0));
            Normal = mat3(transpose(inverse(model))) * aNormal;
            VertexColor = aColor;

            gl_Position = projection * view * vec4(FragPos, 1.0);
        }
    )";

    const char* fragmentShaderSource = R"(
        #version 330 core
        out vec4 FragColor;

        in vec3 FragPos;
        in vec3 Normal;
        in vec4 VertexColor;

        uniform vec3 lightPos;
        uniform vec3 viewPos;
        uniform vec3 objectColor;
        uniform float alpha;

        void main() {
            // Ambient lighting
            float ambientStrength = 0.2;
            vec3 ambient = ambientStrength * vec3(1.0);

            // Diffuse lighting
            vec3 norm = normalize(Normal);
            vec3 lightDir = normalize(lightPos - FragPos);
            float diff = max(dot(norm, lightDir), 0.0);
            vec3 diffuse = diff * vec3(0.8);

            // Specular lighting
            float specularStrength = 0.3;
            vec3 viewDir = normalize(viewPos - FragPos);
            vec3 reflectDir = reflect(-lightDir, norm);
            float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32);
            vec3 specular = specularStrength * spec * vec3(1.0);

            vec3 result = (ambient + diffuse + specular) * objectColor;

            // Blend with vertex colors if available
            if (VertexColor.a > 0.0) {
                result = mix(result, VertexColor.rgb, 0.5);
            }

            FragColor = vec4(result, alpha);
        }
    )";

    const char* wireframeVertexSource = R"(
        #version 330 core
        layout (location = 0) in vec3 aPos;

        uniform mat4 model;
        uniform mat4 view;
        uniform mat4 projection;

        void main() {
            gl_Position = projection * view * model * vec4(aPos, 1.0);
        }
    )";

    const char* wireframeFragmentSource = R"(
        #version 330 core
        out vec4 FragColor;

        uniform vec3 objectColor;
        uniform float alpha;

        void main() {
            FragColor = vec4(objectColor, alpha);
        }
    )";
}

// ============================================================================
// CAMERA3D IMPLEMENTATION
// ============================================================================

Camera3D::Camera3D() : target(0.0f, 0.0f, 0.0f), distance(5.0f), yaw(0.0f), pitch(0.0f),
                       fov(45.0f), nearPlane(0.1f), farPlane(1000.0f) {
    updatePosition();
}

void Camera3D::processMouseInput(float deltaX, float deltaY, bool leftButton, bool rightButton, bool middleButton) {
    const float sensitivity = 0.005f;
    const float panSensitivity = 0.003f;

    if (leftButton) {
        // Orbit around target
        yaw -= deltaX * sensitivity;
        pitch += deltaY * sensitivity;

        // Clamp pitch to avoid flipping
        pitch = std::clamp(pitch, -1.5f, 1.5f);

        updatePosition();
    } else if (rightButton || middleButton) {
        // Pan the target
        glm::vec3 right = glm::normalize(glm::cross(position - target, glm::vec3(0.0f, 1.0f, 0.0f)));
        glm::vec3 up = glm::normalize(glm::cross(right, position - target));

        target += right * deltaX * panSensitivity * distance;
        target -= up * deltaY * panSensitivity * distance;

        updatePosition();
    }
}

void Camera3D::processScrollInput(float scrollDelta) {
    const float zoomSensitivity = 0.1f;
    distance *= (1.0f - scrollDelta * zoomSensitivity);
    distance = std::clamp(distance, 0.1f, 100.0f);
    updatePosition();
}

void Camera3D::reset() {
    target = glm::vec3(0.0f, 0.0f, 0.0f);
    distance = 5.0f;
    yaw = 0.0f;
    pitch = 0.0f;
    updatePosition();
}

void Camera3D::focusOnBounds(const glm::vec3& minBounds, const glm::vec3& maxBounds) {
    target = (minBounds + maxBounds) * 0.5f;
    glm::vec3 size = maxBounds - minBounds;
    float maxDim = std::max({size.x, size.y, size.z});
    distance = maxDim * 2.5f;
    if (distance < 1.0f) distance = 5.0f;
    updatePosition();
}

glm::mat4 Camera3D::getViewMatrix() const {
    return glm::lookAt(position, target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera3D::getProjectionMatrix(float aspectRatio) const {
    return glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);
}

void Camera3D::updatePosition() {
    position.x = target.x + distance * cos(pitch) * cos(yaw);
    position.y = target.y + distance * sin(pitch);
    position.z = target.z + distance * cos(pitch) * sin(yaw);
}

// ============================================================================
// MESHRENDERER IMPLEMENTATION
// ============================================================================

MeshRenderer::MeshRenderer() : shaderProgram(0), wireframeShaderProgram(0),
                               wireframeMode(false), showNormals(false),
                               showCollisionMesh(true), showVisualMesh(true),
                               collisionAlpha(0.5f) {
}

MeshRenderer::~MeshRenderer() {
    cleanup();
}

bool MeshRenderer::initialize() {
    if (!createShaders()) {
        std::cerr << "Failed to create shaders" << std::endl;
        return false;
    }

    // Enable OpenGL features
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    return true;
}

void MeshRenderer::cleanup() {
    clearMeshes();

    if (shaderProgram) {
        glDeleteProgram(shaderProgram);
        shaderProgram = 0;
    }
    if (wireframeShaderProgram) {
        glDeleteProgram(wireframeShaderProgram);
        wireframeShaderProgram = 0;
    }
}

void MeshRenderer::loadMesh(const CMeshData& meshData, bool isCollisionMesh) {
    if (meshData.points_count == 0) {
        return; // Skip empty meshes
    }

    MeshObject mesh;
    mesh.name = meshData.element_name;
    mesh.isCollision = isCollisionMesh;
    mesh.collisionType = meshData.collision_type;

    // Set color based on mesh type
    if (isCollisionMesh) {
        mesh.color = getCollisionTypeColor(meshData.collision_type);
    } else {
        mesh.color = glm::vec3(0.7f, 0.7f, 0.9f); // Light blue for visual mesh
    }

    // Store collision metadata
    if (isCollisionMesh) {
        mesh.boundingBoxMin = glm::vec3(meshData.bounding_box_min[0], meshData.bounding_box_min[1], meshData.bounding_box_min[2]);
        mesh.boundingBoxMax = glm::vec3(meshData.bounding_box_max[0], meshData.bounding_box_max[1], meshData.bounding_box_max[2]);
        mesh.sphereCenter = glm::vec3(meshData.sphere_center[0], meshData.sphere_center[1], meshData.sphere_center[2]);
        mesh.sphereRadius = meshData.sphere_radius;
    }

    createMeshBuffers(mesh, meshData, isCollisionMesh);
    meshes.push_back(mesh);
}

void MeshRenderer::clearMeshes() {
    for (auto& mesh : meshes) {
        glDeleteVertexArrays(1, &mesh.VAO);
        glDeleteBuffers(1, &mesh.VBO);
        glDeleteBuffers(1, &mesh.EBO);
        if (mesh.normalVBO) glDeleteBuffers(1, &mesh.normalVBO);
        if (mesh.colorVBO) glDeleteBuffers(1, &mesh.colorVBO);
    }
    meshes.clear();
}

void MeshRenderer::render(const glm::mat4& viewMatrix, const glm::mat4& projMatrix,
                         const glm::vec3& lightPos, const glm::vec3& cameraPos) {
    glm::mat4 model = glm::mat4(1.0f);

    // Use appropriate shader
    GLuint currentShader = wireframeMode ? wireframeShaderProgram : shaderProgram;
    glUseProgram(currentShader);

    // Set uniforms
    glUniformMatrix4fv(glGetUniformLocation(currentShader, "model"), 1, GL_FALSE, glm::value_ptr(model));
    glUniformMatrix4fv(glGetUniformLocation(currentShader, "view"), 1, GL_FALSE, glm::value_ptr(viewMatrix));
    glUniformMatrix4fv(glGetUniformLocation(currentShader, "projection"), 1, GL_FALSE, glm::value_ptr(projMatrix));

    if (!wireframeMode) {
        glUniform3fv(glGetUniformLocation(currentShader, "lightPos"), 1, glm::value_ptr(lightPos));
        glUniform3fv(glGetUniformLocation(currentShader, "viewPos"), 1, glm::value_ptr(cameraPos));
    }

    // Render visual meshes first (opaque)
    for (const auto& mesh : meshes) {
        if (mesh.isCollision) continue;
        if (!showVisualMesh) continue;

        renderSingleMesh(mesh, currentShader, 1.0f);
    }

    // Render collision meshes second (transparent)
    if (showCollisionMesh) {
        glEnable(GL_BLEND);
        for (const auto& mesh : meshes) {
            if (!mesh.isCollision) continue;

            renderSingleMesh(mesh, currentShader, collisionAlpha);
        }
        glDisable(GL_BLEND);
    }
}

void MeshRenderer::renderSingleMesh(const MeshObject& mesh, GLuint shader, float alpha) {
    // Set mesh-specific uniforms
    glUniform3fv(glGetUniformLocation(shader, "objectColor"), 1, glm::value_ptr(mesh.color));
    glUniform1f(glGetUniformLocation(shader, "alpha"), alpha);

    // Set polygon mode
    if (wireframeMode) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(mesh.isCollision ? 2.0f : 1.0f);
    } else {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    // Bind and render
    glBindVertexArray(mesh.VAO);
    if (mesh.indexCount > 0) {
        glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, 0);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, mesh.vertexCount);
    }
    glBindVertexArray(0);

    // Reset line width
    if (wireframeMode) {
        glLineWidth(1.0f);
    }
}

size_t MeshRenderer::getVisualMeshCount() const {
    return std::count_if(meshes.begin(), meshes.end(),
                        [](const MeshObject& mesh) { return !mesh.isCollision; });
}

size_t MeshRenderer::getCollisionMeshCount() const {
    return std::count_if(meshes.begin(), meshes.end(),
                        [](const MeshObject& mesh) { return mesh.isCollision; });
}

bool MeshRenderer::calculateBounds(glm::vec3& minBounds, glm::vec3& maxBounds) const {
    if (meshes.empty()) return false;

    bool first = true;
    for (const auto& mesh : meshes) {
        if (mesh.isCollision && mesh.boundingBoxMin != glm::vec3(0.0f)) {
            if (first) {
                minBounds = mesh.boundingBoxMin;
                maxBounds = mesh.boundingBoxMax;
                first = false;
            } else {
                minBounds = glm::min(minBounds, mesh.boundingBoxMin);
                maxBounds = glm::max(maxBounds, mesh.boundingBoxMax);
            }
        }
    }

    if (first) {
        // Fallback: use default bounds
        minBounds = glm::vec3(-1.0f);
        maxBounds = glm::vec3(1.0f);
    }

    return true;
}

void MeshRenderer::createMeshBuffers(MeshObject& mesh, const CMeshData& meshData, bool isCollisionMesh) {
    // Generate buffers
    glGenVertexArrays(1, &mesh.VAO);
    glGenBuffers(1, &mesh.VBO);
    glGenBuffers(1, &mesh.EBO);

    glBindVertexArray(mesh.VAO);

    // Determine which data to use
    const float* vertices;
    const unsigned int* indices;
    size_t vertexCount, indexCount;

    if (isCollisionMesh && meshData.collision_vertices_count > 0) {
        vertices = meshData.collision_vertices;
        indices = meshData.collision_indices;
        vertexCount = meshData.collision_vertices_count / 3;
        indexCount = meshData.collision_indices_count;
    } else {
        vertices = meshData.points;
        indices = meshData.indices;
        vertexCount = meshData.points_count / 3;
        indexCount = meshData.indices_count;
    }

    mesh.vertexCount = vertexCount;
    mesh.indexCount = indexCount;

    // Create vertex data with position, normal, and color
    std::vector<float> vertexData;
    vertexData.reserve(vertexCount * 10); // pos(3) + normal(3) + color(4)

    for (size_t i = 0; i < vertexCount; ++i) {
        // Position
        vertexData.push_back(vertices[i * 3 + 0]);
        vertexData.push_back(vertices[i * 3 + 1]);
        vertexData.push_back(vertices[i * 3 + 2]);

        // Normal (use mesh normals if available, otherwise default up)
        if (!isCollisionMesh && meshData.normals_count > i * 3 + 2) {
            vertexData.push_back(meshData.normals[i * 3 + 0]);
            vertexData.push_back(meshData.normals[i * 3 + 1]);
            vertexData.push_back(meshData.normals[i * 3 + 2]);
        } else {
            vertexData.push_back(0.0f);
            vertexData.push_back(1.0f);
            vertexData.push_back(0.0f);
        }

        // Color (use vertex colors if available, otherwise default)
        if (!isCollisionMesh && meshData.vertex_colors_count > i * 4 + 3) {
            vertexData.push_back(meshData.vertex_colors[i * 4 + 0]);
            vertexData.push_back(meshData.vertex_colors[i * 4 + 1]);
            vertexData.push_back(meshData.vertex_colors[i * 4 + 2]);
            vertexData.push_back(meshData.vertex_colors[i * 4 + 3]);
        } else {
            vertexData.push_back(1.0f);
            vertexData.push_back(1.0f);
            vertexData.push_back(1.0f);
            vertexData.push_back(1.0f);
        }
    }

    // Upload vertex data
    glBindBuffer(GL_ARRAY_BUFFER, mesh.VBO);
    glBufferData(GL_ARRAY_BUFFER, vertexData.size() * sizeof(float), vertexData.data(), GL_STATIC_DRAW);

    // Upload index data
    if (indexCount > 0) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexCount * sizeof(unsigned int), indices, GL_STATIC_DRAW);
    }

    // Set vertex attributes
    // Position attribute
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Normal attribute
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Color attribute
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
}

glm::vec3 MeshRenderer::getCollisionTypeColor(int collisionType) const {
    switch (collisionType) {
        case 0: return glm::vec3(0.5f, 0.5f, 0.5f);    // Gray - None
        case 1: return glm::vec3(0.0f, 1.0f, 0.0f);    // Green - Simple
        case 2: return glm::vec3(0.0f, 0.5f, 1.0f);    // Blue - Convex Hull
        case 3: return glm::vec3(1.0f, 0.0f, 0.0f);    // Red - Complex
        case 4: return glm::vec3(1.0f, 1.0f, 0.0f);    // Yellow - Simplified
        case 5: return glm::vec3(1.0f, 0.0f, 1.0f);    // Magenta - Convex Decomp
        default: return glm::vec3(1.0f, 1.0f, 1.0f);   // White - Unknown
    }
}

bool MeshRenderer::createShaders() {
    // Create main shader program
    GLuint vertexShader = compileShader(Shaders::vertexShaderSource, GL_VERTEX_SHADER);
    GLuint fragmentShader = compileShader(Shaders::fragmentShaderSource, GL_FRAGMENT_SHADER);

    if (vertexShader == 0 || fragmentShader == 0) {
        return false;
    }

    shaderProgram = linkProgram(vertexShader, fragmentShader);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    if (shaderProgram == 0) {
        return false;
    }

    // Create wireframe shader program
    GLuint wireVertexShader = compileShader(Shaders::wireframeVertexSource, GL_VERTEX_SHADER);
    GLuint wireFragmentShader = compileShader(Shaders::wireframeFragmentSource, GL_FRAGMENT_SHADER);

    if (wireVertexShader == 0 || wireFragmentShader == 0) {
        glDeleteShader(wireVertexShader);
        glDeleteShader(wireFragmentShader);
        return false;
    }

    wireframeShaderProgram = linkProgram(wireVertexShader, wireFragmentShader);
    glDeleteShader(wireVertexShader);
    glDeleteShader(wireFragmentShader);

    return wireframeShaderProgram != 0;
}

GLuint MeshRenderer::compileShader(const char* source, GLenum type) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        std::cerr << "Shader compilation failed: " << infoLog << std::endl;
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

GLuint MeshRenderer::linkProgram(GLuint vertexShader, GLuint fragmentShader) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, NULL, infoLog);
        std::cerr << "Program linking failed: " << infoLog << std::endl;
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

// ============================================================================
// VIEWER3D IMPLEMENTATION
// ============================================================================

Viewer3D::Viewer3D() : isVisible(true), framebuffer(0), colorTexture(0), depthTexture(0),
                       viewportWidth(800), viewportHeight(600), wireframeMode(false),
                       showNormals(false), showCollisionMesh(true), showVisualMesh(true),
                       collisionAlpha(0.5f), selectedMeshIndex(-1),
                       lightPosition(10.0f, 10.0f, 10.0f), isDragging(false) {
    camera = std::make_unique<Camera3D>();
    renderer = std::make_unique<MeshRenderer>();
}

Viewer3D::~Viewer3D() {
    cleanup();
}

bool Viewer3D::initialize() {
    if (!renderer->initialize()) {
        return false;
    }

    return createFramebuffer(viewportWidth, viewportHeight);
}

void Viewer3D::cleanup() {
    deleteFramebuffer();
    if (renderer) {
        renderer->cleanup();
    }
}

void Viewer3D::render(const std::vector<CMeshData>& meshes) {
    if (!isVisible) return;

    ImGui::Begin("🎮 3D Model Viewer", &isVisible, ImGuiWindowFlags_MenuBar);

    // Menu bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset Camera", "R")) {
                camera->reset();
            }
            if (ImGui::MenuItem("Focus All", "F")) {
                focusOnAllMeshes();
            }
            ImGui::Separator();
            ImGui::MenuItem("Wireframe", "W", &wireframeMode);
            ImGui::MenuItem("Show Visual", "1", &showVisualMesh);
            ImGui::MenuItem("Show Collision", "2", &showCollisionMesh);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Clear Meshes")) {
                renderer->clearMeshes();
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    // Load meshes if changed
    static size_t lastMeshCount = 0;
    if (meshes.size() != lastMeshCount) {
        loadMeshesIntoRenderer(meshes);
        lastMeshCount = meshes.size();
    }

    renderControls();
    ImGui::Separator();

    renderViewport();
    ImGui::Separator();

    renderMeshList(meshes);

    if (selectedMeshIndex >= 0 && selectedMeshIndex < static_cast<int>(meshes.size())) {
        ImGui::Separator();
        renderSelectedMeshInfo(meshes);
    }

    ImGui::End();
}

void Viewer3D::renderControls() {
    ImGui::Text("🎛️ Controls");

    if (ImGui::Button("🏠 Reset Camera")) {
        camera->reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("🎯 Focus All")) {
        focusOnAllMeshes();
    }

    ImGui::Checkbox("📐 Wireframe", &wireframeMode);
    ImGui::SameLine();
    ImGui::Checkbox("👁️ Visual Mesh", &showVisualMesh);
    ImGui::SameLine();
    ImGui::Checkbox("💥 Collision Mesh", &showCollisionMesh);

    if (showCollisionMesh) {
        ImGui::SliderFloat("Collision Alpha", &collisionAlpha, 0.1f, 1.0f, "%.2f");
    }

    // Light position control
    ImGui::Text("💡 Light Position:");
    ImGui::SliderFloat3("##LightPos", glm::value_ptr(lightPosition), -20.0f, 20.0f);

    // Update renderer settings
    renderer->setWireframeMode(wireframeMode);
    renderer->setShowCollision(showCollisionMesh);
    renderer->setShowVisual(showVisualMesh);
    renderer->setCollisionAlpha(collisionAlpha);
}

void Viewer3D::renderViewport() {
    ImVec2 vMin = ImGui::GetWindowContentRegionMin();
    ImVec2 vMax = ImGui::GetWindowContentRegionMax();

    vMin.x += ImGui::GetWindowPos().x;
    vMin.y += ImGui::GetWindowPos().y;
    vMax.x += ImGui::GetWindowPos().x;
    vMax.y += ImGui::GetWindowPos().y;

    ImVec2 viewportSize = ImVec2(vMax.x - vMin.x, vMax.y - vMin.y);

    // Ensure minimum size
    if (viewportSize.x < 200) viewportSize.x = 200;
    if (viewportSize.y < 200) viewportSize.y = 200;

    // Recreate framebuffer if size changed
    if (static_cast<int>(viewportSize.x) != viewportWidth ||
        static_cast<int>(viewportSize.y) != viewportHeight) {
        setViewportSize(static_cast<int>(viewportSize.x), static_cast<int>(viewportSize.y));
    }

    // Render to framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, viewportWidth, viewportHeight);

    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Render scene
    if (renderer->getMeshCount() > 0) {
        float aspectRatio = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
        glm::mat4 view = camera->getViewMatrix();
        glm::mat4 proj = camera->getProjectionMatrix(aspectRatio);

        renderer->render(view, proj, lightPosition, camera->getPosition());
    }

    // Restore default framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Display the rendered texture
    ImGui::Image(reinterpret_cast<void*>(colorTexture), viewportSize, ImVec2(0, 1), ImVec2(1, 0));

    // Handle mouse input
    handleMouseInput(viewportSize);

    // Display stats
    ImGui::Text("📊 Meshes: %zu (Visual: %zu, Collision: %zu)",
               renderer->getMeshCount(),
               renderer->getVisualMeshCount(),
               renderer->getCollisionMeshCount());
}

void Viewer3D::renderMeshList(const std::vector<CMeshData>& meshes) {
    ImGui::Text("📋 Mesh List");

    ImGui::BeginChild("MeshList", ImVec2(0, 150), true);

    for (size_t i = 0; i < meshes.size(); ++i) {
        const auto& mesh = meshes[i];

        bool isSelected = (selectedMeshIndex == static_cast<int>(i));
        if (ImGui::Selectable(("##mesh_" + std::to_string(i)).c_str(), isSelected)) {
            selectedMeshIndex = static_cast<int>(i);
        }

        ImGui::SameLine();
        ImGui::Text("%zu: %s", i, mesh.element_name);

        ImGui::SameLine(200);
        ImGui::Text("V:%zu T:%zu",
                   mesh.points_count / 3,
                   mesh.indices_count / 3);

        if (mesh.collision_vertices_count > 0) {
            ImGui::SameLine(300);
            ImGui::Text("CV:%zu CT:%zu",
                       mesh.collision_vertices_count / 3,
                       mesh.collision_indices_count / 3);
        }
    }

    ImGui::EndChild();
}

void Viewer3D::renderSelectedMeshInfo(const std::vector<CMeshData>& meshes) {
    const auto& mesh = meshes[selectedMeshIndex];

    ImGui::Text("🔍 Selected: %s", mesh.element_name);
    ImGui::Text("Visual: %zu vertices, %zu triangles",
                mesh.points_count / 3, mesh.indices_count / 3);

    if (mesh.collision_vertices_count > 0) {
        ImGui::Text("Collision: %zu vertices, %zu triangles (%s)",
                    mesh.collision_vertices_count / 3,
                    mesh.collision_indices_count / 3,
                    getCollisionTypeName(mesh.collision_type).c_str());
    }
}

void Viewer3D::handleMouseInput(const ImVec2& viewportSize) {
    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();

        // Handle mouse dragging
        if (io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2]) {
            if (!isDragging) {
                isDragging = true;
                lastMousePos = io.MousePos;
            } else {
                ImVec2 mouseDelta = ImVec2(io.MousePos.x - lastMousePos.x, io.MousePos.y - lastMousePos.y);
                camera->processMouseInput(mouseDelta.x, mouseDelta.y,
                                        io.MouseDown[0], io.MouseDown[1], io.MouseDown[2]);
                lastMousePos = io.MousePos;
            }
        } else {
            isDragging = false;
        }

        // Handle scroll wheel
        if (io.MouseWheel != 0.0f) {
            camera->processScrollInput(io.MouseWheel);
        }
    } else {
        isDragging = false;
    }
}

void Viewer3D::loadMeshesIntoRenderer(const std::vector<CMeshData>& meshes) {
    renderer->clearMeshes();

    for (const auto& mesh : meshes) {
        // Load visual mesh
        renderer->loadMesh(mesh, false);

        // Load collision mesh if available
        if (mesh.collision_vertices_count > 0) {
            renderer->loadMesh(mesh, true);
        }
    }
}

void Viewer3D::focusOnAllMeshes() {
    glm::vec3 minBounds, maxBounds;
    if (renderer->calculateBounds(minBounds, maxBounds)) {
        camera->focusOnBounds(minBounds, maxBounds);
    }
}

void Viewer3D::setViewportSize(int width, int height) {
    viewportWidth = width;
    viewportHeight = height;

    deleteFramebuffer();
    createFramebuffer(width, height);
}

bool Viewer3D::createFramebuffer(int width, int height) {
    // Create framebuffer
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    // Create color texture
    glGenTextures(1, &colorTexture);
    glBindTexture(GL_TEXTURE_2D, colorTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture, 0);

    // Create depth texture
    glGenTextures(1, &depthTexture);
    glBindTexture(GL_TEXTURE_2D, depthTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTexture, 0);

    // Check framebuffer completeness
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Framebuffer not complete!" << std::endl;
        deleteFramebuffer();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void Viewer3D::deleteFramebuffer() {
    if (framebuffer) {
        glDeleteFramebuffers(1, &framebuffer);
        framebuffer = 0;
    }
    if (colorTexture) {
        glDeleteTextures(1, &colorTexture);
        colorTexture = 0;
    }
    if (depthTexture) {
        glDeleteTextures(1, &depthTexture);
        depthTexture = 0;
    }
}

std::string Viewer3D::getCollisionTypeName(int type) const {
    switch (type) {
        case 0: return "None";
        case 1: return "Simple";
        case 2: return "Convex Hull";
        case 3: return "Complex";
        case 4: return "Simplified";
        case 5: return "Convex Decomp";
        default: return "Unknown";
    }
}
