#include "HashVerifier.h"
#include "MiddlewareLogging.h"

#include <openssl/evp.h>
#include <openssl/err.h>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <chrono>

namespace anari_usd_middleware {

// Static member initialization
std::mutex HashVerifier::opensslMutex;

bool HashVerifier::verifyHash(const std::vector<uint8_t>& data, const std::string& expectedHash) {
    MIDDLEWARE_LOG_DEBUG("Verifying hash for data of size %zu bytes", data.size());

    // Input validation
    if (!validateInputData(data, "verifyHash")) {
        return false;
    }

    if (!validateHashString(expectedHash, "verifyHash")) {
        return false;
    }

    try {
        // Calculate hash with safety measures
        std::string computedHash = calculateHash(data);
        if (computedHash.empty()) {
            MIDDLEWARE_LOG_ERROR("Failed to compute hash for verification");
            return false;
        }

        // Safe comparison
        bool result = compareHashes(computedHash, expectedHash);

        if (result) {
            MIDDLEWARE_LOG_DEBUG("Hash verification successful");
        } else {
            MIDDLEWARE_LOG_WARNING("Hash verification failed: expected=%s, computed=%s",
                                 expectedHash.c_str(), computedHash.c_str());
        }

        return result;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in verifyHash: %s", e.what());
        return false;
    }
}

std::string HashVerifier::calculateHash(const std::vector<uint8_t>& data) {
    MIDDLEWARE_LOG_DEBUG("Calculating hash for data of size %zu bytes", data.size());

    // Input validation
    if (!validateInputData(data, "calculateHash")) {
        return "";
    }

    try {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hashLen = 0;

        // Thread-safe hash calculation
        std::lock_guard<std::mutex> lock(opensslMutex);

        if (!performHashOperation(data, hash, &hashLen, "calculateHash")) {
            return "";
        }

        // Convert to hex string safely
        std::string result = bytesToHexString(hash, hashLen);

        MIDDLEWARE_LOG_DEBUG("Calculated hash: %s", result.c_str());
        return result;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in calculateHash: %s", e.what());
        return "";
    }
}

bool HashVerifier::verifyHashStreaming(const std::vector<uint8_t>& data,
                                     const std::string& expectedHash,
                                     size_t chunkSize) {
    MIDDLEWARE_LOG_DEBUG("Streaming hash verification for %zu bytes with chunk size %zu",
                        data.size(), chunkSize);

    // Input validation
    if (!validateInputData(data, "verifyHashStreaming")) {
        return false;
    }

    if (!validateHashString(expectedHash, "verifyHashStreaming")) {
        return false;
    }

    // Validate chunk size
    if (chunkSize == 0 || chunkSize > safety::MAX_BUFFER_SIZE) {
        MIDDLEWARE_LOG_ERROR("Invalid chunk size: %zu", chunkSize);
        return false;
    }

    try {
        std::lock_guard<std::mutex> lock(opensslMutex);

        // Create EVP context
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> mdctx(
            EVP_MD_CTX_new(), EVP_MD_CTX_free);

        if (!mdctx) {
            MIDDLEWARE_LOG_ERROR("Failed to create EVP_MD_CTX for streaming");
            return false;
        }

        // Initialize digest
        const EVP_MD* md = EVP_sha256();
        if (!md || !EVP_DigestInit_ex(mdctx.get(), md, nullptr)) {
            MIDDLEWARE_LOG_ERROR("Failed to initialize digest for streaming");
            return false;
        }

        // Process data in chunks
        size_t processed = 0;
        while (processed < data.size()) {
            size_t currentChunk = std::min(chunkSize, data.size() - processed);

            if (!EVP_DigestUpdate(mdctx.get(), data.data() + processed, currentChunk)) {
                MIDDLEWARE_LOG_ERROR("Failed to update digest during streaming at offset %zu", processed);
                return false;
            }

            processed += currentChunk;

            // Optional: Log progress for very large files
            if (data.size() > 100000000) { // 100MB
                float progress = static_cast<float>(processed) / static_cast<float>(data.size());
                if (static_cast<int>(progress * 10) % 2 == 0) { // Log every 20%
                    MIDDLEWARE_LOG_DEBUG("Streaming hash progress: %.1f%%", progress * 100.0f);
                }
            }
        }

        // Finalize hash
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hashLen = 0;

        if (!EVP_DigestFinal_ex(mdctx.get(), hash, &hashLen)) {
            MIDDLEWARE_LOG_ERROR("Failed to finalize digest for streaming");
            return false;
        }

        // Convert and compare
        std::string computedHash = bytesToHexString(hash, hashLen);
        bool result = compareHashes(computedHash, expectedHash);

        if (result) {
            MIDDLEWARE_LOG_DEBUG("Streaming hash verification successful");
        } else {
            MIDDLEWARE_LOG_WARNING("Streaming hash verification failed: expected=%s, computed=%s",
                                 expectedHash.c_str(), computedHash.c_str());
        }

        return result;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in verifyHashStreaming: %s", e.what());
        return false;
    }
}

std::string HashVerifier::calculateHashWithProgress(const std::vector<uint8_t>& data,
                                                   std::function<void(float)> progressCallback) {
    MIDDLEWARE_LOG_DEBUG("Calculating hash with progress for %zu bytes", data.size());

    // Input validation
    if (!validateInputData(data, "calculateHashWithProgress")) {
        return "";
    }

    try {
        std::lock_guard<std::mutex> lock(opensslMutex);

        // Create EVP context
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> mdctx(
            EVP_MD_CTX_new(), EVP_MD_CTX_free);

        if (!mdctx) {
            MIDDLEWARE_LOG_ERROR("Failed to create EVP_MD_CTX for progress calculation");
            return "";
        }

        // Initialize digest
        const EVP_MD* md = EVP_sha256();
        if (!md || !EVP_DigestInit_ex(mdctx.get(), md, nullptr)) {
            MIDDLEWARE_LOG_ERROR("Failed to initialize digest for progress calculation");
            return "";
        }

        // Process with progress updates
        const size_t chunkSize = 1048576; // 1MB chunks
        size_t processed = 0;

        while (processed < data.size()) {
            size_t currentChunk = std::min(chunkSize, data.size() - processed);

            if (!EVP_DigestUpdate(mdctx.get(), data.data() + processed, currentChunk)) {
                MIDDLEWARE_LOG_ERROR("Failed to update digest with progress at offset %zu", processed);
                return "";
            }

            processed += currentChunk;

            // Call progress callback if provided
            if (progressCallback) {
                float progress = static_cast<float>(processed) / static_cast<float>(data.size());
                try {
                    progressCallback(progress);
                } catch (const std::exception& e) {
                    MIDDLEWARE_LOG_WARNING("Exception in progress callback: %s", e.what());
                    // Continue processing despite callback error
                }
            }
        }

        // Finalize hash
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hashLen = 0;

        if (!EVP_DigestFinal_ex(mdctx.get(), hash, &hashLen)) {
            MIDDLEWARE_LOG_ERROR("Failed to finalize digest with progress");
            return "";
        }

        std::string result = bytesToHexString(hash, hashLen);

        // Final progress callback
        if (progressCallback) {
            try {
                progressCallback(1.0f);
            } catch (const std::exception& e) {
                MIDDLEWARE_LOG_WARNING("Exception in final progress callback: %s", e.what());
            }
        }

        MIDDLEWARE_LOG_DEBUG("Hash calculation with progress completed: %s", result.c_str());
        return result;

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in calculateHashWithProgress: %s", e.what());
        return "";
    }
}

bool HashVerifier::isValidHashFormat(const std::string& hashString) {
    // SHA256 hash should be exactly 64 hexadecimal characters
    if (hashString.length() != 64) {
        return false;
    }

    // Check if all characters are valid hexadecimal
    return std::all_of(hashString.begin(), hashString.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c));
    });
}

