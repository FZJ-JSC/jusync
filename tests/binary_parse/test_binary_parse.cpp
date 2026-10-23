// Phase 1 validation: binary .usdc parse through the real client path.
//
// Feeds the tinyusdz-bundled REAL binary crate files through the exact
// QueryUSDFullLayout_C -> FillUSDFullCompact_C sequence the UE plugin uses
// (content-magic detection must route them past the ASCII-only transforms),
// plus an ASCII twin as baseline. Also exercises the Phase 0 JusyncDiag* C
// API end to end.
//
// Build (from a VS dev prompt, after the middleware is built):
//   cl /nologo /EHsc /std:c++17 /I ..\..\include test_binary_parse.cpp ^
//      ..\..\build-diag\Release\anari_usd_middleware.lib /Fe:test_binary_parse.exe
// Run with anari_usd_middleware.dll next to the exe or on PATH.

#include "AnariUsdMiddleware_C.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

static std::vector<unsigned char> ReadFile(const char* path)
{
    FILE* f = nullptr;
#ifdef _WIN32
    fopen_s(&f, path, "rb");
#else
    f = fopen(path, "rb");
#endif
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> data(static_cast<size_t>(sz));
    const size_t rd = fread(data.data(), 1, data.size(), f);
    fclose(f);
    data.resize(rd);
    return data;
}

static int g_fails = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) { ++g_fails; printf("  FAIL: "); printf(__VA_ARGS__);  \
                       printf("\n"); }                                      \
        else         { printf("  ok:   "); printf(__VA_ARGS__);             \
                       printf("\n"); }                                      \
    } while (0)

