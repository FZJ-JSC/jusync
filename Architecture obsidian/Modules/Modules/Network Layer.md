# Network Layer (ZmqConnector)

`ZmqConnector` — **DEALER-only stub.** No ZMQ context or socket is created. Endpoint validation utilities only.

## Files
- `include/ZmqConnector.h` (181 lines)
- `src/ZmqConnector.cpp`

## ⚠️ Current State: DEALER-Only Stub

`ZmqConnector::initialize()` does NOT create a ZMQ context. `zmqContext` and `zmqSocket` are always null. Only endpoint validation and health tracking remain functional.

`startReceiving()`/`stopReceiving()` are graceful no-ops (toggle `running` flag). Required by public API (C API, tools).

**Consequence**: ROUTER file reception is non-functional. File reception is solely via `AnariUsdClient` (DEALER) outbound requests.

## Public Methods — Current Behavior

| Method | Effect Now |
|---|---|
| `initialize()` | Validates endpoint, sets Connected status, no ZMQ context |
| `receiveFile()` | Stub — logs warning, returns `false` |
| `receiveAnyMessage()` | Stub — logs warning, returns `false` |
| `disconnect()` | Sets Disconnected status, no-ops on null context/socket |
| `getSocket()` | Returns `nullptr` |
| `isConnected()` | Returns `false` (checks `zmqSocket != nullptr`) |

## What Still Works (Utilities)

| Method | Purpose |
|---|---|
| `getCurrentEndpoint()` | Returns validated endpoint string |
| `validateEndpoint()` | Validates tcp://, ipc://, inproc:// formats |
| `getMessageStats()` | Returns snapshot (always zeros) |
| `validateFilename()` | Cross-platform filename safety checks |
| `validateHashFormatPermissive()` | Hex hash format validation |

## Endpoint Validation
```cpp
validateEndpoint(string)        // dispatches to tcp/ipc/inproc validator
validateTcpEndpoint(string)     // static const regex, port range check ✅ (was per-call, fixed)
validateIpcEndpoint(string)    // path format, no ".."
validateInprocEndpoint(string) // name format
```

## Dead Private Methods
These private helpers exist but are never called (socket is always null):
- `sendReply()`, `receiveMessagePart()`, `drainRemainingParts()` — ROUTER receive helpers
- `configurePlatformSpecificSocket()` — returns early on `!zmqSocket`
- `configureLinuxSpecific()`, `configureWindowsSpecific()` — Linux/Windows TCP settings
- `tryAlternativeEndpoints()` — fallback endpoint generator

## Design Decision Needed
Either re-enable ROUTER socket creation in `initialize()` or remove the dead ROUTER code. See [[Reference/Known Issues & Dead Code]].

See [[Modules/ZeroMQ Client]] for the active DEALER side (`AnariUsdClient`).