bool HashVerifier::compareHashes(const std::string& hash1, const std::string& hash2) {
    // Validate both hashes first
    if (!isValidHashFormat(hash1) || !isValidHashFormat(hash2)) {
        MIDDLEWARE_LOG_ERROR("Invalid hash format in comparison");
        return false;
    }

    // Convert to lowercase for comparison
    std::string lowerHash1 = hash1;
    std::string lowerHash2 = hash2;

    std::transform(lowerHash1.begin(), lowerHash1.end(), lowerHash1.begin(),
                   [](char c) { return std::tolower(static_cast<unsigned char>(c)); });
    std::transform(lowerHash2.begin(), lowerHash2.end(), lowerHash2.begin(),
                   [](char c) { return std::tolower(static_cast<unsigned char>(c)); });

    // Constant-time comparison to prevent timing attacks
    bool result = true;
    for (size_t i = 0; i < lowerHash1.length(); ++i) {
        if (lowerHash1[i] != lowerHash2[i]) {
            result = false;
        }
    }

    return result;
}

// Private helper methods

bool HashVerifier::validateInputData(const std::vector<uint8_t>& data, const std::string& context) {
    if (data.empty()) {
        MIDDLEWARE_LOG_ERROR("Cannot process hash: Empty data buffer in %s", context.c_str());
        return false;
    }

    if (data.size() > safety::MAX_BUFFER_SIZE) {
        MIDDLEWARE_LOG_ERROR("Data buffer too large (%zu bytes) in %s, max allowed: %zu",
                            data.size(), context.c_str(), safety::MAX_BUFFER_SIZE);
        return false;
    }

    return true;
}

