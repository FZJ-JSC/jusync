#pragma once

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <vector>
#include <string>
#include <memory>
#include <map>
#include "../include/AnariUsdMiddleware_C.h"

// ============================================================================
// CAMERA3D CLASS
// ============================================================================
class Camera3D {
public:
    Camera3D();

    void processMouseInput(float deltaX, float deltaY, bool leftButton, bool rightButton, bool middleButton);
    void processScrollInput(float scrollDelta);
    void reset();
    void focusOnBounds(const glm::vec3& minBounds, const glm::vec3& maxBounds);

    glm::mat4 getViewMatrix() const;
    glm::mat4 getProjectionMatrix(float aspectRatio) const;

    inline glm::vec3 getPosition() const { return position; }
    inline glm::vec3 getTarget() const { return target; }
    inline float getDistance() const { return distance; }

private:
    void updatePosition();

    glm::vec3 position;
    glm::vec3 target;
    float distance;
    float yaw;
    float pitch;
    float fov;
    float nearPlane;
    float farPlane;
};

// ============================================================================
// MESH OBJECT STRUCT
// ============================================================================
struct MeshObject {
    GLuint VAO, VBO, EBO, normalVBO, colorVBO;
    size_t indexCount;
    size_t vertexCount;
    std::string name;
    bool isCollision;
    glm::vec3 color;

    // Collision mesh specific data
    int collisionType;
    glm::vec3 boundingBoxMin, boundingBoxMax;
    glm::vec3 sphereCenter;
    float sphereRadius;

    // ✅ USD Geometry Features
    std::string subdivisionScheme = "none";
    bool doubleSided = false;
    std::vector<unsigned int> faceVertexCounts;
    std::vector<std::string> uvSetNames;
    int numUVChannels = 0;

    MeshObject() : VAO(0), VBO(0), EBO(0), normalVBO(0), colorVBO(0),
                   indexCount(0), vertexCount(0), isCollision(false),
                   color(0.8f, 0.8f, 0.8f), collisionType(0),
                   boundingBoxMin(0.0f), boundingBoxMax(0.0f),
                   sphereCenter(0.0f), sphereRadius(0.0f) {}
};

// ============================================================================
// MESHRENDERER CLASS
// ============================================================================
class MeshRenderer {
public:
    MeshRenderer();
    ~MeshRenderer();

    bool initialize();
    void cleanup();

    void loadMesh(const CMeshData& meshData, bool isCollisionMesh = false);
    void clearMeshes();
    void render(const glm::mat4& viewMatrix, const glm::mat4& projMatrix,
                const glm::vec3& lightPos, const glm::vec3& cameraPos);

    const std::vector<MeshObject>& getMeshes() const { return meshes; }

    // Mesh statistics
    inline size_t getMeshCount() const { return meshes.size(); }
    size_t getVisualMeshCount() const;
    size_t getCollisionMeshCount() const;
    bool calculateBounds(glm::vec3& minBounds, glm::vec3& maxBounds) const;

    // Rendering options
    inline void setWireframeMode(bool enabled) { wireframeMode = enabled; }
    inline void setShowNormals(bool enabled) { showNormals = enabled; }
    inline void setShowCollision(bool enabled) { showCollisionMesh = enabled; }
    inline void setShowVisual(bool enabled) { showVisualMesh = enabled; }
    inline void setCollisionAlpha(float alpha) { collisionAlpha = alpha; }

    // ✅ NEW: Subdivision control
    inline void setSubdivisionLevel(int level) { subdivisionLevel = level; }
    inline int getSubdivisionLevel() const { return subdivisionLevel; }

private:
    void createMeshBuffers(MeshObject& mesh, const CMeshData& meshData, bool isCollisionMesh);
    void renderSingleMesh(const MeshObject& mesh, GLuint shader, float alpha = 1.0f);
    glm::vec3 getCollisionTypeColor(int collisionType) const;

    // ✅ NEW: Subdivision functions
    void subdivideMesh(std::vector<float>& vertices, std::vector<unsigned int>& indices,
                      const std::vector<unsigned int>& faceVertexCounts, int levels);
    void subdivideCatmullClark(std::vector<glm::vec3>& vertices, std::vector<unsigned int>& indices,
                               const std::vector<unsigned int>& faceVertexCounts);

    bool createShaders();
    GLuint compileShader(const char* source, GLenum type);
    GLuint linkProgram(GLuint vertexShader, GLuint fragmentShader);

    std::vector<MeshObject> meshes;
    GLuint shaderProgram;
    GLuint wireframeShaderProgram;

    bool wireframeMode;
    bool showNormals;
    bool showCollisionMesh;
    bool showVisualMesh;
    float collisionAlpha;
    int subdivisionLevel; // ✅ NEW
};

// ============================================================================
// VIEWER3D CLASS
// ============================================================================
class Viewer3D {
public:
    Viewer3D();
    ~Viewer3D();

    bool initialize();
    void cleanup();
    void render(const std::vector<CMeshData>& meshes);

    inline bool isWindowVisible() const { return isVisible; }
    inline void setVisible(bool visible) { isVisible = visible; }

private:
    void renderControls();
    void renderViewport();
    void renderMeshList(const std::vector<CMeshData>& meshes);
    void renderSelectedMeshInfo(const std::vector<CMeshData>& meshes);
    void renderUSDGeometryFeatures(const std::vector<CMeshData>& meshes);

    void handleMouseInput(const ImVec2& viewportSize);
    void loadMeshesIntoRenderer(const std::vector<CMeshData>& meshes);
    void focusOnAllMeshes();
    void setViewportSize(int width, int height);

    bool createFramebuffer(int width, int height);
    void deleteFramebuffer();

    std::string getCollisionTypeName(int type) const;

    std::unique_ptr<Camera3D> camera;
    std::unique_ptr<MeshRenderer> renderer;

    bool isVisible;
    GLuint framebuffer, colorTexture, depthTexture;
    int viewportWidth, viewportHeight;

    bool wireframeMode;
    bool showNormals;
    bool showCollisionMesh;
    bool showVisualMesh;
    float collisionAlpha;
    int subdivisionLevel; // ✅ NEW

    int selectedMeshIndex;
    glm::vec3 lightPosition;

    bool isDragging;
    ImVec2 lastMousePos;
};
