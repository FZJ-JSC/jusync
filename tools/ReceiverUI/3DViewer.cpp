#include "3DViewer.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <glm/gtc/type_ptr.hpp>

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
    float ambientStrength = 0.2;
    vec3 ambient = ambientStrength * vec3(1.0);

    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * vec3(0.8);

    float specularStrength = 0.3;
    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32);
    vec3 specular = specularStrength * spec * vec3(1.0);

    vec3 result = (ambient + diffuse + specular) * objectColor;

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
        yaw -= deltaX * sensitivity;
        pitch += deltaY * sensitivity;
        pitch = std::clamp(pitch, -1.5f, 1.5f);
        updatePosition();
    } else if (rightButton || middleButton) {
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
                               collisionAlpha(0.5f), subdivisionLevel(0) {  // ✅ Changed to 0
}

MeshRenderer::~MeshRenderer() {
    cleanup();
}

bool MeshRenderer::initialize() {
    if (!createShaders()) {
        std::cerr << "Failed to create shaders" << std::endl;
        return false;
    }

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
    std::cout << "📦 MeshRenderer::loadMesh called (collision=" << isCollisionMesh << ")" << std::endl;

    // ============================================================================
    // ✅ COMPREHENSIVE NULL CHECKS - VALIDATE ALL POINTERS FIRST!
    // ============================================================================
    if (isCollisionMesh) {
        // Collision mesh validation
        if (!meshData.collision_vertices) {
            std::cerr << "❌ FATAL ERROR: NULL collision_vertices pointer!" << std::endl;
            std::cerr << "   Pointer address: " << (void*)meshData.collision_vertices << std::endl;
            return;  // ← SAFETY EXIT
        }
        if (!meshData.collision_indices) {
            std::cerr << "❌ FATAL ERROR: NULL collision_indices pointer!" << std::endl;
            std::cerr << "   Pointer address: " << (void*)meshData.collision_indices << std::endl;
            return;  // ← SAFETY EXIT
        }
        if (meshData.collision_vertices_count == 0) {
            std::cerr << "❌ ERROR: Zero collision_vertices_count!" << std::endl;
            return;
        }
        if (meshData.collision_indices_count == 0) {
            std::cerr << "❌ ERROR: Zero collision_indices_count!" << std::endl;
            return;
        }
        std::cout << "✅ Collision mesh validated: "
                  << (meshData.collision_vertices_count/3) << " vertices, "
                  << (meshData.collision_indices_count/3) << " triangles" << std::endl;
    } else {
        // Visual mesh validation
        if (!meshData.points) {
            std::cerr << "❌ FATAL ERROR: NULL points pointer!" << std::endl;
            std::cerr << "   Pointer address: " << (void*)meshData.points << std::endl;
            return;  // ← SAFETY EXIT
        }
        if (!meshData.indices) {
            std::cerr << "❌ FATAL ERROR: NULL indices pointer!" << std::endl;
            std::cerr << "   Pointer address: " << (void*)meshData.indices << std::endl;
            return;  // ← SAFETY EXIT
        }
        if (meshData.points_count == 0) {
            std::cerr << "❌ ERROR: Zero points_count!" << std::endl;
            return;
        }
        if (meshData.indices_count == 0) {
            std::cerr << "❌ ERROR: Zero indices_count!" << std::endl;
            return;
        }
        std::cout << "✅ Visual mesh validated: "
                  << (meshData.points_count/3) << " vertices, "
                  << (meshData.indices_count/3) << " triangles" << std::endl;
    }

    // ============================================================================
    // ✅ ALL POINTERS ARE VALID - PROCEED WITH MESH CREATION
    // ============================================================================
    MeshObject mesh;
    mesh.name = meshData.element_name;
    mesh.isCollision = isCollisionMesh;
    mesh.collisionType = meshData.collision_type;

    // Process visual mesh specific data
    if (!isCollisionMesh) {
        mesh.subdivisionScheme = meshData.subdivision_scheme ?
                                 std::string(meshData.subdivision_scheme) : "none";
        mesh.doubleSided = meshData.double_sided;
        mesh.numUVChannels = meshData.uv_sets_count;

        // Copy face vertex counts
        if (meshData.face_vertex_counts && meshData.face_vertex_counts_size > 0) {
            mesh.faceVertexCounts.reserve(meshData.face_vertex_counts_size);
            for (size_t i = 0; i < meshData.face_vertex_counts_size; ++i) {
                mesh.faceVertexCounts.push_back(meshData.face_vertex_counts[i]);
            }
        }

        // Copy UV set names
        if (meshData.uv_set_names && meshData.uv_sets_count > 0) {
            for (size_t i = 0; i < meshData.uv_sets_count; ++i) {
                if (meshData.uv_set_names[i]) {
                    mesh.uvSetNames.push_back(meshData.uv_set_names[i]);
                }
            }
        }
    }

    // Set mesh color
    if (isCollisionMesh) {
        mesh.color = getCollisionTypeColor(meshData.collision_type);
    } else {
        mesh.color = glm::vec3(0.7f, 0.7f, 0.9f);
    }

    // Copy bounding box data for collision meshes
    if (isCollisionMesh) {
        mesh.boundingBoxMin = glm::vec3(meshData.bounding_box_min[0],
                                       meshData.bounding_box_min[1],
                                       meshData.bounding_box_min[2]);
        mesh.boundingBoxMax = glm::vec3(meshData.bounding_box_max[0],
                                       meshData.bounding_box_max[1],
                                       meshData.bounding_box_max[2]);
        mesh.sphereCenter = glm::vec3(meshData.sphere_center[0],
                                     meshData.sphere_center[1],
                                     meshData.sphere_center[2]);
        mesh.sphereRadius = meshData.sphere_radius;
    }

    // Create GPU buffers (this function should also have NULL checks!)
    try {
        createMeshBuffers(mesh, meshData, isCollisionMesh);
        meshes.push_back(mesh);
        std::cout << "✅ Mesh '" << mesh.name << "' loaded successfully!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "❌ EXCEPTION in createMeshBuffers: " << e.what() << std::endl;
        // Clean up partially created mesh
        if (mesh.VAO) glDeleteVertexArrays(1, &mesh.VAO);
        if (mesh.VBO) glDeleteBuffers(1, &mesh.VBO);
        if (mesh.EBO) glDeleteBuffers(1, &mesh.EBO);
    }
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
    GLuint currentShader = wireframeMode ? wireframeShaderProgram : shaderProgram;
    glUseProgram(currentShader);

    glUniformMatrix4fv(glGetUniformLocation(currentShader, "model"), 1, GL_FALSE, glm::value_ptr(model));
    glUniformMatrix4fv(glGetUniformLocation(currentShader, "view"), 1, GL_FALSE, glm::value_ptr(viewMatrix));
    glUniformMatrix4fv(glGetUniformLocation(currentShader, "projection"), 1, GL_FALSE, glm::value_ptr(projMatrix));

    if (!wireframeMode) {
        glUniform3fv(glGetUniformLocation(currentShader, "lightPos"), 1, glm::value_ptr(lightPos));
        glUniform3fv(glGetUniformLocation(currentShader, "viewPos"), 1, glm::value_ptr(cameraPos));
    }

    for (const auto& mesh : meshes) {
        if (mesh.isCollision) continue;
        if (!showVisualMesh) continue;
        renderSingleMesh(mesh, currentShader, 1.0f);
    }

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
    glUniform3fv(glGetUniformLocation(shader, "objectColor"), 1, glm::value_ptr(mesh.color));
    glUniform1f(glGetUniformLocation(shader, "alpha"), alpha);

    if (wireframeMode) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(mesh.isCollision ? 2.0f : 1.0f);
    } else {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    glBindVertexArray(mesh.VAO);
    if (mesh.indexCount > 0) {
        glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, 0);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, mesh.vertexCount);
    }
    glBindVertexArray(0);

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
        minBounds = glm::vec3(-1.0f);
        maxBounds = glm::vec3(1.0f);
    }

    return true;
}

