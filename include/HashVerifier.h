#pragma once

#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include <mutex>
#include <cstdint>
#include "MiddlewareLogging.h"

namespace anari_usd_middleware {

/**
 * XXH3-128 hash helper. Replaces the old SHA256/OpenSSL implementation.
 * Thread-safe: XXH3 requires no mutex.
 */
class HashVerifier {
public:
    /**
     * Compute XXH3-128 hash of a data buffer.
     * Returns {hash_lo, hash_hi} as 64-bit integers.
     */
    static std::pair<uint64_t, uint64_t> calculateHash128(const std::vector<uint8_t>& data);

    /**
     * Verify hash of data against expected {lo, hi} pair.
     */
    static bool verifyHash128(const std::vector<uint8_t>& data, uint64_t expectedLo, uint64_t expectedHi);

    /**
     * Validate input data - basic bounds check.
     */
    static bool validateInputData(const std::vector<uint8_t>& data);

private:
    HashVerifier() = delete;
    ~HashVerifier() = delete;
    HashVerifier(const HashVerifier&) = delete;
    HashVerifier& operator=(const HashVerifier&) = delete;
};

} // namespace anari_usd_middleware
