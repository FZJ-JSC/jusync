#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <vector>
#include <string>
#include <memory>

#include "AnariUsdMiddleware_C.h" // For CMeshData

class Camera3D {
public:
    Camera3D();

    // Camera controls
    void processMouseInput(float deltaX, float deltaY, bool leftButton, bool rightButton, bool middleButton);
    void processScrollInput(float scrollDelta);
    void reset();
    void focusOnBounds(const glm::vec3& minBounds, const glm::vec3& maxBounds);

    // Matrix getters
    glm::mat4 getViewMatrix() const;
    glm::mat4 getProjectionMatrix(float aspectRatio) const;

    // Camera state
    glm::vec3 getPosition() const { return position; }
    glm::vec3 getTarget() const { return target; }
    float getDistance() const { return distance; }

private:
    glm::vec3 target;
    glm::vec3 position;
    float distance;
    float yaw, pitch;
    float fov;
    float nearPlane, farPlane;

    void updatePosition();
};

class MeshRenderer {
public:
    MeshRenderer();
    ~MeshRenderer();

    // Initialization
    bool initialize();
    void cleanup();

    // Mesh management
    void loadMesh(const CMeshData& meshData, bool isCollisionMesh = false);
    void clearMeshes();

    // Rendering
    void render(const glm::mat4& viewMatrix, const glm::mat4& projMatrix,
                const glm::vec3& lightPos, const glm::vec3& cameraPos);

    // Rendering options
    void setWireframeMode(bool wireframe) { wireframeMode = wireframe; }
    void setShowNormals(bool show) { showNormals = show; }
    void setShowCollision(bool show) { showCollisionMesh = show; }
    void setShowVisual(bool show) { showVisualMesh = show; }  // ADDED
    void setCollisionAlpha(float alpha) { collisionAlpha = alpha; }

    // Mesh info
    size_t getMeshCount() const { return meshes.size(); }
    size_t getVisualMeshCount() const;
    size_t getCollisionMeshCount() const;

    // Bounds calculation
    bool calculateBounds(glm::vec3& minBounds, glm::vec3& maxBounds) const;

private:
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

        MeshObject() : VAO(0), VBO(0), EBO(0), normalVBO(0), colorVBO(0),
                      indexCount(0), vertexCount(0), isCollision(false),
                      color(0.8f, 0.8f, 0.8f), collisionType(0),
                      boundingBoxMin(0.0f), boundingBoxMax(0.0f),
                      sphereCenter(0.0f), sphereRadius(0.0f) {}
    };

    std::vector<MeshObject> meshes;

    // OpenGL resources
    GLuint shaderProgram;
    GLuint wireframeShaderProgram;

    // Rendering state
    bool wireframeMode;
    bool showNormals;
    bool showCollisionMesh;
    bool showVisualMesh;  // ADDED
    float collisionAlpha;

    // Shader compilation
    bool createShaders();
    GLuint compileShader(const char* source, GLenum type);
    GLuint linkProgram(GLuint vertexShader, GLuint fragmentShader);

    // Mesh creation helpers - FIXED SIGNATURE
    void createMeshBuffers(MeshObject& mesh, const CMeshData& meshData, bool isCollisionMesh);
    void renderSingleMesh(const MeshObject& mesh, GLuint shader, float alpha);  // ADDED

    // Color helpers
    glm::vec3 getCollisionTypeColor(int collisionType) const;
};

class Viewer3D {
public:
    Viewer3D();
    ~Viewer3D();

    // Initialization
    bool initialize();
    void cleanup();

    // Main interface
    void render(const std::vector<CMeshData>& meshes);

    // Visibility control
    void setVisible(bool visible) { isVisible = visible; }
    bool getVisible() const { return isVisible; }

    // Viewport size
    void setViewportSize(int width, int height);

private:
    bool isVisible;

    // Components
    std::unique_ptr<Camera3D> camera;
    std::unique_ptr<MeshRenderer> renderer;

    // Framebuffer for rendering to texture
    GLuint framebuffer;
    GLuint colorTexture;
    GLuint depthTexture;
    int viewportWidth, viewportHeight;

    // UI state
    bool wireframeMode;
    bool showNormals;
    bool showCollisionMesh;
    bool showVisualMesh;
    float collisionAlpha;
    int selectedMeshIndex;

    // Lighting
    glm::vec3 lightPosition;

    // Mouse interaction
    ImVec2 lastMousePos;
    bool isDragging;

    // Framebuffer management
    bool createFramebuffer(int width, int height);
    void deleteFramebuffer();

    // UI rendering
    void renderControls();
    void renderViewport();
    void renderMeshList(const std::vector<CMeshData>& meshes);
    void renderSelectedMeshInfo(const std::vector<CMeshData>& meshes);

    // Utility methods
    void loadMeshesIntoRenderer(const std::vector<CMeshData>& meshes);
    void handleMouseInput(const ImVec2& viewportSize);
    void focusOnAllMeshes();  // ADDED
    std::string getCollisionTypeName(int type) const;  // ADDED
};

// Shader sources
namespace Shaders {
    extern const char* vertexShaderSource;
    extern const char* fragmentShaderSource;
    extern const char* wireframeVertexSource;
    extern const char* wireframeFragmentSource;
}
