# Hash Verifier

`HashVerifier` — Threadsafe, stateless XXH3-128 hash computation and verification.

## Files
- `include/HashVerifier.h` (43 lines)
- `src/HashVerifier.cpp`
- `external/xxhash/xxhash.c` (bundled)

## Migration: SHA-256 → XXH3-128

| Property | Old (SHA-256) | New (XXH3-128) |
|---|---|---|
| Library | OpenSSL (heavy, mutex needed) | xxHash (tiny, stateless) |
| Hash Size | 256-bit (64 hex chars) | 128-bit (lo64, hi64) |
| Speed | ~1 GB/s | ~15 GB/s (32-bit, ~30 GB/s 64-bit) |
| Thread Safety | OpenSSL not lock-free | Inherently stateless |
| Crypto Security | Yes | No (non-cryptographic) |
| Purpose | Integrity check | Integrity check (sufficient) |

## API

```cpp
class HashVerifier {
public:
    // Compute XXH3-128 → {low64, high64}
    static pair<uint64_t, uint64_t> calculateHash128(const vector<uint8_t>& data);

    // Verify data against expected {lo, hi}
    static bool verifyHash128(const vector<uint8_t>& data, uint64_t expectedLo, uint64_t expectedHi);

    // Basic bounds validation
    static bool validateInputData(const vector<uint8_t>& data);

private:
    HashVerifier() = delete;              // Stateless — no instances
    ~HashVerifier() = delete;
    HashVerifier(const HashVerifier&) = delete;
    HashVerifier& operator=(const HashVerifier&) = delete;
};
```

## Usage in Pipeline

1. **Send side** (broker/worker): Compute `xxhash128(fileData)` → attach to `ZmqFileNotification.hash128[2]` or send in separate frame
2. **Receive side** (`ZmqConnector::receiveFile()`): Compute local hash → `HashVerifier::verifyHash128()` compares

## Hash in Notification Protocol

`ZmqFileNotification` includes:
```cpp
uint64_t hash128[2];  // XXH3-128 hash of file data
```
Consumer can skip re-download if local cache matches.
