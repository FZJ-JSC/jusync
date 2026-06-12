# Testing

JUSYNC includes a comprehensive test suite with multiple execution modes.

---

## Test Targets

| CMake Target | Description |
|---|---|
| `anari_usd_middleware` | Main library (prerequisite) |
| `usd_validation_tool` | USD validation CLI |
| `test_anari_usd_client` | ANARI client test (if source exists) |
| `test_quick` | Basic validation (3 categories) |
| `test_collision` | Collision-specific tests |
| `test_full` | Full suite, verbose output |
| `test_parallel` | Parallel execution |
| `run_validation_tool` | Legacy: run all USD file tests |
| `verify_glibc_compatibility` | Linux-only ABI check |

---

## CTest Categories (9)

| Test Name | Command | Depends On |
|---|---|---|
| `build_verification` | Echo test | — |
| `static_analysis` | `--mode=static-check` | `build_verification` |
| `usd_basic_processing` | `--mode=usd-basic` | `static_analysis` |
| `collision_simple` | `--mode=collision --type=simple` | `usd_basic_processing` |
| `collision_complex` | `--mode=collision --type=complex` | `usd_basic_processing` |
| `collision_convex` | `--mode=collision --type=convex` | `usd_basic_processing` |
| `error_handling_tests` | `--mode=error-handling` | `usd_basic_processing` |
| `performance_benchmarks` | `--mode=performance` | all 3 collision |
| `integration_suite` | `--mode=integration` | `usd_basic` + `collision_simple` + `error` |

All tests have a **300 second timeout**.

---

## Running Tests

```bash
# Quick smoke tests
make test_quick

# Collision tests only
make test_collision

# Full suite
make test_full

# Parallel (uses all cores)
make test_parallel

# Or directly with ctest
ctest --output-on-failure            # All
ctest -R collision_ --verbose       # Filter
ctest -j$(nproc) --output-on-failure # Parallel
```

---

## CLI Tool Modes

```bash
# Direct CLI usage
./usd_validation_tool --mode=usd-basic
./usd_validation_tool --mode=collision --type=simple
./usd_validation_tool --mode=performance
./usd_validation_tool --mode=static-check
./usd_validation_tool --mode=error-handling
./usd_validation_tool --mode=integration

# Or with a directory of USD files
./usd_validation_tool /path/to/usd_samples
```

---

## Individual File Tests

When `tests/data/usd_samples/` exists, each `.usda` file becomes an individual CTest:
```
usd_file_<name>  # Runs validation on that specific file
```
Each file test has a **120 second timeout** and depends on `usd_basic_processing`.

---

## Test Data

CMake copies `tests/data/usd_samples/*.usda`, `*.usdc`, `*.usd` into `build/test_data/usd_samples/` at configure time. Reports are written to `build/test_reports/`.

---

## GUI Testing Tool

Build with `-DBUILD_JUSYNC_Receiver_GUI=ON`:
- Dear ImGui application at `tools/ReceiverUI/`
- Visualizes loaded USD models in 3D
- Shows connection status, file reception, mesh data, texture previews, performance metrics

---

## Build Verification (Linux)

```bash
make verify_glibc_compatibility
```
Runs post-build checks:
1. `objdump -T` — GLIBC version requirements
2. `ldd` — Missing dynamic dependencies
3. `ldd | grep zmq` — Confirms ZMQ is static (should be empty)
4. `readelf -d | grep stdc++` — Confirms libstdc++ is static (should be empty)
