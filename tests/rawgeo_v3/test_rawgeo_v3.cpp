// RawGeo wire-format test: v1 and v2 stay readable, v3 adds curve topology.
//
// The structs are packed, so the sizes and section arithmetic below are the
// contract the sender (ANARI-USD) and the UE client both depend on.

#include "AnariUsdProtocol.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace anari_usd_protocol;

static int g_fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Layout exactly as the spec documents it (no attrs, colors=3, optional widths).
static std::vector<uint8_t> BuildV3(uint32_t topology, uint64_t numPoints,
                                    uint64_t curveCount, uint64_t fullCurveCount,
                                    uint64_t subsetIndex, uint64_t subsetCount,
                                    uint32_t widthComponents,
                                    const std::vector<uint32_t>& curveCounts)
{
    JusyncRawGeoHeader h{};
    h.magic = JUSYNC_RAWGEO_MAGIC;
    h.version = JUSYNC_RAWGEO_VERSION;
    std::strncpy(h.prim_name, "Streamlines", sizeof(h.prim_name) - 1);
    h.num_points = numPoints;
    h.time = 1.5;
    h.position_components = 3;
    h.color_components = 3;
    h.num_attributes = 0;
    h.attr_uniform_count = (uint32_t)numPoints;
    h.subset_index = subsetIndex;
    h.subset_count = subsetCount;
    h.full_num_points = numPoints;
    h.topology = topology;
    h.curve_count = (uint32_t)curveCount;
    h.full_curve_count = fullCurveCount;
    h.width_components = widthComponents;
    h.flags = 0;
    h.payload_bytes = numPoints * 3 * 4 + numPoints * 3 * 4
                    + curveCounts.size() * 4 + numPoints * widthComponents * 4;
    std::vector<uint8_t> b(sizeof(h) + (size_t)h.payload_bytes, 0);
    std::memcpy(b.data(), &h, sizeof(h));
    uint8_t* p = b.data() + sizeof(h);
    for (uint64_t i = 0; i < numPoints * 3; ++i) { float v = (float)i; std::memcpy(p, &v, 4); p += 4; }
    p += numPoints * 3 * 4; // colors stay zero
    if (!curveCounts.empty())
    {
        std::memcpy(p, curveCounts.data(), curveCounts.size() * 4);
        p += curveCounts.size() * 4;
    }
    for (uint64_t i = 0; i < numPoints * widthComponents; ++i) { float v = 1.0f; std::memcpy(p, &v, 4); p += 4; }
    return b;
}

static JusyncRawGeoHeader ReadHeader(const std::vector<uint8_t>& b)
{
    JusyncRawGeoHeader h{};
    std::memcpy(&h, b.data(), std::min(b.size(), sizeof(h)));
    return h;
}