// ============================================================================
// SUBDIVISION SURFACE IMPLEMENTATION
// ============================================================================
void MeshRenderer::subdivideMesh(std::vector<float>& vertices, std::vector<unsigned int>& indices,
                                 const std::vector<unsigned int>& faceVertexCounts, int levels) {
    if (levels <= 0 || faceVertexCounts.empty()) return;

    std::vector<glm::vec3> verts;
    for (size_t i = 0; i < vertices.size(); i += 3) {
        verts.push_back(glm::vec3(vertices[i], vertices[i+1], vertices[i+2]));
    }

    for (int i = 0; i < levels; ++i) {
        subdivideCatmullClark(verts, indices, faceVertexCounts);
    }

    vertices.clear();
    for (const auto& v : verts) {
        vertices.push_back(v.x);
        vertices.push_back(v.y);
        vertices.push_back(v.z);
    }
}

void MeshRenderer::subdivideCatmullClark(std::vector<glm::vec3>& vertices,
                                        std::vector<unsigned int>& indices,
                                        const std::vector<unsigned int>& faceVertexCounts) {
    std::map<std::pair<unsigned int, unsigned int>, std::vector<unsigned int>> edgeToFaces;
    std::vector<glm::vec3> facePoints;

    size_t indexOffset = 0;
    for (size_t f = 0; f < faceVertexCounts.size(); ++f) {
        unsigned int faceSize = faceVertexCounts[f];

        glm::vec3 facePoint(0.0f);
        for (unsigned int i = 0; i < faceSize; ++i) {
            facePoint += vertices[indices[indexOffset + i]];
        }
        facePoint /= static_cast<float>(faceSize);
        facePoints.push_back(facePoint);

        for (unsigned int i = 0; i < faceSize; ++i) {
            unsigned int v1 = indices[indexOffset + i];
            unsigned int v2 = indices[indexOffset + (i + 1) % faceSize];
            auto edge = std::make_pair(std::min(v1, v2), std::max(v1, v2));
            edgeToFaces[edge].push_back(static_cast<unsigned int>(f));
        }

        indexOffset += faceSize;
    }

    std::map<std::pair<unsigned int, unsigned int>, glm::vec3> edgePoints;
    for (const auto& pair : edgeToFaces) {
        const auto& edge = pair.first;
        const auto& faces = pair.second;

        glm::vec3 edgePoint = (vertices[edge.first] + vertices[edge.second]) * 0.5f;

        if (faces.size() == 2) {
            edgePoint = (vertices[edge.first] + vertices[edge.second] +
                        facePoints[faces[0]] + facePoints[faces[1]]) * 0.25f;
        }

        edgePoints[edge] = edgePoint;
    }

    std::vector<glm::vec3> newVertices = vertices;
    for (size_t v = 0; v < vertices.size(); ++v) {
        std::vector<unsigned int> adjacentFaces;
        std::vector<std::pair<unsigned int, unsigned int>> adjacentEdges;

        indexOffset = 0;
        for (size_t f = 0; f < faceVertexCounts.size(); ++f) {
            unsigned int faceSize = faceVertexCounts[f];
            for (unsigned int i = 0; i < faceSize; ++i) {
                if (indices[indexOffset + i] == static_cast<unsigned int>(v)) {
                    adjacentFaces.push_back(static_cast<unsigned int>(f));

                    unsigned int prev = indices[indexOffset + (i + faceSize - 1) % faceSize];
                    unsigned int next = indices[indexOffset + (i + 1) % faceSize];

                    // ✅ Fixed type casting
                    adjacentEdges.push_back(std::make_pair(
                        std::min(static_cast<unsigned int>(v), prev),
                        std::max(static_cast<unsigned int>(v), prev)
                    ));
                    adjacentEdges.push_back(std::make_pair(
                        std::min(static_cast<unsigned int>(v), next),
                        std::max(static_cast<unsigned int>(v), next)
                    ));
                    break;
                }
            }
            indexOffset += faceSize;
        }

        if (!adjacentFaces.empty()) {
            glm::vec3 F(0.0f);
            for (unsigned int f : adjacentFaces) {
                F += facePoints[f];
            }
            F /= static_cast<float>(adjacentFaces.size());

            glm::vec3 R(0.0f);
            for (const auto& edge : adjacentEdges) {
                R += edgePoints[edge];
            }
            R /= static_cast<float>(adjacentEdges.size());

            float n = static_cast<float>(adjacentFaces.size());
            newVertices[v] = (F + 2.0f * R + (n - 3.0f) * vertices[v]) / n;
        }
    }

    std::vector<unsigned int> newIndices;
    std::vector<glm::vec3> finalVertices = newVertices;

    std::map<std::pair<unsigned int, unsigned int>, unsigned int> edgePointIndices;
    unsigned int nextIndex = static_cast<unsigned int>(finalVertices.size());

    for (const auto& pair : edgePoints) {
        edgePointIndices[pair.first] = nextIndex++;
        finalVertices.push_back(pair.second);
    }

    std::vector<unsigned int> facePointIndices;
    for (const auto& fp : facePoints) {
        facePointIndices.push_back(nextIndex++);
        finalVertices.push_back(fp);
    }

    indexOffset = 0;
    for (size_t f = 0; f < faceVertexCounts.size(); ++f) {
        unsigned int faceSize = faceVertexCounts[f];
        unsigned int fpIdx = facePointIndices[f];

        for (unsigned int i = 0; i < faceSize; ++i) {
            unsigned int v0 = indices[indexOffset + i];
            unsigned int v1 = indices[indexOffset + (i + 1) % faceSize];

            auto edge1 = std::make_pair(
                std::min(v0, indices[indexOffset + (i + faceSize - 1) % faceSize]),
                std::max(v0, indices[indexOffset + (i + faceSize - 1) % faceSize])
            );
            auto edge2 = std::make_pair(std::min(v0, v1), std::max(v0, v1));

            unsigned int ep1 = edgePointIndices[edge1];
            unsigned int ep2 = edgePointIndices[edge2];

            newIndices.push_back(v0);
            newIndices.push_back(ep2);
            newIndices.push_back(fpIdx);
            newIndices.push_back(ep1);
        }

        indexOffset += faceSize;
    }

    vertices = finalVertices;
    indices = newIndices;
}

