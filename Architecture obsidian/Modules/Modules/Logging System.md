# Logging System

Cross-platform logging macro system with automatic environment detection (Unreal Engine vs standard C++).

## File
- `include/MiddlewareLogging.h` (162 lines)

## Environment Detection

```
Compiling inside Unreal Engine?
  ┌─ Yes → UE_LOG(LogTemp, Display/Warning/Error/Verbose/VeryVerbose)
  └─ No → Compile on Windows?
            ┌─ Yes → OutputDebugStringA() + printf()
            └─ No  → printf() / fprintf(stderr)
```

Detection: `#if defined(_MSC_VER) && defined(__UNREAL__)`

## Log Levels

| Macro | Unreal | Windows std | Linux std |
|---|---|---|---|
| `MIDDLEWARE_LOG_INFO` | `Display` | `OutputDebugStringA` | `printf` |
| `MIDDLEWARE_LOG_WARNING` | `Warning` | `OutputDebugStringA` | `fprintf(stderr)` |
| `MIDDLEWARE_LOG_ERROR` | `Error` | `OutputDebugStringA` | `fprintf(stderr)` |
| `MIDDLEWARE_LOG_DEBUG` | `Verbose` | `OutputDebugStringA` | `printf` |
| `MIDDLEWARE_LOG_VERBOSE` | `VeryVerbose` | (none) | `printf` |

Production optimization:
```cpp
#define DISABLE_DEBUG_LOGGING  // Uncomment to eliminate debug/verbose overhead
```
When defined, `MIDDLEWARE_LOG_DEBUG` and `MIDDLEWARE_LOG_VERBOSE` expand to nothing (empty macro).

## String Conversion Helpers

```cpp
// Unreal: std::string → FString
#define TO_MIDDLEWARE_STRING(str)   FString(UTF8_TO_TCHAR(str.c_str()))
#define FROM_MIDDLEWARE_STRING(str) TCHAR_TO_UTF8(*str)

// Standard C++: no-op identity
#define TO_MIDDLEWARE_STRING(str)   str
#define FROM_MIDDLEWARE_STRING(str) str
```

## Safety Macros

### Pointer Validation
```cpp
MIDDLEWARE_VALIDATE_POINTER(ptr, "context_name");
// If ptr is null → log error, return false immediately
```
Unreal variant additionally checks `FPlatformMemory::IsValidPointer(ptr)`.

### Safe Delete
```cpp
MIDDLEWARE_SAFE_DELETE(ptr);
// delete ptr; ptr = nullptr; (null-safe)
```

### Array Bounds
```cpp
MIDDLEWARE_SAFE_ARRAY_ACCESS(array, index, "context");
// If index out of range → log error with index + size, return false
```

### Safe Math
```cpp
MIDDLEWARE_SAFE_DIVIDE(numerator, denominator, result, "context");
// If denominator < EPSILON → log, result = 0

MIDDLEWARE_VALIDATE_FINITE(value, "context");
// If !std::isfinite(value) → log NaN/Inf, return false
```

## Safety Constants

```cpp
namespace anari_usd_middleware {
    namespace safety {
        static constexpr size_t MAX_BUFFER_SIZE    = ~MAX_INT64 / 2;   // ~4.6 EB
        static constexpr size_t MAX_VECTOR_SIZE    = ~MAX_INT64 / 2;
        static constexpr size_t MAX_STRING_SIZE    = ~MAX_INT64 / 2;
        static constexpr size_t MAX_MESH_VERTICES  = ~MAX_INT64 / 2;   // Unlimited for RMC
        static constexpr size_t MAX_MESH_INDICES   = ~MAX_INT64 / 2;   // Unlimited for RMC
        static constexpr int32_t  MAX_RECURSION_DEPTH = 1000;
        static constexpr double   EPSILON = 1e-10;
    }
}
```

Limits are intentionally extreme to support RealtimeMesh Component (RMC) with very large geometry.
