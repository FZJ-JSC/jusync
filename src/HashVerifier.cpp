#include "HashVerifier.h"
#include "MiddlewareLogging.h"
#include <openssl/evp.h>
#include <sstream>
#include <iomanip>

namespace anari_usd_middleware {

bool HashVerifier::verifyHash(const std::vector<uint8_t>& data, const std::string& expectedHash) {
    MIDDLEWARE_LOG_DEBUG("Verifying hash for data of size %zu bytes", data.size());

    if (data.empty()) {
        MIDDLEWARE_LOG_ERROR("Cannot verify hash: Empty data buffer");
        return false;
    }

    if (expectedHash.empty()) {
        MIDDLEWARE_LOG_ERROR("Cannot verify hash: Empty expected hash");
        return false;
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen;

    // Create a new EVP_MD_CTX for the hash computation
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        MIDDLEWARE_LOG_ERROR("Failed to create EVP_MD_CTX");
        return false;
    }

    // Use SHA-256 as the hash algorithm
    const EVP_MD* md = EVP_sha256();
    if (!md) {
        MIDDLEWARE_LOG_ERROR("Failed to get SHA-256 algorithm");
        EVP_MD_CTX_free(mdctx);
        return false;
    }

    bool success = false;

    // Initialize the hash context
    if (!EVP_DigestInit_ex(mdctx, md, nullptr)) {
        MIDDLEWARE_LOG_ERROR("Failed to initialize digest context");
        EVP_MD_CTX_free(mdctx);
        return false;
    }

    // Update the hash with the data
    if (!EVP_DigestUpdate(mdctx, data.data(), data.size())) {
        MIDDLEWARE_LOG_ERROR("Failed to update digest with data");
        EVP_MD_CTX_free(mdctx);
        return false;
    }

    // Finalize the hash computation
    if (!EVP_DigestFinal_ex(mdctx, hash, &hashLen)) {
        MIDDLEWARE_LOG_ERROR("Failed to finalize digest");
        EVP_MD_CTX_free(mdctx);
        return false;
    }

    // Convert the binary hash to a hexadecimal string
    std::stringstream ss;
    for (unsigned int i = 0; i < hashLen; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    std::string computedHash = ss.str();

    // Compare the computed hash with the expected hash
    success = (computedHash == expectedHash);

    if (success) {
        MIDDLEWARE_LOG_DEBUG("Hash verification successful");
    } else {
        MIDDLEWARE_LOG_WARNING("Hash verification failed: expected=%s, computed=%s",
                              expectedHash.c_str(), computedHash.c_str());
    }

    // Clean up
    EVP_MD_CTX_free(mdctx);

    return success;
}

    std::string HashVerifier::calculateHash(const std::vector<uint8_t>& data) {
    MIDDLEWARE_LOG_DEBUG("Calculating hash for data of size %zu bytes", data.size());
    if (data.empty()) {
        MIDDLEWARE_LOG_ERROR("Cannot calculate hash: Empty data buffer");
        return "";
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen;

    // Create a new EVP_MD_CTX for the hash computation
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        MIDDLEWARE_LOG_ERROR("Failed to create EVP_MD_CTX");
        return "";
    }

    // Use SHA-256 as the hash algorithm
    const EVP_MD* md = EVP_sha256();
    if (!md) {
        MIDDLEWARE_LOG_ERROR("Failed to get SHA-256 algorithm");
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    // Initialize the hash context
    if (!EVP_DigestInit_ex(mdctx, md, nullptr)) {
        MIDDLEWARE_LOG_ERROR("Failed to initialize digest context");
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    // Update the hash with the data
    if (!EVP_DigestUpdate(mdctx, data.data(), data.size())) {
        MIDDLEWARE_LOG_ERROR("Failed to update digest with data");
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    // Finalize the hash computation
    if (!EVP_DigestFinal_ex(mdctx, hash, &hashLen)) {
        MIDDLEWARE_LOG_ERROR("Failed to finalize digest");
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    // Convert the binary hash to a hexadecimal string
    std::stringstream ss;
    for (unsigned int i = 0; i < hashLen; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    std::string computedHash = ss.str();
    MIDDLEWARE_LOG_DEBUG("Calculated hash: %s", computedHash.c_str());

    // Clean up
    EVP_MD_CTX_free(mdctx);
    return computedHash;
}

} 
