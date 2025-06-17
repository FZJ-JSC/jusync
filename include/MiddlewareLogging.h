#pragma once

// Check if we're compiling with Unreal Engine
#if defined(_MSC_VER) && defined(__UNREAL__)
    // Unreal Engine environment
    #include "CoreMinimal.h"
    #define MIDDLEWARE_LOG_INFO(format, ...) UE_LOG(LogTemp, Display, TEXT(format), ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_WARNING(format, ...) UE_LOG(LogTemp, Warning, TEXT(format), ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_ERROR(format, ...) UE_LOG(LogTemp, Error, TEXT(format), ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_DEBUG(format, ...) UE_LOG(LogTemp, Verbose, TEXT(format), ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_VERBOSE(format, ...) UE_LOG(LogTemp, VeryVerbose, TEXT(format), ##__VA_ARGS__)

    // String conversion helpers
    #define TO_MIDDLEWARE_STRING(str) FString(UTF8_TO_TCHAR(str.c_str()))
    #define FROM_MIDDLEWARE_STRING(str) TCHAR_TO_UTF8(*str)
#else
    // Standard C++ environment
    #include <iostream>
    #include <string>
    #define MIDDLEWARE_LOG_INFO(format, ...) printf("[INFO] " format "\n", ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_WARNING(format, ...) fprintf(stderr, "[WARNING] " format "\n", ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_ERROR(format, ...) fprintf(stderr, "[ERROR] " format "\n", ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_DEBUG(format, ...) printf("[DEBUG] " format "\n", ##__VA_ARGS__)
    #define MIDDLEWARE_LOG_VERBOSE(format, ...) printf("[VERBOSE] " format "\n", ##__VA_ARGS__)

    // String conversion helpers (no-ops in standard C++)
    #define TO_MIDDLEWARE_STRING(str) str
    #define FROM_MIDDLEWARE_STRING(str) str
#endif
