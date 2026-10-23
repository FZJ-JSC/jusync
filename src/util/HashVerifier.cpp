#include "HashVerifier.h"
#include "MiddlewareLogging.h"
#include "xxhash.h"
#include <iomanip>
#include <chrono>

namespace anari_usd_middleware {

std::pair<uint64_t, uint64_t> HashVerifier::calculateHash128(const std::vector<uint8_t>& data) {
    MIDDLEWARE_LOG_DEBUG("XXH3-128 hash for %zu bytes", data.size());
    if (!validateInputData(data)) return {0, 0};
    XXH128_hash_t h = XXH3_128bits(data.data(), data.size());
    MIDDLEWARE_LOG_DEBUG("XXH3-128: lo=0x%016lx hi=0x%016lx",
                         (unsigned long)h.low64, (unsigned long)h.high64);
    return {h.low64, h.high64};
}

bool HashVerifier::verifyHash128(const std::vector<uint8_t>& data, uint64_t expectedLo, uint64_t expectedHi) {
    if (!validateInputData(data)) return false;
    XXH128_hash_t h = XXH3_128bits(data.data(), data.size());
    bool result = (h.low64 == expectedLo && h.high64 == expectedHi);
    if (!result) {
        MIDDLEWARE_LOG_WARNING("XXH3-128 mismatch: expected 0x%016lx:0x%016lx got 0x%016lx:0x%016lx",
                               (unsigned long)expectedLo, (unsigned long)expectedHi,
                               (unsigned long)h.low64, (unsigned long)h.high64);
    }
    return result;
}

bool HashVerifier::validateInputData(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        MIDDLEWARE_LOG_ERROR("HashVerifier: empty data buffer");
        return false;
    }
    return true;
}

} // namespace anari_usd_middleware
