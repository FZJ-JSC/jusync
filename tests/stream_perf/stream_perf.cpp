// stream_perf: middleware-level performance harness for the P1/P2 streaming
// work (the same C API surface the UE plugin drives). Run against a live
// broker (e.g. the jusync-sender docker rig) to measure, without UE:
//   1. connect latency + protocol version
//   2. file-list round trip, payload sizes, crate-vs-ASCII magic sniff (P1)
//   3. sequential download latency/throughput (loopback upper bound)
//   4. abort-token responsiveness + early-abort exit (P2 mechanism)
//   5. update-callback flow across an idle window (notifications keep coming)
//   6. final JusyncDiag journal dump
//
// build:  cmake --build build-diag --config Release --target stream_perf
// run:    stream_perf.exe [tcp://localhost:5556] [idle-seconds]

#include "AnariUsdMiddleware_C.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static double MsSince(const Clock::time_point& t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static std::atomic<int> g_updates{0};
static void OnFileReceived(const CFileData* /*file_data*/) { ++g_updates; }

// Raw broker pushes: NOTIFY_FILE_UPDATE=300, NOTIFY_COMMIT_COMPLETE=301,
// NOTIFY_FILE_UPDATE_V2=302, NOTIFY_SCENE_UPDATE=303, NOTIFY_PROPERTY_UPDATE=304.
// Count by type to tell "broker stayed silent" (hash dedup) from
// "client dropped pushes" (callback wiring).
static std::atomic<int> g_notifs{0};
static std::atomic<int> g_notifFileUpdate{0};
static std::atomic<int> g_notifCommitComplete{0};
static std::atomic<int> g_notifSceneUpdate{0};
static std::atomic<int> g_notifOther{0};
static void OnNotification(uint32_t message_type, int32_t /*source_rank*/,
                           const char* /*filename*/, uint64_t /*file_size*/,
                           uint64_t /*timestamp*/, uint64_t /*hashLo*/,
                           uint64_t /*hashHi*/, uint64_t /*hashPrevLo*/,
                           uint64_t /*hashPrevHi*/, bool /*hasOldData*/)
{
    ++g_notifs;
    switch (message_type)
    {
    case 300: case 302: ++g_notifFileUpdate; break;
    case 301: ++g_notifCommitComplete; break;
    case 303: ++g_notifSceneUpdate; break;
    default: ++g_notifOther; break;
    }
}