int main()
{
    CHECK(sizeof(JusyncRawGeoHeaderV1) == 112, "v1 size");
    CHECK(sizeof(JusyncRawGeoHeaderV2) == 136, "v2 size");
    CHECK(sizeof(JusyncRawGeoHeader) == 160, "v3 size");
    CHECK(sizeof(JusyncRawGeoAttrDesc) == 40, "attr size");
    CHECK(JusyncRawGeoHeader::header_bytes(1) == 112, "header_bytes(1)");
    CHECK(JusyncRawGeoHeader::header_bytes(2) == 136, "header_bytes(2)");
    CHECK(JusyncRawGeoHeader::header_bytes(3) == 160, "header_bytes(3)");
    CHECK(JUSYNC_RAWGEO_VERSION == 3, "current version");
    CHECK(JUSYNC_RAWGEO_TOPOLOGY_POINTS == 0, "points topology");
    CHECK(JUSYNC_RAWGEO_TOPOLOGY_LINEAR_CURVES == 1, "curves topology");

    // v1 payloads stay valid byte-for-byte.
    {
        JusyncRawGeoHeaderV1 h{};
        h.magic = JUSYNC_RAWGEO_MAGIC; h.version = 1;
        std::strncpy(h.prim_name, "p", sizeof(h.prim_name) - 1);
        h.num_points = 2; h.position_components = 3;
        h.attr_uniform_count = 2; h.payload_bytes = 24;
        std::vector<uint8_t> b(sizeof(h) + 24, 0);
        std::memcpy(b.data(), &h, sizeof(h));
        JusyncRawGeoHeader v3{};
        std::memcpy(&v3, b.data(), sizeof(h));
        CHECK(v3.valid(b.data(), b.size()), "v1 payload valid");
    }

    // v2 payloads stay valid byte-for-byte.
    {
        JusyncRawGeoHeaderV2 h{};
        h.magic = JUSYNC_RAWGEO_MAGIC; h.version = 2;
        std::strncpy(h.prim_name, "p", sizeof(h.prim_name) - 1);
        h.num_points = 2; h.position_components = 3; h.attr_uniform_count = 2;
        h.payload_bytes = 24; h.subset_index = 1; h.subset_count = 4; h.full_num_points = 8;
        std::vector<uint8_t> b(sizeof(h) + 24, 0);
        std::memcpy(b.data(), &h, sizeof(h));
        JusyncRawGeoHeader v3{};
        std::memcpy(&v3, b.data(), sizeof(h));
        CHECK(v3.valid(b.data(), b.size()), "v2 payload valid");
        v3.subset_index = 4;
        CHECK(!v3.valid(b.data(), b.size()), "v2 subset_index bound");
    }

    // v3 flat points
    { auto b = BuildV3(0, 4, 0, 0, 0, 1, 0, {}); CHECK(ReadHeader(b).valid(b.data(), b.size()), "v3 points"); }
    // v3 curves, 2 of 10, subset 2 of 4, widths on
    { auto b = BuildV3(1, 4, 2, 10, 2, 4, 1, {2, 2}); CHECK(ReadHeader(b).valid(b.data(), b.size()), "v3 curves"); }
    // empty subset
    { auto b = BuildV3(1, 0, 0, 10, 3, 4, 0, {}); CHECK(ReadHeader(b).valid(b.data(), b.size()), "empty subset"); }

    auto reject = [](std::vector<uint8_t> b) { return !ReadHeader(b).valid(b.data(), b.size()); };
    { auto b = BuildV3(0, 4, 0, 0, 0, 1, 0, {}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->magic = 0; CHECK(reject(b), "bad magic"); }
    { auto b = BuildV3(0, 4, 0, 0, 0, 1, 0, {}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->version = 4; CHECK(reject(b), "bad version"); }
    { auto b = BuildV3(0, 4, 0, 0, 0, 1, 0, {}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->flags = 1; CHECK(reject(b), "bad flags"); }
    { auto b = BuildV3(0, 4, 0, 0, 0, 1, 0, {}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->topology = 2; CHECK(reject(b), "bad topology"); }
    { auto b = BuildV3(0, 4, 0, 0, 0, 1, 0, {}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->curve_count = 1; CHECK(reject(b), "points with curves"); }
    { auto b = BuildV3(1, 4, 2, 1, 0, 1, 0, {2, 2}); CHECK(reject(b), "curve_count > full_curve_count"); }
    { auto b = BuildV3(1, 2, 2, 10, 0, 4, 0, {1, 1}); CHECK(reject(b), "num_points < 2*curve_count"); }
    { auto b = BuildV3(1, 4, 2, 10, 0, 4, 1, {2, 2}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->width_components = 2; CHECK(reject(b), "bad width_components"); }
    { auto b = BuildV3(1, 4, 2, 10, 0, 4, 0, {2, 2}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->payload_bytes += 4; CHECK(reject(b), "payload_bytes mismatch"); }
    { auto b = BuildV3(1, 4, 2, 10, 0, 4, 0, {2, 2}); b.resize(b.size() - 4); CHECK(reject(b), "truncated"); }
    { auto b = BuildV3(1, 4, 2, 10, 0, 4, 0, {2, 2}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->subset_index = 4; CHECK(reject(b), "subset_index >= subset_count"); }
    { auto b = BuildV3(1, 4, 2, 10, 0, 4, 0, {2, 2}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->subset_count = 0; CHECK(reject(b), "subset_count == 0"); }
    { auto b = BuildV3(1, 4, 2, 10, 0, 4, 0, {2, 2}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->full_num_points = 3; CHECK(reject(b), "full_num_points < num_points"); }
    { auto b = BuildV3(1, 4, 2, 10, 0, 4, 0, {2, 2}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->attr_uniform_count = 3; CHECK(reject(b), "attr_uniform_count != num_points"); }

    if (g_fails) { printf("FAILED %d\n", g_fails); return 1; }
    printf("OK rawgeo_v3\n");
    return 0;
}
