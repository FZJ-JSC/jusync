#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <glm/glm.hpp>
#include "MiddlewareLogging.h"

#ifndef ANARI_USD_MIDDLEWARE_API
#ifdef _WIN32
#define ANARI_USD_MIDDLEWARE_API __declspec(dllexport)
#else
#define ANARI_USD_MIDDLEWARE_API __attribute__((visibility("default")))
#endif
#endif

namespace tinyusdz {
    class Prim;
    class Stage;
}

namespace anari_usd_middleware {

/**
 * Provides USD processing capabilities for the ANARI-USD middleware.
 * This class handles USD file conversion, mesh extraction, and texture processing.
 */
class ANARI_USD_MIDDLEWARE_API UsdProcessor {
public:
    /**
     * Mesh data structure to hold extracted geometry from USD files.
     */
    struct MeshData {
        std::string elementName;    ///< Name of the USD element
        std::string typeName;       ///< Type of the USD element
        std::vector<glm::vec3> points;  ///< 3D vertex positions
        std::vector<uint32_t> indices;  ///< Triangle indices
        std::vector<glm::vec3> normals; ///< Normal vectors
        std::vector<glm::vec2> uvs;     ///< Texture coordinates
    };

    /**
     * Texture data structure for image information.
     */
    struct TextureData {
        int width = 0;              ///< Width in pixels
        int height = 0;             ///< Height in pixels
        int channels = 0;           ///< Number of color channels
        std::vector<uint8_t> data;  ///< Raw pixel data
    };

    /**
     * Constructor.
     */
    UsdProcessor();

    /**
     * Destructor.
     */
    ~UsdProcessor();

    /**
     * Convert USD file to USDC format (handles .usd and .usda).
     * @param inputFilePath Path to the input USD file
     * @param outputFilePath Path where the USDC file should be written
     * @return True if conversion was successful, false otherwise
     */
    bool ConvertUSDtoUSDC(const std::string& inputFilePath, const std::string& outputFilePath);

    /**
     * Create texture from raw buffer data.
     * @param buffer Raw image data buffer
     * @return TextureData structure containing the processed texture
     */
    TextureData CreateTextureFromBuffer(const std::vector<uint8_t>& buffer);

    /**
     * Load USD data from buffer and extract mesh information.
     * @param buffer Raw USD data buffer
     * @param fileName Original filename (used for format detection)
     * @param outMeshData Output vector to store the extracted mesh data
     * @return True if loading was successful, false otherwise
     */
    bool LoadUSDBuffer(const std::vector<uint8_t>& buffer, const std::string& fileName,
                      std::vector<MeshData>& outMeshData);

private:
    /**
     * Process a USD primitive and its children recursively.
     * @param prim Pointer to the USD primitive
     * @param meshDataArray Output array to store extracted mesh data
     * @param parentTransform Parent transformation matrix
     * @param depth Current recursion depth
     */
    void ProcessPrim(void* prim, std::vector<MeshData>& meshDataArray,
                     const glm::mat4& parentTransform, int32_t depth);

    /**
     * Extract mesh data from a USD mesh primitive.
     * @param mesh Pointer to the USD mesh
     * @param outMeshData Output structure to store the extracted mesh data
     * @param worldTransform World transformation matrix
     */

    void ExtractReferencePaths(const tinyusdz::Stage& stage, std::vector<std::string>& outReferencePaths);

    std::vector<std::string> ExtractClipsFromRawContent(const std::vector<uint8_t> &buffer);

    void ExtractReferencePathsFromPrim(const tinyusdz::Prim& prim, std::vector<std::string>& outReferencePaths);
    void ListPrimHierarchy(const tinyusdz::Prim& prim, int depth);



    void ExtractMeshData(void* mesh, MeshData& outMeshData,
                         const glm::mat4& worldTransform);

    /**
     * Get the local transformation matrix for a USD primitive.
     * @param prim Pointer to the USD primitive
     * @return Local transformation matrix
     */
    glm::mat4 GetLocalTransform(void* prim);

    // TinyUSDZ implementation details
    class UsdProcessorImpl;
    std::unique_ptr<UsdProcessorImpl> pImpl;
};

} 