int main(int argc, char** argv)
{
    const char* endpoint = (argc > 1) ? argv[1] : "tcp://localhost:5556";
    const int idleSeconds = (argc > 2) ? std::atoi(argv[2]) : 30;
    const int32_t rank = 0; // single-rank self-service rig

    // ---- 1. connect ----
    auto t0 = Clock::now();
    if (InitializeMiddleware_C(endpoint) != 1)
    {
        std::printf("initialize FAILED (%s)\n", endpoint);
        return 1;
    }
    std::printf("== connect ==\n");
    std::printf("connect+init: %.1f ms  IsConnected=%d  protocol=%u\n",
                MsSince(t0), IsConnected_C(), GetProtocolVersion_C());

    // UE order: InitializeMiddleware_C -> ConnectToBroker_C (this is where
    // AnariUsdClient is created lazily!) -> register callbacks (must come
    // AFTER connect or they attach to nothing) -> StartReceiving_C. The broker
    // lives inside the sender process, so it briefly disappears once per loop
    // pass: retry the list.
    t0 = Clock::now();
    const int connRc = ConnectToBroker_C(endpoint, 5000);
    std::printf("ConnectToBroker_C: rc=%d in %.1f ms (brokerConnected=%d)\n",
                connRc, MsSince(t0), IsBrokerConnected_C());
    if (connRc != 1)
    {
        std::printf("connect FAILED (%s)\n", endpoint);
        ShutdownMiddleware_C();
        return 1;
    }
    RegisterUpdateCallback_C(OnFileReceived);
    RegisterNotificationCallback_C(OnNotification);
    if (StartReceiving_C() != 1)
    {
        std::printf("StartReceiving FAILED\n");
        ShutdownMiddleware_C();
        return 1;
    }

    // ---- 2. file list ----
    char** names = nullptr;
    uint64_t* sizes = nullptr;
    size_t count = 0;
    int listOk = 0;
    double listMs = 0.0;
    for (int attempt = 0; attempt < 5 && !listOk; ++attempt)
    {
        t0 = Clock::now();
        listOk = RequestFileListWithSizes_C(-1, &names, &sizes, &count, 8000);
        listMs = MsSince(t0);
        if (!listOk) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    std::printf("== file list ==\n");
    std::printf("request list: %.1f ms -> %zu files (rc=%d)\n", listMs, listOk ? count : 0, listOk);
    if (!listOk || count == 0)
    {
        std::printf("no files at broker (is the sender rig running?)\n");
        StopReceiving_C();
        ShutdownMiddleware_C();
        return 1;
    }
    uint64_t listedBytes = 0;
    size_t largest = 0;
    for (size_t i = 0; i < count; ++i)
    {
        listedBytes += sizes[i];
        if (sizes[i] > sizes[largest]) largest = i;
    }
    std::printf("listed payload: %llu bytes total across %zu files (largest: %s)\n",
                (unsigned long long)listedBytes, count, names[largest]);

    // ---- 3. sequential downloads ----
    std::printf("== downloads (sequential, loopback) ==\n");
    std::vector<unsigned char> buf(8u * 1024u * 1024u);
    size_t okCount = 0, gotBytes = 0;
    size_t crates = 0, ascii = 0, other = 0;
    double worstMs = 0.0;
    t0 = Clock::now();
    for (size_t i = 0; i < count; ++i)
    {
        size_t n = 0;
        const auto f0 = Clock::now();
        const int64_t rc = RequestFileIntoBuffer_C(names[i], rank, 8000, buf.data(), buf.size(), &n);
        const double dms = MsSince(f0);
        if (rc > 0)
        {
            ++okCount;
            gotBytes += n;
            if (dms > worstMs) worstMs = dms;
            const char* kind = "other";
            if (n >= 8 && std::memcmp(buf.data(), "PXR-USDC", 8) == 0) { kind = "crate"; ++crates; }
            else if (n >= 5 && std::memcmp(buf.data(), "#usda", 5) == 0) { kind = "ascii"; ++ascii; }
            else ++other;
            std::printf("  %8.2f ms  %9zu B  %-5s  %s\n", dms, n, kind, names[i]);
            if (i < 4 || i % 10 == 0)
            {
                std::printf("      head:");
                for (size_t b = 0; b < (n < 24 ? n : 24); ++b) std::printf(" %02x", buf[b]);
                std::printf("  |");
                for (size_t b = 0; b < (n < 24 ? n : 24); ++b) std::printf("%c", (buf[b] >= 32 && buf[b] < 127) ? buf[b] : '.');
                std::printf("|\n");
            }
        }
        else
        {
            std::printf("  FAILED rc=%lld  %s\n", (long long)rc, names[i]);
        }
    }
    const double dlMs = MsSince(t0);
    std::printf("downloads: %zu ok, %zu B in %.1f ms (%.2f MB/s loopback upper bound), worst file %.2f ms\n",
                okCount, gotBytes, dlMs, gotBytes / (1024.0 * 1024.0) / (dlMs / 1000.0), worstMs);
    std::printf("payload kinds: crate=%zu ascii=%zu other=%zu (P1 expects all crate)\n", crates, ascii, other);

    // ---- 4. abort tokens ----
    // Small loopback files finish before any mid-transfer abort can bite, so
    // this proves the token MECHANISM: a pre-fired token exits the download
    // immediately (-1), and post-completion triggers are harmless no-ops.
    std::printf("== abort tokens (P2 mechanism) ==\n");
    {
        const uint64_t h = JusyncAbortCreate_C();
        JusyncAbortTrigger_C(h); // fire BEFORE the download even starts
        size_t n = 0;
        const auto a0 = Clock::now();
        const int64_t rc = RequestFileIntoBufferAbort_C(names[largest], rank, 8000, buf.data(), buf.size(), &n, h);
        std::printf("  pre-fired token: returned in %.1f ms rc=%lld size=%zu (want fast rc=-1)\n",
                    MsSince(a0), (long long)rc, n);
        JusyncAbortRelease_C(h);
    }
    for (const int delay : {0, 30, 80})
    {
        const uint64_t h = JusyncAbortCreate_C();
        std::atomic<bool> fired{false};
        std::thread killer([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            JusyncAbortTrigger_C(h);
            fired = true;
        });
        size_t n = 0;
        const auto a0 = Clock::now();
        const int64_t rc = RequestFileIntoBufferAbort_C(names[largest], rank, 8000, buf.data(), buf.size(), &n, h);
        const double retMs = MsSince(a0);
        killer.join();
        std::printf("  abort delay=%3d ms: returned in %8.1f ms rc=%lld size=%zu\n",
                    delay, retMs, (long long)rc, n);
        JusyncAbortRelease_C(h);
    }

    // ---- 5. notification flow over idle window ----
    std::printf("== notifications (%d s idle window, sender should keep looping) ==\n", idleSeconds);
    {
        const int before = g_notifs.load();
        const auto w0 = Clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(idleSeconds));
        const double secs = MsSince(w0) / 1000.0;
        std::printf("broker notifications: %d total in %.1f s (%.2f/s)  file-update=%d commit-complete=%d scene-update=%d other=%d  update-callbacks=%d\n",
                    g_notifs.load() - before, secs, (g_notifs.load() - before) / secs,
                    g_notifFileUpdate.load(), g_notifCommitComplete.load(),
                    g_notifSceneUpdate.load(), g_notifOther.load(), g_updates.load());
    }

    // ---- 6. diag journal ----
    JusyncDiagSetEnabled_C(1);
    static char dump[65536];
    const int dn = JusyncDiagDump_C(dump, sizeof(dump) - 1, 15);
    if (dn > 0)
    {
        dump[dn] = 0;
        std::printf("== diag journal (last %d) ==\n%s", 15, dump);
    }
    else
    {
        std::printf("== diag journal: empty (marks are host-driven; UE adds queue/spawn stages) ==\n");
    }

    StopReceiving_C();
    ShutdownMiddleware_C();
    std::printf("done.\n");
    return 0;
}
