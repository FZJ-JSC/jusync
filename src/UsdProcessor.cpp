#include "UsdProcessor.h"
#include "MiddlewareLogging.h"

// Standard library includes

#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <limits>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <regex>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// Include TinyUSDZ
#include "tinyusdz.hh"


#define STB_IMAGE_IMPLEMENTATION
#include <io-util.hh>

#include "stb_image.h"

#define DEBUG_LEVEL 2

namespace anari_usd_middleware {

class UsdProcessor::UsdProcessorImpl {
public:
    // Helper method to preprocess USD content
    std::vector<uint8_t> preprocessUsdContent(const std::vector<uint8_t>& buffer) {
        MIDDLEWARE_LOG_INFO("Preprocessing USD content of size %zu", buffer.size());

        // Convert buffer to string for preprocessing
        std::string fileContent(reinterpret_cast<const char*>(buffer.data()), buffer.size());

        // Preprocess content to fix common issues (ported from TinyUSDZFunctionLibrary) -> found in anari-usd output file
        fileContent = std::regex_replace(fileContent, std::regex("0: None"), "0: []");
        fileContent = std::regex_replace(fileContent, std::regex("asset:images/"), "@./images/");
        fileContent = std::regex_replace(fileContent, std::regex("texCoord2f"), "texCoord2f[]");

        std::istringstream iss(fileContent);
        std::vector<std::string> lines;
        std::string line;

        while (std::getline(iss, line)) {
            lines.push_back(line);
        }

        if (lines.size() > 33) {
            std::string& line34 = lines[33];
            MIDDLEWARE_LOG_DEBUG("Line 34 content: %s", line34.c_str());
            if (line34.find("texture") != std::string::npos || line34.find("albedoTex") != std::string::npos) {
                if (line34.find("uniform") == std::string::npos) {
                    lines[33] = "uniform token info:id = \"UsdPreviewSurface\";" + line34;
                    MIDDLEWARE_LOG_DEBUG("Modified Line 34: %s", lines[33].c_str());
                }
            }
        }

        // Rebuild content
        fileContent.clear();
        for (const auto& l : lines) {
            fileContent += l + "\n";
        }

        // Convert back to buffer
        MIDDLEWARE_LOG_INFO("Preprocessing complete, new size: %zu", fileContent.size());
        return std::vector<uint8_t>(fileContent.begin(), fileContent.end());
    }
};

UsdProcessor::UsdProcessor() : pImpl(new UsdProcessorImpl()) {
    MIDDLEWARE_LOG_INFO("UsdProcessor created");
}

UsdProcessor::~UsdProcessor() {
    MIDDLEWARE_LOG_INFO("UsdProcessor destroyed");
}


//convert usdc to usda or usd using tusdcat -> not available at the moment
bool UsdProcessor::ConvertUSDtoUSDC(const std::string& inputFilePath, const std::string& outputFilePath) {
    MIDDLEWARE_LOG_INFO("Converting USD to USDC: %s -> %s", inputFilePath.c_str(), outputFilePath.c_str());

    if (!std::filesystem::exists(inputFilePath)) {
        MIDDLEWARE_LOG_ERROR("Input file does not exist: %s", inputFilePath.c_str());
        return false;
    }

    // Check if the input is already a USDC file
    std::string extension = std::filesystem::path(inputFilePath).extension().string();
    if (extension == ".usdc") {
        MIDDLEWARE_LOG_INFO("Input is already USDC, copying file");
        // If the input is already USDC, just copy it
        try {
            std::filesystem::copy_file(inputFilePath, outputFilePath,
                                      std::filesystem::copy_options::overwrite_existing);
            return true;
        } catch (const std::exception& e) {
            MIDDLEWARE_LOG_ERROR("Error copying file: %s", e.what());
            return false;
        }
    }

    // Check if the input is a USD or USDA file
    if (extension != ".usd" && extension != ".usda") {
        MIDDLEWARE_LOG_ERROR("Input file is not a USD format: %s", inputFilePath.c_str());
        return false;
    }

    std::string outputDirectory = std::filesystem::path(outputFilePath).parent_path().string();
    if (!std::filesystem::exists(outputDirectory)) {
        std::filesystem::create_directories(outputDirectory);
    }

    // Path to tusdcat executable
    std::string tusdcatPath = "";
    // Build command
    std::string command = tusdcatPath + " -o \"" + outputFilePath + "\" \"" + inputFilePath + "\"";
    // Execute command
    MIDDLEWARE_LOG_INFO("Executing command: %s", command.c_str());
    int result = system(command.c_str());

    if (result != 0 || !std::filesystem::exists(outputFilePath)) {
        MIDDLEWARE_LOG_ERROR("Failed to convert file");
        return false;
    }

    MIDDLEWARE_LOG_INFO("Successfully converted %s to %s", inputFilePath.c_str(), outputFilePath.c_str());
    return true;
}

