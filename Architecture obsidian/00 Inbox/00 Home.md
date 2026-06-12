# JUSYNC Architecture

High-performance C++ middleware library for real-time streaming of USD (Universal Scene Description) data over ZeroMQ to ANARI-based rendering pipelines and Unreal Engine.

Built as part of the **InHPC-DE** project (BMFTR / MWK-NRW, Gauss Centre for Supercomputing).

---

## Navigation

### Getting Started
- [[01 Getting Started]]
- [[02 Architecture Overview]]

### For AI/LLM Agents
- [[Reference/LLM Architecture Guide]] — File map, design rules, how to navigate this vault
- [[Reference/Data Flow]] — End-to-end traces (ROUTER push, DEALER pull, parallel streaming)
- [[Reference/Edit Guide]] — "I need to change X" → which file to edit
- [[Reference/External Dependencies]] — Bundled libs in `external/`
- [[Reference/Known Issues & Dead Code]] — Audit findings: dead code, bugs, incomplete impls

### Modules
- [[Modules/Middleware Core]]
- [[Modules/USD Processor]]
- [[Modules/Point Cloud]]
- [[Modules/Color Interpolation]]
- [[Modules/Network Layer]]
- [[Modules/ZeroMQ Client]]
- [[Modules/Message Protocol]]
- [[Modules/Collision Processor]]
- [[Modules/CUDA GPU]]
- [[Modules/Hash Verifier]]
- [[Modules/Parallel Downloader]]
- [[Modules/Memory Monitor]]
- [[Modules/C Interface]]
- [[Modules/C Global State]]
- [[Modules/Logging System]]

### Build & Test
- [[Build/03 Build System]]
- [[Build/04 API Reference]]
- [[Build/05 Network Protocol]]
- [[Build/06 Unreal Plugin]]
- [[Build/07 Testing]]
- [[Build/08 Troubleshooting]]
