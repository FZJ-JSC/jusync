# Troubleshooting

## Common Issues

### Port Binding Fails
**Symptom**: `initialize()` returns false, log shows bind error.
**Fix**: The middleware auto-falls back to ports 5557, 5558, etc. Check `getCurrentEndpoint()` to see which port succeeded.

### Hash Verification Fails
**Symptom**: File rejected, log shows "hash mismatch: expected XX, calculated YY".
**Fix**: Sender and receiver must use the same hash algorithm. JUSYNC switched from SHA-256 to **XXH3-128** in v1.0.1. If your broker still sends SHA-256, you need to update it or use the legacy receiver.

### USD Reference Not Found
**Symptom**: Mesh loaded but missing geometry. Log says "reference resolution failed".
**Fix**:
- Ensure `setReferenceResolutionEnabled(true)` (default)
- Referenced `.usdc`/`.usda` files must be in the same directory or resolvable via TinyUSDZ's search path
- Check `ProcessingStats::referencesResolved` count

### Missing OpenSSL
**Symptom**: CMake configure fails: `OpenSSL::SSL not found`.
**Fix**:
```bash
# Debian/Ubuntu
sudo apt install libssl-dev

# Specify path
cmake -DOPENSSL_ROOT_DIR=/usr/local/openssl ..
```

### GCC 15 Compilation Error
**Symptom**: tinyusdz macro `is_array` template-dependent lookup failure.
**Fix**: CMakeList already adds `-fpermissive` for GCC >= 15. If still failing, ensure CMake version >= 3.16.

### GLIBC Mismatch (Binary Built on Newer Host)
**Symptom**: `ERROR: dynamic linking: GLIBC_2.36 not found`.
**Fix**: Build on a GLIBC 2.35 host or use Docker. The library statically links libstdc++/libgcc, but GLIBC itself cannot be embedded.

### TBB Not Found
**Symptom**: Log: "TBB: Not found".
**Fix**: Parallel UV normalization and batch collision still work (fall back to std::thread). Install `libtbb-dev` for full performance.

### CUDA Not Found
**Symptom**: CMake: "CUDA requested but nvcc not found".
**Fix**: Install CUDA toolkit or build without `-DENABLE_CUDA=ON`. All GPU kernels fall back to CPU transparently.

## Debugging

### Enable Verbose Logging
Remove the `DISABLE_DEBUG_LOGGING` define in `MiddlewareLogging.h`:
```cpp
// #define DISABLE_DEBUG_LOGGING  <- comment this out
```
This enables `MIDDLEWARE_LOG_DEBUG` and `MIDDLEWARE_LOG_VERBOSE` which show:
- Full USD prim hierarchy
- ZeroMQ send/receive details
- File system operations
- GPU kernel launch details

### Linux ABI Check
```bash
make verify_glibc_compatibility

# Manual checks
ldd libanari_usd_middleware.so | grep zmq     # Should be empty (static)
ldd libanari_usd_middleware.so | grep not found  # Should be empty
objdump -T libanari_usd_middleware.so | grep GLIBC_ | sort -u | tail -1
```

### Observe ZeroMQ Traffic
Use `zmq_proxy` or tcpdump:
```bash
tcpdump -i any -nn port 5556 -w /tmp/zmq.pcap
```
