#pragma once


#if defined(_MSC_VER) && defined(__UNREAL__)
#include "CoreMinimal.h"
#include "HAL/PlatformMemory.h"
#include "Misc/ScopeLock.h"

#define MIDDLEWARE_LOG_WARNING(format, ...) UE_LOG(LogTemp, Warning, TEXT(format), ##__VA_ARGS__)
#define MIDDLEWARE_LOG_ERROR(format, ...) UE_LOG(LogTemp, Error, TEXT(format), ##__VA_ARGS__)

#ifdef DISABLE_DEBUG_LOGGING
    #define MIDDLEWARE_LOG_INFO(format, ...)
    #define MIDDLEWARE_LOG_DEBUG(format, ...)
    #define MIDDLEWARE_LOG_VERBOSE(format, ...)
#else
    #define MIDDLEWARE_LOG_INFO(format, ...) UE_LOG(LogTemp, Display, TEXT(format), ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_DEBUG(format, ...) UE_LOG(LogTemp, Verbose, TEXT(format), ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_VERBOSE(format, ...) UE_LOG(LogTemp, VeryVerbose, TEXT(format), ##__VA_ARGS__)
#endif

#define TO_MIDDLEWARE_STRING(str) FString(UTF8_TO_TCHAR(str.c_str()))
#define FROM_MIDDLEWARE_STRING(str) TCHAR_TO_UTF8(*str)

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
#include <iostream>
#include <cstdio>
#include <limits>

#ifdef _WIN32
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char* lpOutputString);

#define MIDDLEWARE_LOG_TO_OUTPUT(format, ...) do { \
    char buf[1024]; \
    snprintf(buf, sizeof(buf), format, ##__VA_ARGS__); \
    OutputDebugStringA(buf); \
    printf("%s", buf); \
} while(0)

#define MIDDLEWARE_LOG_WARNING(format, ...) MIDDLEWARE_LOG_TO_OUTPUT("[WARNING] " format "\n", ##__VA_ARGS__)
#define MIDDLEWARE_LOG_ERROR(format, ...) MIDDLEWARE_LOG_TO_OUTPUT("[ERROR] " format "\n", ##__VA_ARGS__)

#ifdef DISABLE_DEBUG_LOGGING
    #define MIDDLEWARE_LOG_INFO(format, ...)
    #define MIDDLEWARE_LOG_DEBUG(format, ...)
    #define MIDDLEWARE_LOG_VERBOSE(format, ...)
#else
    #define MIDDLEWARE_LOG_INFO(format, ...) MIDDLEWARE_LOG_TO_OUTPUT("[INFO] " format "\n", ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_DEBUG(format, ...) MIDDLEWARE_LOG_TO_OUTPUT("[DEBUG] " format "\n", ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_VERBOSE(format, ...) MIDDLEWARE_LOG_TO_OUTPUT("[VERBOSE] " format "\n", ##__VA_ARGS__)
#endif

#else
#define MIDDLEWARE_LOG_WARNING(format, ...) fprintf(stderr, "[WARNING] " format "\n", ##__VA_ARGS__)
#define MIDDLEWARE_LOG_ERROR(format, ...) fprintf(stderr, "[ERROR] " format "\n", ##__VA_ARGS__)

#ifdef DISABLE_DEBUG_LOGGING
    #define MIDDLEWARE_LOG_INFO(format, ...)
    #define MIDDLEWARE_LOG_DEBUG(format, ...)
    #define MIDDLEWARE_LOG_VERBOSE(format, ...)
#else
    #define MIDDLEWARE_LOG_INFO(format, ...) printf("[INFO] " format "\n", ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_DEBUG(format, ...) printf("[DEBUG] " format "\n", ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_VERBOSE(format, ...) printf("[VERBOSE] " format "\n", ##__VA_ARGS__)
#endif
#endif

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

namespace anari_usd_middleware {
    namespace safety {
        static constexpr size_t MAX_BUFFER_SIZE = static_cast<size_t>(std::numeric_limits<int64_t>::max() / 2); // Essentially unlimited (4.6EB)
        static constexpr size_t MAX_VECTOR_SIZE = static_cast<size_t>(std::numeric_limits<int64_t>::max() / 2); // Unlimited elements
        static constexpr size_t MAX_STRING_SIZE = static_cast<size_t>(std::numeric_limits<int64_t>::max() / 2); // Unlimited string size
        static constexpr size_t MAX_MESH_VERTICES = static_cast<size_t>(std::numeric_limits<int64_t>::max() / 2); // Unlimited vertices for RMC
        static constexpr size_t MAX_MESH_INDICES = static_cast<size_t>(std::numeric_limits<int64_t>::max() / 2); // Unlimited indices for RMC
        static constexpr int32_t MAX_RECURSION_DEPTH = 1000;        // Increased USD hierarchy depth
        static constexpr double EPSILON = 1e-10;                    // For floating point comparisons
    }
}

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