// Query + fill one USD buffer through the production client path.
// Returns total vertex count over all meshes/clouds, -1 on parse failure.
static long ParseAndFill(const std::vector<unsigned char>& data,
                         const char* label,
                         bool expectFiniteCube)
{
    void* handle = nullptr;
    CMeshLayout* ml = nullptr;
    CPointCloudLayout* cl = nullptr;
    size_t mc = 0, cc = 0;

    const int ok = QueryUSDFullLayout_C(data.data(), data.size(), label, &handle,
                                        &ml, &mc, &cl, &cc);
    if (!ok) return -1;

    printf("  queried '%s': %zu meshes, %zu clouds\n", label, mc, cc);

    long totalVerts = 0;
    std::vector<CMeshDataFillCompact> fills(mc);
    std::vector<std::vector<float>> pts(mc), nrm(mc), uvs(mc);
    std::vector<std::vector<int32_t>> idx(mc);
    std::vector<std::vector<unsigned char>> col(mc);

    for (size_t i = 0; i < mc; ++i) {
        pts[i].resize(ml[i].points_count * 3);
        idx[i].resize(ml[i].indices_count ? ml[i].indices_count : 1);
        nrm[i].resize(ml[i].normals_count * 3);
        uvs[i].resize(ml[i].uvs_count * 2);
        col[i].resize(ml[i].vertex_colors_count * 4);
        fills[i].points  = pts[i].data();
        fills[i].indices = idx[i].data();
        fills[i].normals = ml[i].normals_count ? nrm[i].data() : nullptr;
        fills[i].uvs     = ml[i].uvs_count ? uvs[i].data() : nullptr;
        fills[i].vertex_colors8 = ml[i].vertex_colors_count ? col[i].data() : nullptr;
        totalVerts += static_cast<long>(ml[i].points_count);
    }

    std::vector<CPointCloudDataFill> cfills(cc);
    std::vector<std::vector<double>> cpos(cc);
    std::vector<std::vector<unsigned char>> ccol(cc);
    for (size_t i = 0; i < cc; ++i) {
        cpos[i].resize(cl[i].point_count * 3);
        ccol[i].resize(cl[i].has_colors ? cl[i].point_count * 4 : 0);
        cfills[i].positions = cpos[i].data();
        cfills[i].colors8   = cl[i].has_colors ? ccol[i].data() : nullptr;
        cfills[i].widths    = nullptr;
        cfills[i].lidar_points = nullptr;
        totalVerts += static_cast<long>(cl[i].point_count);
    }

    const int filled = FillUSDFullCompact_C(handle, mc ? fills.data() : nullptr, mc,
                                            cc ? cfills.data() : nullptr, cc);
    CHECK(filled == 1, "FillUSDFullCompact_C('%s') returned %d", label, filled);

    if (expectFiniteCube && filled == 1 && mc > 0) {
        bool finite = true;
        for (size_t i = 0; i < mc && finite; ++i) {
            for (float v : pts[i]) {
                if (!std::isfinite(v)) { finite = false; break; }
            }
        }
        CHECK(finite, "filled points are finite for '%s'", label);
    }

    FreeUSDFullLayouts_C(ml, mc, cl, cc);
    FreeParsedUSD_C(handle);
    return totalVerts;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    // Find tinyusdz models relative to the jusync repo root.
    if (argc > 1) { SetCurrentDirectoryA(argv[1]); }
#endif
    const char* crate   = "external/tinyusdz/models/cube.usdc";
    const char* crate2  = "external/tinyusdz/models/suzanne.usdc";

    printf("=== P1 binary .usdc validation ===\n");

    // --- diag: unknown file baseline
    CHECK(JusyncDiagTotalMs_C("nothing/here") < 0, "unknown file totalMs < 0");

    // --- binary crate via the production query/fill path
    auto bin = ReadFile(crate);
    CHECK(!bin.empty() && bin.size() >= 8 &&
          memcmp(bin.data(), "PXR-USDC", 8) == 0, "cube.usdc loaded with PXR-USDC magic (%zu bytes)",
          bin.size());
    const long binVerts = ParseAndFill(bin, crate, true);
    CHECK(binVerts >= 4, "binary cube parsed %ld verts (>= 4)", binVerts);

    // --- second, bigger binary crate (suzanne)
    auto suz = ReadFile(crate2);
    CHECK(!suz.empty() && memcmp(suz.data(), "PXR-USDC", 8) == 0, "suzanne.usdc magic ok");
    const long suzVerts = ParseAndFill(suz, crate2, true);
    CHECK(suzVerts > binVerts, "suzanne crate has more verts than cube (%ld > %ld)",
          suzVerts, binVerts);

    // --- ASCII baseline: embedded known-good .usda (same fixture the
    // tinyusdz's own ascii models use primvar
    // syntax this lightusd fork rejects, so they are not valid baselines).
    static const std::string asciiCube = R"(#usda 1.0
def Mesh "TestCube"
{
    float3[] extent = [(-1, -1, -1), (1, 1, 1)]
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
    point3f[] points = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)]
}
)";
    std::vector<unsigned char> txt(asciiCube.begin(), asciiCube.end());
    CHECK(!txt.empty(), "ascii fixture built (%zu bytes)", txt.size());
    const long txtVerts = ParseAndFill(txt, "cube_ascii.usda", true);
    CHECK(txtVerts >= 4, "ascii cube parsed %ld verts (>= 4)", txtVerts);

    // --- Phase 0 C-API journal worked through this whole flow
    char table[8192];
    const int n = JusyncDiagDump_C(table, sizeof(table), 10);
    printf("--- JusyncDiagDump_C (%d bytes) ---\n%s", n, n > 0 ? table : "(empty)\n");
    CHECK(n > 0 && strstr(table, "cube.usdc") != nullptr,
          "journal table contains cube.usdc");
    CHECK(JusyncDiagTotalMs_C(crate) >= 0, "totalMs(cube.usdc) recorded by parse stages");

    printf(g_fails == 0 ? "\nP1 BINARY PARSE TEST: PASS\n"
                        : "\nP1 BINARY PARSE TEST: FAIL (%d)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