void MeshRenderer::createMeshBuffers(MeshObject& mesh, const CMeshData& meshData, bool isCollisionMesh) {
    std::cout << "🔨 createMeshBuffers called (collision=" << isCollisionMesh << ")" << std::endl;

    // ============================================================================
    // ✅ STEP 1: VALIDATE AND EXTRACT MESH DATA
    // ============================================================================
    const float* vertices = nullptr;
    const unsigned int* indices = nullptr;
    size_t vertexCount = 0;
    size_t indexCount = 0;

    if (isCollisionMesh) {
        // Collision mesh validation
        if (!meshData.collision_vertices || !meshData.collision_indices) {
            std::cerr << "❌ FATAL: NULL collision pointers!" << std::endl;
            return;
        }
        if (meshData.collision_vertices_count == 0 || meshData.collision_indices_count == 0) {
            std::cerr << "❌ FATAL: Zero-size collision data!" << std::endl;
            return;
        }

        vertices = meshData.collision_vertices;
        indices = meshData.collision_indices;
        vertexCount = meshData.collision_vertices_count / 3;  // Total floats → vertex count
        indexCount = meshData.collision_indices_count;

        std::cout << "✅ Collision: " << vertexCount << " vertices, " << (indexCount/3) << " triangles" << std::endl;
    } else {
        // Visual mesh validation
        if (!meshData.points || !meshData.indices) {
            std::cerr << "❌ FATAL: NULL visual mesh pointers!" << std::endl;
            return;
        }
        if (meshData.points_count == 0 || meshData.indices_count == 0) {
            std::cerr << "❌ FATAL: Zero-size visual data!" << std::endl;
            return;
        }

        vertices = meshData.points;
        indices = meshData.indices;
        vertexCount = meshData.points_count / 3;  // Total floats → vertex count
        indexCount = meshData.indices_count;

        std::cout << "✅ Visual: " << vertexCount << " vertices, " << (indexCount/3) << " triangles" << std::endl;
    }

    // Final sanity check
    if (vertexCount == 0 || indexCount == 0) {
        std::cerr << "❌ FATAL: Zero vertex/index count!" << std::endl;
        return;
    }

    // ============================================================================
    // ✅ STEP 2: CREATE OPENGL BUFFERS
    // ============================================================================
    glGenVertexArrays(1, &mesh.VAO);
    glGenBuffers(1, &mesh.VBO);
    glGenBuffers(1, &mesh.EBO);
    glBindVertexArray(mesh.VAO);

    // ============================================================================
    // ✅ STEP 3: COPY VERTEX AND INDEX DATA
    // ============================================================================
    std::vector<float> subdividedVerts(vertices, vertices + vertexCount * 3);
    std::vector<unsigned int> subdividedIndices(indices, indices + indexCount);

    // ============================================================================
    // ✅ STEP 4: SMART SUBDIVISION (VISUAL MESHES ONLY)
    // ============================================================================
    bool shouldSubdivide = false;

    if (!isCollisionMesh &&
        mesh.subdivisionScheme != "none" &&
        subdivisionLevel > 0 &&
        !mesh.faceVertexCounts.empty()) {

        // Check if mesh has quads
        bool hasQuads = false;
        for (auto count : mesh.faceVertexCounts) {
            if (count == 4) {
                hasQuads = true;
                break;
            }
        }

        shouldSubdivide = hasQuads;

        if (!hasQuads && subdivisionLevel > 0) {
            std::cout << "⚠️ Mesh '" << mesh.name << "' already triangulated. Skipping subdivision." << std::endl;
        }
    }

    if (shouldSubdivide) {
        std::cout << "✅ Applying " << mesh.subdivisionScheme << " subdivision level "
                  << subdivisionLevel << " to '" << mesh.name << "'" << std::endl;
        subdivideMesh(subdividedVerts, subdividedIndices, mesh.faceVertexCounts, subdivisionLevel);
        vertexCount = subdividedVerts.size() / 3;
        indexCount = subdividedIndices.size();
    }

    // ============================================================================
    // ✅ STEP 5: BUILD VERTEX DATA WITH NORMALS & COLORS
    // ============================================================================
    mesh.vertexCount = vertexCount;
    mesh.indexCount = indexCount;

    std::vector<float> vertexData;
    vertexData.reserve(vertexCount * 10);  // pos(3) + normal(3) + color(4)

    // Initial vertex data (positions + placeholder normals + colors)
    for (size_t i = 0; i < vertexCount; ++i) {
        // Position
        vertexData.push_back(subdividedVerts[i * 3 + 0]);
        vertexData.push_back(subdividedVerts[i * 3 + 1]);
        vertexData.push_back(subdividedVerts[i * 3 + 2]);

        // Normal (placeholder, will compute below)
        vertexData.push_back(0.0f);
        vertexData.push_back(1.0f);
        vertexData.push_back(0.0f);

        // Color (white for now)
        vertexData.push_back(1.0f);
        vertexData.push_back(1.0f);
        vertexData.push_back(1.0f);
        vertexData.push_back(1.0f);
    }

    // Compute normals
    std::vector<glm::vec3> normals(vertexCount, glm::vec3(0.0f));
    for (size_t i = 0; i < indexCount; i += 3) {
        unsigned int i0 = subdividedIndices[i];
        unsigned int i1 = subdividedIndices[i + 1];
        unsigned int i2 = subdividedIndices[i + 2];

        glm::vec3 v0(subdividedVerts[i0 * 3], subdividedVerts[i0 * 3 + 1], subdividedVerts[i0 * 3 + 2]);
        glm::vec3 v1(subdividedVerts[i1 * 3], subdividedVerts[i1 * 3 + 1], subdividedVerts[i1 * 3 + 2]);
        glm::vec3 v2(subdividedVerts[i2 * 3], subdividedVerts[i2 * 3 + 1], subdividedVerts[i2 * 3 + 2]);

        glm::vec3 normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
        normals[i0] += normal;
        normals[i1] += normal;
        normals[i2] += normal;
    }

    // Update normals in vertex data
    for (size_t i = 0; i < vertexCount; ++i) {
        glm::vec3 n = glm::normalize(normals[i]);
        vertexData[i * 10 + 3] = n.x;
        vertexData[i * 10 + 4] = n.y;
        vertexData[i * 10 + 5] = n.z;
    }

    // ============================================================================
    // ✅ STEP 6: UPLOAD TO GPU
    // ============================================================================
    glBindBuffer(GL_ARRAY_BUFFER, mesh.VBO);
    glBufferData(GL_ARRAY_BUFFER, vertexData.size() * sizeof(float), vertexData.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexCount * sizeof(unsigned int), subdividedIndices.data(), GL_STATIC_DRAW);

    // ============================================================================
    // ✅ STEP 7: CONFIGURE VERTEX ATTRIBUTES
    // ============================================================================
    // Position attribute (location = 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Normal attribute (location = 1)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Color attribute (location = 2)
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);

    std::cout << "✅ Mesh buffers created successfully!" << std::endl;
}


