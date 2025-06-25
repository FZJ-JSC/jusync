#pragma once

// Check if we're compiling with Unreal Engine
#if defined(_MSC_VER) && defined(__UNREAL__)
// Unreal Engine environment
#include "CoreMinimal.h"
#include "HAL/PlatformMemory.h"
#include "Misc/ScopeLock.h"

#define MIDDLEWARE_LOG_INFO(format, ...) UE_LOG(LogTemp, Display, TEXT(format), ##__VA_ARGS__)
#define MIDDLEWARE_LOG_WARNING(format, ...) UE_LOG(LogTemp, Warning, TEXT(format), ##__VA_ARGS__)
#define MIDDLEWARE_LOG_ERROR(format, ...) UE_LOG(LogTemp, Error, TEXT(format), ##__VA_ARGS__)
#define MIDDLEWARE_LOG_DEBUG(format, ...) UE_LOG(LogTemp, Verbose, TEXT(format), ##__VA_ARGS__)
#define MIDDLEWARE_LOG_VERBOSE(format, ...) UE_LOG(LogTemp, VeryVerbose, TEXT(format), ##__VA_ARGS__)

// String conversion helpers
#define TO_MIDDLEWARE_STRING(str) FString(UTF8_TO_TCHAR(str.c_str()))
#define FROM_MIDDLEWARE_STRING(str) TCHAR_TO_UTF8(*str)

// Unreal Engine 5.5 specific safety measures
#define MIDDLEWARE_SAFE_DELETE(ptr) \
    do { \
        if (ptr) { \
            delete ptr; \
            ptr = nullptr; \
        } \
    } while(0)

#define MIDDLEWARE_VALIDATE_POINTER(ptr, context) \
    do { \
        if (!ptr || !FPlatformMemory::IsValidPointer(ptr)) { \
            UE_LOG(LogTemp, Error, TEXT("Invalid pointer in %s"), TEXT(context)); \
            return false; \
        } \
    } while(0)

#define MIDDLEWARE_SAFE_ARRAY_ACCESS(array, index, context) \
    do { \
        if (index >= array.size() || index < 0) { \
            UE_LOG(LogTemp, Error, TEXT("Array bounds violation in %s: index %d, size %d"), \
                TEXT(context), static_cast<int32>(index), static_cast<int32>(array.size())); \
            return false; \
        } \
    } while(0)

#else
// Standard C++ environment
#include <iostream>
#include <cstdio>

#define MIDDLEWARE_LOG_INFO(format, ...) printf("[INFO] " format "\n", ##__VA_ARGS__)
#define MIDDLEWARE_LOG_WARNING(format, ...) fprintf(stderr, "[WARNING] " format "\n", ##__VA_ARGS__)
#define MIDDLEWARE_LOG_ERROR(format, ...) fprintf(stderr, "[ERROR] " format "\n", ##__VA_ARGS__)
#define MIDDLEWARE_LOG_DEBUG(format, ...) printf("[DEBUG] " format "\n", ##__VA_ARGS__)
#define MIDDLEWARE_LOG_VERBOSE(format, ...) printf("[VERBOSE] " format "\n", ##__VA_ARGS__)

// String conversion helpers (no-ops in standard C++)
#define TO_MIDDLEWARE_STRING(str) str
#define FROM_MIDDLEWARE_STRING(str) str

#define MIDDLEWARE_SAFE_DELETE(ptr) \
    do { \
        if (ptr) { \
            delete ptr; \
            ptr = nullptr; \
        } \
    } while(0)

#define MIDDLEWARE_VALIDATE_POINTER(ptr, context) \
    do { \
        if (!ptr) { \
            fprintf(stderr, "[ERROR] Invalid pointer in %s\n", context); \
            return false; \
        } \
    } while(0)

#define MIDDLEWARE_SAFE_ARRAY_ACCESS(array, index, context) \
    do { \
        if (index >= array.size() || index < 0) { \
            fprintf(stderr, "[ERROR] Array bounds violation in %s: index %zu, size %zu\n", \
                context, static_cast<size_t>(index), array.size()); \
            return false; \
        } \
    } while(0)

#endif

// Common safety constants
namespace anari_usd_middleware {
    namespace safety {
        static constexpr size_t MAX_BUFFER_SIZE = 500000000;        // 500MB
        static constexpr size_t MAX_VECTOR_SIZE = 100000000;        // 100M elements
        static constexpr size_t MAX_STRING_SIZE = 10000000;         // 10MB
        static constexpr size_t MAX_MESH_VERTICES = 10000000;       // 10M vertices
        static constexpr size_t MAX_MESH_INDICES = 30000000;        // 30M indices
        static constexpr int32_t MAX_RECURSION_DEPTH = 100;         // Max USD hierarchy depth
        static constexpr double EPSILON = 1e-10;                    // For floating point comparisons
    }
}

// Safe math operations
#define MIDDLEWARE_SAFE_DIVIDE(numerator, denominator, result, context) \
    do { \
        if (std::abs(denominator) < anari_usd_middleware::safety::EPSILON) { \
            MIDDLEWARE_LOG_ERROR("Division by zero in %s", context); \
            result = 0; \
        } else { \
            result = numerator / denominator; \
        } \
    } while(0)

#define MIDDLEWARE_VALIDATE_FINITE(value, context) \
    do { \
        if (!std::isfinite(value)) { \
            MIDDLEWARE_LOG_ERROR("Non-finite value detected in %s: %f", context, static_cast<double>(value)); \
            return false; \
        } \
    } while(0)