bool HashVerifier::validateHashString(const std::string& hash, const std::string& context) {
    if (hash.empty()) {
        MIDDLEWARE_LOG_ERROR("Cannot process hash: Empty hash string in %s", context.c_str());
        return false;
    }

    if (!isValidHashFormat(hash)) {
        MIDDLEWARE_LOG_ERROR("Invalid hash format in %s: %s", context.c_str(), hash.c_str());
        return false;
    }

    return true;
}

bool HashVerifier::performHashOperation(const std::vector<uint8_t>& data,
                                       unsigned char* hash,
                                       unsigned int* hashLen,
                                       const std::string& context) {
    MIDDLEWARE_VALIDATE_POINTER(hash, context.c_str());
    MIDDLEWARE_VALIDATE_POINTER(hashLen, context.c_str());

    // Create EVP context with RAII
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> mdctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);

    if (!mdctx) {
        MIDDLEWARE_LOG_ERROR("Failed to create EVP_MD_CTX in %s", context.c_str());
        return false;
    }

    // Get SHA-256 algorithm
    const EVP_MD* md = EVP_sha256();
    if (!md) {
        MIDDLEWARE_LOG_ERROR("Failed to get SHA-256 algorithm in %s", context.c_str());
        return false;
    }

    // Initialize digest
    if (!EVP_DigestInit_ex(mdctx.get(), md, nullptr)) {
        MIDDLEWARE_LOG_ERROR("Failed to initialize digest context in %s", context.c_str());
        return false;
    }

    // Update with data
    if (!EVP_DigestUpdate(mdctx.get(), data.data(), data.size())) {
        MIDDLEWARE_LOG_ERROR("Failed to update digest with data in %s", context.c_str());
        return false;
    }

    // Finalize
    if (!EVP_DigestFinal_ex(mdctx.get(), hash, hashLen)) {
        MIDDLEWARE_LOG_ERROR("Failed to finalize digest in %s", context.c_str());
        return false;
    }

    return true;
}

std::string HashVerifier::bytesToHexString(const unsigned char* bytes, unsigned int length) {
    if (!bytes || length == 0) {
        MIDDLEWARE_LOG_ERROR("Invalid input to bytesToHexString");
        return "";
    }

    try {
        std::stringstream ss;
        ss << std::hex << std::setfill('0');

        for (unsigned int i = 0; i < length; ++i) {
            ss << std::setw(2) << static_cast<unsigned int>(bytes[i]);
        }

        return ss.str();

    } catch (const std::exception& e) {
        MIDDLEWARE_LOG_ERROR("Exception in bytesToHexString: %s", e.what());
        return "";
    }
}

} // namespace anari_usd_middleware
