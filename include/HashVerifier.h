#pragma once

#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include <mutex>
#include "MiddlewareLogging.h"

namespace anari_usd_middleware {

/**
 * Thread-safe utility class for verifying SHA256 hashes with comprehensive error handling
 * and memory safety features for Unreal Engine 5.5 compatibility.
 */
class HashVerifier {
public:
    /**
     * Verify a SHA256 hash against provided data with bounds checking
     * @param data The data buffer to verify (must not exceed safety limits)
     * @param expectedHash The expected SHA256 hash string (64 hex characters)
     * @return True if hash matches and verification succeeds, false otherwise
     */
    static bool verifyHash(const std::vector<uint8_t>& data, const std::string& expectedHash);

    /**
     * Calculate SHA256 hash from data buffer with memory safety
     * @param data The data buffer to hash (must not exceed safety limits)
     * @return SHA256 hash as lowercase hex string, empty string on failure
     */
    static std::string calculateHash(const std::vector<uint8_t>& data);

    /**
     * Verify hash with streaming support for large files
     * @param data The data buffer to verify
     * @param expectedHash The expected hash
     * @param chunkSize Size of chunks to process (default 1MB)
     * @return True if verification succeeds, false otherwise
     */
    static bool verifyHashStreaming(const std::vector<uint8_t>& data,
                                   const std::string& expectedHash,
                                   size_t chunkSize = 1048576);

    /**
     * Calculate hash with progress callback for large operations
     * @param data The data buffer to hash
     * @param progressCallback Optional callback for progress updates (0.0 to 1.0)
     * @return SHA256 hash string, empty on failure
     */
    static std::string calculateHashWithProgress(const std::vector<uint8_t>& data,
                                                std::function<void(float)> progressCallback = nullptr);

    /**
     * Validate hash string format
     * @param hashString The hash string to validate
     * @return True if valid SHA256 format (64 hex characters), false otherwise
     */
    static bool isValidHashFormat(const std::string& hashString);

    /**
     * Compare two hash strings safely
     * @param hash1 First hash string
     * @param hash2 Second hash string
     * @return True if hashes match, false otherwise
     */
    static bool compareHashes(const std::string& hash1, const std::string& hash2);

    /**
     * Get the maximum safe buffer size for hash operations
     * @return Maximum buffer size in bytes
     */
    static constexpr size_t getMaxSafeBufferSize() {
        return safety::MAX_BUFFER_SIZE;
    }

private:
    // Thread safety for OpenSSL operations
    static std::mutex opensslMutex;

    // Internal validation helpers
    static bool validateInputData(const std::vector<uint8_t>& data, const std::string& context);
    static bool validateHashString(const std::string& hash, const std::string& context);

    // OpenSSL wrapper with error handling
    static bool performHashOperation(const std::vector<uint8_t>& data,
                                   unsigned char* hash,
                                   unsigned int* hashLen,
                                   const std::string& context);

    // Safe hex conversion
    static std::string bytesToHexString(const unsigned char* bytes, unsigned int length);

    // Prevent instantiation
    HashVerifier() = delete;
    ~HashVerifier() = delete;
    HashVerifier(const HashVerifier&) = delete;
    HashVerifier& operator=(const HashVerifier&) = delete;
};

} // namespace anari_usd_middleware
