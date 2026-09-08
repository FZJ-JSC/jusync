# C Interface

Flat C API wrapper for FFI (Foreign Function Interface) compatibility. Allows embedding the middleware in languages/processes without C++ ABI linkage.

## Files
- `include/AnariUsdMiddleware_C.h`
- `src/AnariUsdMiddleware_C.cpp`

## Purpose
- Exposes all core middleware functionality via `extern "C"` linkage
- Safe memory management — no premature deallocation after v1.0.1 fix
- Pointer validation via `MIDDLEWARE_VALIDATE_POINTER` macros
- `ANARI_USD_MIDDLEWARE_API` export macros for DLL/shared lib exports

## Design
The C API wraps the C++ `AnariUsdMiddleware` using opaque handles:

```c
// Opaque handle — consumer never includes C++ headers
typedef struct AnariUsdMiddlewareHandle AnariUsdMiddlewareHandle;

ANARI_USD_MIDDLEWARE_API AnariUsdMiddlewareHandle* CreateMiddleware(void);
ANARI_USD_MIDDLEWARE_API void DestroyMiddleware(AnariUsdMiddlewareHandle*);
ANARI_USD_MIDDLEWARE_API bool InitializeMiddleware(AnariUsdMiddlewareHandle*, const char* endpoint);
ANARI_USD_MIDDLEWARE_API void ShutdownMiddleware(AnariUsdMiddlewareHandle*);
```

## Fixed Issues (v1.0.1)
- **Premature memory deallocation** — C API was freeing internal buffers before consumer finished reading
- **Memory access violations** — Fixed by ensuring lifecycle: allocate → pass to C caller → C caller frees via dedicated free function
- **Duplicate API macros** — Fixed `ANARI_USD_MIDDLEWARE_API` `#ifndef` guards across headers

## Safety Macros Used
- `MIDDLEWARE_VALIDATE_POINTER(ptr, "C_API")` — Null check on all C handles before use
- `MIDDLEWARE_SAFE_DELETE(ptr)` — Null-safe delete + nullptr assignment
- `MIDDLEWARE_SAFE_ARRAY_ACCESS(arr, idx, "C_API")` — Bounds check for array returns

## Export Macros
```cpp
#ifdef _WIN32
    #ifdef ANARI_USD_MIDDLEWARE_EXPORTS
        #define ANARI_USD_MIDDLEWARE_API __declspec(dllexport)
    #else
        #define ANARI_USD_MIDDLEWARE_API __declspec(dllimport)
    #endif
#else
    #define ANARI_USD_MIDDLEWARE_API __attribute__((visibility("default")))
#endif
```

The Linux build sets `visibility("default")` in `CMakeLists.txt` via `-DANARI_USD_MIDDLEWARE_API=__attribute__((visibility("default")))`.

See [[Modules/Middleware Core]] for the underlying C++ API this wraps.
