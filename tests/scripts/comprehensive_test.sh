#!/usr/bin/env bash
set -euo pipefail

# ════════════════════════════════════════════════════════════════════════════════
#               JUSYNC USD COLLISION VALIDATION SUITE - COMPACT
# ════════════════════════════════════════════════════════════════════════════════

# CONFIGURATION
WORKSPACE="/workspace"
BUILD_DIR="$WORKSPACE/build_test"
TEST_DATA_DIR="$WORKSPACE/tests/data/usd_samples"
REPORTS_DIR="$WORKSPACE/tests/reports"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")

# COLOR LOGGING
RED=$'\e[0;31m'; GREEN=$'\e[0;32m'; YELLOW=$'\e[1;33m'
BLUE=$'\e[0;34m'; PURPLE=$'\e[0;35m'; CYAN=$'\e[0;36m'; NC=$'\e[0m'
log_info()    { printf '%b\n' "${BLUE}[INFO]${NC} $*"; }
log_success() { printf '%b\n' "${GREEN}[SUCCESS]${NC} $*"; }
log_warning() { printf '%b\n' "${YELLOW}[WARNING]${NC} $*"; }
log_error()   { printf '%b\n' "${RED}[ERROR]${NC} $*"; }
log_debug()   { printf '%b\n' "${CYAN}[DEBUG]${NC} $*"; }

# ENVIRONMENT SETUP
initialize_environment() {
    log_info "Setting up test environment..."
    mkdir -p "$BUILD_DIR" "$REPORTS_DIR" "$TEST_DATA_DIR"

    # Optionally create minimal test files if directory is empty
    if [ ! "$(ls -A "$TEST_DATA_DIR" 2>/dev/null)" ]; then
        log_info "No USD test files found, creating a minimal test file for smoke test..."
        cat > "$TEST_DATA_DIR/simple_minimal.usda" <<'USDA'
#usda 1.0
def Mesh "TestMesh" {}
USDA
    fi
}

# BUILD PHASE (no changes)
build_validation() {
    log_info "Building validation tool..."
    rm -rf "$BUILD_DIR"/*
    cmake -S "$WORKSPACE" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
    ninja -C "$BUILD_DIR" -j"$(nproc)"
    if [[ ! -x "$BUILD_DIR/usd_validation_tool" ]]; then
        log_error "usd_validation_tool executable not found after build!"
        exit 1
    fi
    log_success "Validation tool built successfully."
}

# CALL THE VALIDATION TOOL (no file logic here)
run_usd_validation() {
    log_info "Running USD validation tool on $TEST_DATA_DIR"
    local validation_log="$REPORTS_DIR/validation_${TIMESTAMP}.log"
    if ! "$BUILD_DIR/usd_validation_tool" "$TEST_DATA_DIR" > "$validation_log" 2>&1; then
        log_error "Validation failed. See $validation_log for details."
        exit 1
    fi
    log_success "Validation completed. Log: $validation_log"
}

# OPTIONAL: ERROR HANDLING TEST
error_handling_test() {
    log_info "Testing error handling..."
    local error_log="$REPORTS_DIR/error_handling_${TIMESTAMP}.log"
    local tests_passed=0
    local total_tests=2

    if ! "$BUILD_DIR/usd_validation_tool" "/non/existent/path" > "$error_log" 2>&1; then
        log_success "Correctly rejected non-existent directory."
        tests_passed=$((tests_passed+1))
    else
        log_warning "Tool did not fail for non-existent directory."
    fi

    local empty_dir="/tmp/empty_usd_test"
    mkdir -p "$empty_dir"
    if "$BUILD_DIR/usd_validation_tool" "$empty_dir" > "$error_log" 2>&1; then
        log_success "Handled empty directory gracefully."
        tests_passed=$((tests_passed+1))
    else
        log_warning "Tool failed with empty directory (may be expected)."
    fi
    rm -rf "$empty_dir"
    log_info "Error handling: $tests_passed/$total_tests tests passed."
}

# OPTIONAL: PERFORMANCE BENCHMARKS
performance_benchmark() {
    log_info "Running performance benchmark..."
    local runs=3
    local total_time=0
    for run in $(seq 1 $runs); do
        log_debug "Performance run $run/$runs"
        local start_time end_time duration_ms
        start_time=$(date +%s%N)
        "$BUILD_DIR/usd_validation_tool" "$TEST_DATA_DIR" > /dev/null 2>&1
        end_time=$(date +%s%N)
        duration_ms=$(( (end_time - start_time) / 1000000 ))
        total_time=$((total_time + duration_ms))
        log_debug "Run $run: ${duration_ms}ms"
    done
    log_info "Average validation time: $((total_time/runs)) ms"
}

# OPTIONAL: INTEGRATION SMOKE TEST
integration_smoke_test() {
    log_info "Running integration smoke test..."
    if [[ ! -f "$BUILD_DIR/libanari_usd_middleware.so" ]]; then
        log_warning "Middleware library not found."
    fi
    if [[ -x "$BUILD_DIR/usd_validation_tool" ]]; then
        log_success "usd_validation_tool is executable."
    else
        log_error "usd_validation_tool is not executable."
    fi
}

# MAIN PHASES
initialize_environment
build_validation
run_usd_validation

# Optionally enable the next lines for extra smoke/performance/error tests:
# error_handling_test
# performance_benchmark
# integration_smoke_test

log_success "ALL TESTS COMPLETED"