    UsdProcessor::TextureData UsdProcessor::CreateTextureFromBuffer(const std::vector<uint8_t>& buffer) {
    MIDDLEWARE_LOG_INFO("Creating texture from buffer of size %zu", buffer.size());
    TextureData textureData;
    if (buffer.empty()) {
        MIDDLEWARE_LOG_ERROR("CreateTextureFromBuffer: Empty buffer");
        return textureData;
    }

    int width, height, channels;
    unsigned char* imageData = stbi_load_from_memory(
        buffer.data(), static_cast<int>(buffer.size()), &width, &height, &channels, 4);
    if (!imageData) {
        MIDDLEWARE_LOG_ERROR("Failed to decode image data: %s", stbi_failure_reason());
        return textureData;
    }

    // If the image is 2 pixels high, keep only the top row (gradient), discard the bottom (solid)
    if (height == 2) {
        textureData.data.resize(width * 4); // Only one row
        // Copy the first row (top row, gradient)
        memcpy(textureData.data.data(), imageData, width * 4);
        textureData.width = width;
        textureData.height = 1;
        textureData.channels = 4;
        stbi_image_free(imageData);
        MIDDLEWARE_LOG_INFO("Removed solid line: kept only the gradient row.");
        return textureData;
    }

    // Default: just copy the image as-is
    size_t dataSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    textureData.data.resize(dataSize);
    memcpy(textureData.data.data(), imageData, dataSize);
    stbi_image_free(imageData);
    textureData.width = width;
    textureData.height = height;
    textureData.channels = 4;
    return textureData;
}


bool UsdProcessor::LoadUSDBuffer(const std::vector<uint8_t>& buffer, const std::string& fileName,
                                 std::vector<MeshData>& outMeshData) {
    MIDDLEWARE_LOG_INFO("Loading USD from buffer, size: %zu, filename: %s", buffer.size(), fileName.c_str());

    std::vector<uint8_t> processedBuffer = pImpl->preprocessUsdContent(buffer);

    // Step 1: Determine if this is a USDZ file
    bool isUSDZ = (fileName.find(".usdz") != std::string::npos);
    if (isUSDZ) {
        MIDDLEWARE_LOG_INFO("Detected USDZ format file");
    }

    // Step 2: Load the USD file with composition enabled
    tinyusdz::Stage stage;
    std::string warnings, errors;
    tinyusdz::USDLoadOptions options;
    options.load_payloads = true;
    options.load_references = true;
    options.load_sublayers = true;
    options.max_memory_limit_in_mb = 4096;

    bool ret = tinyusdz::LoadUSDFromMemory(
        processedBuffer.data(), processedBuffer.size(), fileName.c_str(),
        &stage, &warnings, &errors, options
    );

    if (!ret) {
        MIDDLEWARE_LOG_ERROR("tinyusdz error: %s", errors.c_str());
        return false;
    }

    if (!warnings.empty()) {
        MIDDLEWARE_LOG_WARNING("tinyusdz warning: %s", warnings.c_str());
    }

    MIDDLEWARE_LOG_INFO("USD stage loaded successfully. Root prims: %zu", stage.root_prims().size());

    // Step 3: Process the main stage to extract meshes
    glm::mat4 identity(1.0f);
    for (const auto& rootPrim : stage.root_prims()) {
        ProcessPrim(const_cast<tinyusdz::Prim*>(&rootPrim), outMeshData, identity, 0);
    }

// Step 4: Check if we need to resolve references (no meshes OR meshes with no geometry data)
bool needsReferenceResolution = outMeshData.empty();
if (!needsReferenceResolution) {
    // Check if any mesh has actual geometry data
    for (const auto& mesh : outMeshData) {
        if (mesh.points.empty()) {
            needsReferenceResolution = true;
            break;
        }
    }
}

if (needsReferenceResolution) {
    MIDDLEWARE_LOG_INFO("Found meshes with no geometry data. Trying to resolve references, payloads, and clips.");

    // Get base directory of the USD file using TinyUSDZ's function
    std::string baseDir = tinyusdz::io::GetBaseDir(fileName);
    MIDDLEWARE_LOG_INFO("Base directory: %s", baseDir.c_str());
    if (baseDir.empty()) {
        // Extract directory from the full file path
        std::filesystem::path filePath(fileName);
        baseDir = filePath.parent_path().string();
        MIDDLEWARE_LOG_INFO("Using parent directory as base: %s", baseDir.c_str());

        if (baseDir.empty()) {
            // If still empty, use current working directory
            baseDir = std::filesystem::current_path().string();
            MIDDLEWARE_LOG_INFO("Using current directory as base: %s", baseDir.c_str());
        }
    }

    // Extract reference paths using existing method
    std::vector<std::string> referencePaths;
    ExtractReferencePaths(stage, referencePaths);

    // Also extract clips from raw content
    std::vector<std::string> clipPaths = ExtractClipsFromRawContent(processedBuffer);

    // Combine all paths
    referencePaths.insert(referencePaths.end(), clipPaths.begin(), clipPaths.end());

    MIDDLEWARE_LOG_INFO("Found %zu reference/clip paths to process", referencePaths.size());

    // Process all found paths first
    for (const auto& refPath : referencePaths) {
        std::string fullPath = baseDir + "/" + refPath;
        if (std::filesystem::exists(fullPath)) {
            MIDDLEWARE_LOG_INFO("Loading referenced/clipped file: %s", fullPath.c_str());

            // Load and process the referenced file
            std::ifstream file(fullPath, std::ios::binary);
            if (file) {
                std::vector<uint8_t> refBuffer(
                    (std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>()
                );

                size_t initialMeshCount = outMeshData.size();
                tinyusdz::Stage refStage;
                std::string refWarnings, refErrors;

                bool refRet = tinyusdz::LoadUSDFromMemory(
                    refBuffer.data(), refBuffer.size(), fullPath.c_str(),
                    &refStage, &refWarnings, &refErrors, options
                );

                if (refRet) {
                    for (const auto& refRootPrim : refStage.root_prims()) {
                        ProcessPrim(const_cast<tinyusdz::Prim*>(&refRootPrim),
                                   outMeshData, identity, 0);
                    }

                    size_t meshesAdded = outMeshData.size() - initialMeshCount;
                    if (meshesAdded > 0) {
                        MIDDLEWARE_LOG_INFO("Extracted %zu meshes from %s",
                                           meshesAdded, fullPath.c_str());
                    }
                } else {
                    MIDDLEWARE_LOG_WARNING("Failed to load referenced file: %s - %s",
                                          fullPath.c_str(), refErrors.c_str());
                }
            }
        } else {
            MIDDLEWARE_LOG_WARNING("Referenced file not found: %s", fullPath.c_str());
        }
    }

    // Set up asset resolver with appropriate search paths
    tinyusdz::AssetResolutionResolver resolver;
    resolver.set_current_working_path(baseDir);
    resolver.add_search_path(baseDir);

    // Add assets directory to search path
    std::string assetsDir = baseDir + "/assets";
    if (std::filesystem::exists(assetsDir)) {
        MIDDLEWARE_LOG_INFO("Adding search path: %s", assetsDir.c_str());
        resolver.add_search_path(assetsDir);

        // Enhanced file search to handle multiple USD file patterns
        std::vector<std::string> searchPatterns = {
            ".geom.usd",           // Traditional pattern
            ".usda",               // General USDA files
            "_Geom_",              // Clips pattern: k_actor__triangles_0_Geom_0.000000.usda
            "_Geom.usda",          // Alternative clips pattern
            "_Material.usda",      // Material clips
            "_Sampler.usda",       // Sampler clips
            "_Camera.usda"         // Camera clips
        };

        try {
            // Recursively search for USD files with various patterns
            for (const auto& entry : std::filesystem::recursive_directory_iterator(baseDir)) {
                if (entry.is_regular_file()) {
                    std::string filePath = entry.path().string();
                    std::string fileName = entry.path().filename().string();

                    // Check against all patterns
                    bool matchesPattern = false;
                    std::string matchedPattern;

                    for (const auto& pattern : searchPatterns) {
                        if (filePath.find(pattern) != std::string::npos) {
                            matchesPattern = true;
                            matchedPattern = pattern;
                            break;
                        }
                    }

                    if (matchesPattern) {
                        MIDDLEWARE_LOG_INFO("Found geometry file (%s): %s",
                                           matchedPattern.c_str(), filePath.c_str());

                        // Load and process the file
                        std::ifstream file(filePath, std::ios::binary);
                        if (file) {
                            std::vector<uint8_t> geomBuffer(
                                (std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>()
                            );

                            size_t initialMeshCount = outMeshData.size();
                            tinyusdz::Stage geomStage;
                            std::string geomWarnings, geomErrors;

                            bool geomRet = tinyusdz::LoadUSDFromMemory(
                                geomBuffer.data(), geomBuffer.size(), filePath.c_str(),
                                &geomStage, &geomWarnings, &geomErrors, options
                            );

                            if (geomRet) {
                                for (const auto& geomRootPrim : geomStage.root_prims()) {
                                    ProcessPrim(const_cast<tinyusdz::Prim*>(&geomRootPrim),
                                               outMeshData, identity, 0);
                                }

                                size_t meshesAdded = outMeshData.size() - initialMeshCount;
                                if (meshesAdded > 0) {
                                    MIDDLEWARE_LOG_INFO("Extracted %zu meshes from %s",
                                                       meshesAdded, filePath.c_str());
                                }
                            } else {
                                MIDDLEWARE_LOG_WARNING("Failed to load geometry file: %s - %s",
                                                      filePath.c_str(), geomErrors.c_str());
                            }
                        } else {
                            MIDDLEWARE_LOG_WARNING("Failed to open file: %s", filePath.c_str());
                        }
                    }
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            MIDDLEWARE_LOG_ERROR("Filesystem error during file search: %s", e.what());
        }
    } else {
        MIDDLEWARE_LOG_WARNING("Assets directory not found: %s", assetsDir.c_str());
    }
}

    // If we still have no meshes and this is a USDZ file, try to extract more information
    if (outMeshData.empty() && isUSDZ) {
        MIDDLEWARE_LOG_INFO("No meshes found in USDZ file. Trying to extract more information.");

        // For USDZ files, we can try to get more information about the package contents
        // This is mostly for debugging purposes

        // Instead of trying to access stage.name directly, just log information about the stage
        MIDDLEWARE_LOG_INFO("USDZ package information:");
        MIDDLEWARE_LOG_INFO("  Root prims: %zu", stage.root_prims().size());

        // List all prims in the stage for debugging
        MIDDLEWARE_LOG_INFO("USDZ prims:");
        for (const auto& rootPrim : stage.root_prims()) {
            ListPrimHierarchy(rootPrim, 0);
        }
    }

    MIDDLEWARE_LOG_INFO("Processed %zu meshes from USD", outMeshData.size());
    return true;
}

// Helper function to list the prim hierarchy for debugging
void UsdProcessor::ListPrimHierarchy(const tinyusdz::Prim& prim, int depth) {
    std::string indent(depth * 2, ' ');
    MIDDLEWARE_LOG_INFO("%s- %s (%s)", indent.c_str(),
                      prim.element_name().c_str(),
                      prim.prim_type_name().c_str());

    for (const auto& child : prim.children()) {
        ListPrimHierarchy(child, depth + 1);
    }
}


// Helper function to extract reference paths from a USD stage
    void UsdProcessor::ExtractReferencePaths(const tinyusdz::Stage& stage, std::vector<std::string>& outReferencePaths) {
    MIDDLEWARE_LOG_INFO("Extracting reference paths from stage");

    // Process each root prim
    for (const auto& rootPrim : stage.root_prims()) {
        ExtractReferencePathsFromPrim(rootPrim, outReferencePaths);
    }
}

    // Add this new method to extract clips from raw USD content
    std::vector<std::string> UsdProcessor::ExtractClipsFromRawContent(const std::vector<uint8_t>& buffer) {
    std::vector<std::string> clipPaths;

    // Convert buffer to string for parsing
    std::string content(reinterpret_cast<const char*>(buffer.data()), buffer.size());

    // Look for clips patterns in the raw USD content
    std::regex clipsPattern(R"(asset\[\]\s+assetPaths\s*=\s*\[@([^@]+)@\])");

    std::sregex_iterator iter(content.begin(), content.end(), clipsPattern);
    std::sregex_iterator end;

    while (iter != end) {
        std::string clipPath = (*iter)[1].str();
        MIDDLEWARE_LOG_INFO("Found clip asset path: %s", clipPath.c_str());
        clipPaths.push_back(clipPath);
        ++iter;
    }

    return clipPaths;
}


    void UsdProcessor::ExtractReferencePathsFromPrim(const tinyusdz::Prim& prim, std::vector<std::string>& outReferencePaths) {
    // Check for references in this prim
    if (prim.metas().references.has_value()) {
        const auto& refs = prim.metas().references.value();

        // Access the references vector (second element of the pair)
        const auto& references = refs.second;

        // For each reference in the vector
        for (const auto& ref : references) {
            std::string assetPath = ref.asset_path.GetAssetPath();
            if (!assetPath.empty()) {
                MIDDLEWARE_LOG_INFO("Found reference: %s", assetPath.c_str());
                outReferencePaths.push_back(assetPath);
            }
        }
    }

    // Check for payloads in this prim
    if (prim.metas().payload.has_value()) {
        const auto& pl = prim.metas().payload.value();

        // Access the payloads vector (second element of the pair)
        const auto& payloads = pl.second;

        // For each payload in the vector
        for (const auto& payload : payloads) {
            std::string assetPath = payload.asset_path.GetAssetPath();
            if (!assetPath.empty()) {
                MIDDLEWARE_LOG_INFO("Found payload: %s", assetPath.c_str());
                outReferencePaths.push_back(assetPath);
            }
        }
    }

    // Recursively check child prims
    for (const auto& child : prim.children()) {
        ExtractReferencePathsFromPrim(child, outReferencePaths);
    }
}


void UsdProcessor::ProcessPrim(void* primPtr, std::vector<MeshData>& meshDataArray,
                              const glm::mat4& parentTransform, int32_t depth) {
    const tinyusdz::Prim& prim = *static_cast<const tinyusdz::Prim*>(primPtr);

    MIDDLEWARE_LOG_DEBUG("Processing prim: %s (type: %s, depth: %d)",
                        prim.element_name().data(), prim.type_name().data(), depth);

    // Get local transform and compute world transform
    glm::mat4 localTransform = GetLocalTransform(primPtr);
    glm::mat4 worldTransform = parentTransform * localTransform; // Fixed multiplication order

    // Check if this is a mesh
    if (const auto* mesh = prim.as<tinyusdz::GeomMesh>()) {
        if (DEBUG_LEVEL >= 1) {
            MIDDLEWARE_LOG_DEBUG("Found mesh: %s", prim.element_name().data());
        }

        MeshData meshData;
        meshData.elementName = prim.element_name().data();
        meshData.typeName = prim.type_name().data();

        ExtractMeshData(const_cast<tinyusdz::GeomMesh*>(mesh), meshData, worldTransform);
        meshDataArray.push_back(meshData);
    }

    // Process children recursively
    for (const auto& child : prim.children()) {
        ProcessPrim(const_cast<tinyusdz::Prim*>(&child), meshDataArray, worldTransform, depth + 1);
    }
}

    void UsdProcessor::ExtractMeshData(void* meshPtr, MeshData& outMeshData,
                                      const glm::mat4& worldTransform) {
    tinyusdz::GeomMesh* mesh = static_cast<tinyusdz::GeomMesh*>(meshPtr);
    if (!mesh) {
        MIDDLEWARE_LOG_WARNING("ExtractMeshData: Null mesh pointer");
        return;
    }

    // Process points
    auto points = mesh->get_points();
    MIDDLEWARE_LOG_INFO("Extracting mesh with %zu points", points.size());

    if (points.empty()) {
        MIDDLEWARE_LOG_WARNING("Mesh has no points");
        return;
    }

    outMeshData.points.reserve(points.size());

    // Print first few points for debugging
    if (DEBUG_LEVEL >= 2) {
        for (size_t i = 0; i < std::min(points.size(), size_t(5)); ++i) {
            MIDDLEWARE_LOG_VERBOSE("Point[%zu]: (%f, %f, %f)", i, points[i].x, points[i].y, points[i].z);
        }
    }

    for (const auto& pt : points) {
        glm::vec3 vertex(pt.x, pt.y, pt.z);
        glm::vec4 transformedVertex = worldTransform * glm::vec4(vertex, 1.0f);
        outMeshData.points.push_back(glm::vec3(transformedVertex));
    }

    // Process face vertex indices and counts
    auto faceVertexCounts = mesh->get_faceVertexCounts();
    auto faceVertexIndices = mesh->get_faceVertexIndices();

    if (DEBUG_LEVEL >= 1) {
        MIDDLEWARE_LOG_DEBUG("Triangulating %zu faces", faceVertexCounts.size());
    }

    // Triangulate faces - fixed to use uint32_t to match MeshData.indices type
    std::vector<uint32_t> triangulatedIndices;
    size_t indexOffset = 0;

    for (size_t faceIdx = 0; faceIdx < faceVertexCounts.size(); faceIdx++) {
        int32_t numVertsInFace = faceVertexCounts[faceIdx];

        if (numVertsInFace >= 3) {
            for (int32_t triIdx = 0; triIdx < numVertsInFace - 2; triIdx++) {
                triangulatedIndices.push_back(static_cast<uint32_t>(faceVertexIndices[indexOffset]));
                triangulatedIndices.push_back(static_cast<uint32_t>(faceVertexIndices[indexOffset + triIdx + 1]));
                triangulatedIndices.push_back(static_cast<uint32_t>(faceVertexIndices[indexOffset + triIdx + 2]));
            }
        }

        indexOffset += numVertsInFace;
    }

    if (DEBUG_LEVEL >= 1) {
        MIDDLEWARE_LOG_DEBUG("Generated %zu triangle indices", triangulatedIndices.size());
    }

    outMeshData.indices = std::move(triangulatedIndices);

    // Process normals
    auto normals = mesh->get_normals();
    if (DEBUG_LEVEL >= 1) {
        MIDDLEWARE_LOG_DEBUG("Extracting %zu normals", normals.size());
    }

    outMeshData.normals.reserve(normals.size());

    // Print first few normals for debugging
    if (DEBUG_LEVEL >= 2) {
        for (size_t i = 0; i < std::min(normals.size(), size_t(5)); ++i) {
            MIDDLEWARE_LOG_VERBOSE("Normal[%zu]: (%f, %f, %f)", i, normals[i].x, normals[i].y, normals[i].z);
        }
    }

    for (const auto& nrm : normals) {
        glm::vec3 normalVec(nrm.x, nrm.y, nrm.z);
        glm::mat3 normalMatrix = glm::mat3(worldTransform);
        glm::vec3 transformedNormal = normalMatrix * normalVec;
        transformedNormal = glm::normalize(transformedNormal);
        outMeshData.normals.push_back(transformedNormal);
    }

    // Process UVs
    tinyusdz::GeomPrimvar primvar;
    std::string primvarErr;
    bool foundUVs = false;

    const char* possibleUVNames[] = {
        "primvars:attribute0", "attribute0", "st", "primvars:st", "primvars:uv", "uv"
    };

    // Track UV bounds for debugging - fixed numeric limits usage
    float minU = std::numeric_limits<float>::max();
    float minV = std::numeric_limits<float>::max();
    float maxU = -std::numeric_limits<float>::max();
    float maxV = -std::numeric_limits<float>::max();

    for (const char* name : possibleUVNames) {
        if (DEBUG_LEVEL >= 1) {
            MIDDLEWARE_LOG_DEBUG("Trying to find UVs in primvar: %s", name);
        }

        if (mesh->get_primvar(name, &primvar, &primvarErr)) {
            if (DEBUG_LEVEL >= 1) {
                MIDDLEWARE_LOG_DEBUG("Found UVs in primvar: %s", name);
            }

            std::vector<tinyusdz::value::texcoord2f> uvs;
            if (primvar.get_value(&uvs)) {
                if (DEBUG_LEVEL >= 1) {
                    MIDDLEWARE_LOG_DEBUG("Extracting %zu UVs", uvs.size());
                }

                outMeshData.uvs.reserve(uvs.size());

                // Print first few UVs for debugging
                if (DEBUG_LEVEL >= 2) {
                    for (size_t i = 0; i < std::min(uvs.size(), size_t(5)); ++i) {
                        MIDDLEWARE_LOG_VERBOSE("UV[%zu]: (%f, %f)", i, uvs[i].s, uvs[i].t);
                    }
                }

                for (const auto& uv : uvs) {
                    float u = uv.s;
                    float v = uv.t;

                    // Update bounds
                    minU = std::min(minU, u);
                    minV = std::min(minV, v);
                    maxU = std::max(maxU, u);
                    maxV = std::max(maxV, v);

                    outMeshData.uvs.push_back(glm::vec2(u, v));
                }

                if (DEBUG_LEVEL >= 1) {
                    MIDDLEWARE_LOG_DEBUG("UV Bounds: Min(%f, %f), Max(%f, %f)", minU, minV, maxU, maxV);
                }

                foundUVs = true;
                break;
            }
        }
    }

    if (!foundUVs && DEBUG_LEVEL >= 1) {
        MIDDLEWARE_LOG_DEBUG("No texture coordinates found in any expected primvar");
    }
}

glm::mat4 UsdProcessor::GetLocalTransform(void* primPtr) {
    const tinyusdz::Prim& prim = *static_cast<const tinyusdz::Prim*>(primPtr);
    glm::mat4 localTransform(1.0f); // Identity

    const tinyusdz::Xformable* xformable = nullptr;
    if (!tinyusdz::CastToXformable(prim, &xformable)) {
        return localTransform;
    }

    MIDDLEWARE_LOG_DEBUG("Processing transforms for prim: %s (%zu ops)",
                        prim.element_name().data(), xformable->xformOps.size());

    for (const auto& op : xformable->xformOps) {
        if (op.op_type == tinyusdz::XformOp::OpType::Translate) {
            tinyusdz::value::double3 trans;
            if (op.get_interpolated_value(&trans)) {
                MIDDLEWARE_LOG_DEBUG("Translate: (%f, %f, %f)", trans[0], trans[1], trans[2]);
                localTransform = glm::translate(localTransform,
                    glm::vec3(static_cast<float>(trans[0]), static_cast<float>(trans[1]), static_cast<float>(trans[2])));
            }
        }
        else if (op.op_type == tinyusdz::XformOp::OpType::Scale) {
            tinyusdz::value::double3 scale;
            if (op.get_interpolated_value(&scale)) {
                MIDDLEWARE_LOG_DEBUG("Scale: (%f, %f, %f)", scale[0], scale[1], scale[2]);
                localTransform = glm::scale(localTransform,
                    glm::vec3(static_cast<float>(scale[0]), static_cast<float>(scale[1]), static_cast<float>(scale[2])));
            }
        }
        else if (op.op_type == tinyusdz::XformOp::OpType::RotateXYZ) {
            tinyusdz::value::double3 rot;
            if (op.get_interpolated_value(&rot)) {
                MIDDLEWARE_LOG_DEBUG("Rotate XYZ: (%f, %f, %f)", rot[0], rot[1], rot[2]);
                localTransform = glm::rotate(localTransform,
                    glm::radians(static_cast<float>(rot[0])), glm::vec3(1.0f, 0.0f, 0.0f));
                localTransform = glm::rotate(localTransform,
                    glm::radians(static_cast<float>(rot[1])), glm::vec3(0.0f, 1.0f, 0.0f));
                localTransform = glm::rotate(localTransform,
                    glm::radians(static_cast<float>(rot[2])), glm::vec3(0.0f, 0.0f, 1.0f));
            }
        }
    }

    return localTransform;
}

} 
