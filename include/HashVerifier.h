#pragma once

#include <string>
#include <vector>
#include <openssl/evp.h>

namespace anari_usd_middleware {

    /**
     * Utility class for verifying SHA256 hashes.
     */
    class HashVerifier {
    public:
        static bool verifyHash(const std::vector<uint8_t>& data, const std::string& expectedHash);
        static std::string calculateHash(const std::vector<uint8_t>& data);
    };

}