glm::vec3 MeshRenderer::getCollisionTypeColor(int collisionType) const {
    switch (collisionType) {
        case 0: return glm::vec3(0.5f, 0.5f, 0.5f);
        case 1: return glm::vec3(0.0f, 1.0f, 0.0f);
        case 2: return glm::vec3(0.0f, 0.5f, 1.0f);
        case 3: return glm::vec3(1.0f, 0.0f, 0.0f);
        case 4: return glm::vec3(1.0f, 1.0f, 0.0f);
        case 5: return glm::vec3(1.0f, 0.0f, 1.0f);
        default: return glm::vec3(1.0f, 1.0f, 1.0f);
    }
}

bool MeshRenderer::createShaders() {
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
                       collisionAlpha(0.5f), subdivisionLevel(0), selectedMeshIndex(-1),  // ✅ Changed to 0
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
        renderUSDGeometryFeatures(meshes);
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

    if (ImGui::SliderInt("🔷 Subdivision Level", &subdivisionLevel, 0, 3)) {
        renderer->setSubdivisionLevel(subdivisionLevel);
    }

    if (showCollisionMesh) {
        ImGui::SliderFloat("Collision Alpha", &collisionAlpha, 0.1f, 1.0f, "%.2f");
    }

    ImGui::Text("💡 Light Position:");
    ImGui::SliderFloat3("##LightPos", glm::value_ptr(lightPosition), -20.0f, 20.0f);

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

    if (viewportSize.x < 200) viewportSize.x = 200;
    if (viewportSize.y < 200) viewportSize.y = 200;

    if (static_cast<int>(viewportSize.x) != viewportWidth ||
        static_cast<int>(viewportSize.y) != viewportHeight) {
        setViewportSize(static_cast<int>(viewportSize.x), static_cast<int>(viewportSize.y));
    }

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, viewportWidth, viewportHeight);
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (renderer->getMeshCount() > 0) {
        float aspectRatio = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
        glm::mat4 view = camera->getViewMatrix();
        glm::mat4 proj = camera->getProjectionMatrix(aspectRatio);
        renderer->render(view, proj, lightPosition, camera->getPosition());
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    ImGui::Image(reinterpret_cast<void*>(colorTexture), viewportSize, ImVec2(0, 1), ImVec2(1, 0));

    handleMouseInput(viewportSize);

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
        ImGui::Text("V:%zu T:%zu", mesh.points_count / 3, mesh.indices_count / 3);
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

void Viewer3D::renderUSDGeometryFeatures(const std::vector<CMeshData>& meshes) {
    if (selectedMeshIndex < 0 || selectedMeshIndex >= static_cast<int>(meshes.size())) {
        return;
    }

    const auto& rendererMeshes = renderer->getMeshes();

    if (static_cast<size_t>(selectedMeshIndex) >= rendererMeshes.size()) {
        return;
    }

    const auto& rendererMesh = rendererMeshes[selectedMeshIndex];

    if (rendererMesh.isCollision) {
        return;
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "🔷 USD Geometry Features");

    if (rendererMesh.subdivisionScheme != "none") {
        ImGui::Text("Subdivision:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "%s", rendererMesh.subdivisionScheme.c_str());

        if (!rendererMesh.faceVertexCounts.empty()) {
            size_t triCount = 0, quadCount = 0, nGonCount = 0;
            for (auto vCount : rendererMesh.faceVertexCounts) {
                if (vCount == 3) triCount++;
                else if (vCount == 4) quadCount++;
                else if (vCount > 4) nGonCount++;
            }

            ImGui::Indent();
            ImGui::Text("Faces: %zu total", rendererMesh.faceVertexCounts.size());
            if (triCount > 0) ImGui::Text("  Tris: %zu", triCount);
            if (quadCount > 0) ImGui::Text("  Quads: %zu", quadCount);
            if (nGonCount > 0) ImGui::Text("  N-gons: %zu", nGonCount);
            ImGui::Unindent();
        }
    }

    if (rendererMesh.doubleSided) {
        ImGui::Text("Rendering:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Double-Sided");
    }

    if (rendererMesh.numUVChannels > 1) {
        ImGui::Text("UV Channels: %d", rendererMesh.numUVChannels);
        ImGui::Indent();
        for (size_t i = 0; i < rendererMesh.uvSetNames.size() && i < 8; ++i) {
            ImGui::BulletText("[%zu] %s", i, rendererMesh.uvSetNames[i].c_str());
        }
        ImGui::Unindent();
    }
}

void Viewer3D::handleMouseInput(const ImVec2& viewportSize) {
    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();
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

        if (io.MouseWheel != 0.0f) {
            camera->processScrollInput(io.MouseWheel);
        }
    } else {
        isDragging = false;
    }
}

void Viewer3D::loadMeshesIntoRenderer(const std::vector<CMeshData>& meshes) {
    std::cout << "🔍 loadMeshesIntoRenderer: Processing " << meshes.size() << " meshes" << std::endl;
    renderer->clearMeshes();

    for (size_t i = 0; i < meshes.size(); i++) {
        const auto& mesh = meshes[i];

        // NULL checks
        if (!mesh.points || !mesh.indices || mesh.points_count == 0 || mesh.indices_count == 0) {
            std::cerr << "❌ MESH " << i << ": Invalid visual mesh data!" << std::endl;
            continue;
        }

        std::cout << "✅ Loading mesh " << i << ": " << (mesh.points_count/3) << " vertices" << std::endl;

        try {
            renderer->loadMesh(mesh, false);  // Visual mesh only
        } catch (const std::exception& e) {
            std::cerr << "❌ Exception: " << e.what() << std::endl;
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
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    glGenTextures(1, &colorTexture);
    glBindTexture(GL_TEXTURE_2D, colorTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture, 0);

    glGenTextures(1, &depthTexture);
    glBindTexture(GL_TEXTURE_2D, depthTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTexture, 0);

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
