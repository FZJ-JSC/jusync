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

class HashVerifier {
public:
    static std::pair<uint64_t, uint64_t> calculateHash128(const std::vector<uint8_t>& data);

    static bool verifyHash128(const std::vector<uint8_t>& data, uint64_t expectedLo, uint64_t expectedHi);

    static bool validateInputData(const std::vector<uint8_t>& data);

private:
    HashVerifier() = delete;
    ~HashVerifier() = delete;
    HashVerifier(const HashVerifier&) = delete;
    HashVerifier& operator=(const HashVerifier&) = delete;
};

} // namespace anari_usd_middleware
