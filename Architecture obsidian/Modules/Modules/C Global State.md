# C Interface -- Global State

The C interface (`AnariUsdMiddleware_C.cpp`) uses a **single global instance** model. All C API functions operate on `g_middleware`, a process-scope `unique_ptr`.

## Files
- `include/AnariUsdMiddleware_C.h` — C function declarations
- `src/AnariUsdMiddleware_C.cpp` (2372 lines) — Global state + implementation

## Global State

```cpp
// Process-scope middleware instance
static std::unique_ptr<AnariUsdMiddleware> g_middleware;

// Process-scope collision processor
static std::unique_ptr<CollisionProcessor> g_collision_processor;

// Atomic C function pointers (thread-safe from ZMQ callback threads)
static std::atomic<FileReceivedCallback_C> g_file_callback = nullptr;
static std::atomic<MessageReceivedCallback_C> g_message_callback = nullptr;

// Default collision complexity
static int g_default_collision_complexity = COLLISION_COMPLEX;

// Mutex for g_middleware / g_collision_processor access
static std::mutex g_middleware_mutex;
```

## Lifecycle

### 1. Pre-Init: Set Callbacks (Optional)
```c
SetFileReceivedCallback_C(myCallback);
// Callback stored in g_file_callback (atomic store)
```

### 2. Initialize
```c
int result = InitializeMiddleware_C("tcp://*:5556");
// Creates g_middleware and g_collision_processor if not exists
// Calls g_middleware->initialize()
// Registers callbacks (if g_file_callback was set)
```

### 3. Use
```c
int status = GetMiddlewareStatus_C();
// Locks g_middleware_mutex, forwards to g_middleware->isConnected()
```

### 4. Shutdown
```c
ShutdownMiddleware_C();
// Calls g_middleware->shutdown()
// Does NOT destroy g_middleware (can re-initialize)
```

## Thread Safety

- `g_middleware_mutex` protects `g_middleware` and `g_collision_processor` access on all public API functions
- `g_file_callback` is `std::atomic<FunctionPtr>` — thread-safe from ZMQ callback threads
- Callbacks fire on background ZMQ threads; consumer must re-entrant-safe

## Callback Memory Management

C callback `CFileData` allocates `data` with `new[]`:

```c
c_data.data = new unsigned char[c_data.data_size];
std::memcpy(c_data.data, file_data.data.data(), c_data.data_size);

// ... call callback ...

// Clean up AFTER callback returns (critical fix in v1.0.1)
delete[] c_data.data;
c_data.data = nullptr;
```

The callback **must copy** any data it needs to keep — the pointer becomes dangling after the callback returns.

## String Handling

Cross-platform safe string copying:

```cpp
#ifdef _WIN32
    strncpy_s(c_data.filename, sizeof(c_data.filename), src.c_str(), _TRUNCATE);
#else
    snprintf(c_data.filename, sizeof(c_data.filename), "%s", src.c_str());
#endif
```

All fixed-size char arrays are null-terminated.

## Export Macros

`ANARI_USD_MIDDLEWARE_EXPORTS` is defined at the top of `AnariUsdMiddleware_C.cpp` to ensure `__declspec(dllexport)` on Windows. See [[Modules/Logging System]] for macro details.

See [[Modules/C Interface]] for FFI wrapper overview.
