#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace jusync_diag {

enum Stage {
    STAGE_NOTIFY = 0,
    STAGE_QUEUE,
    STAGE_DL_START,
    STAGE_DL_DONE,
    STAGE_PARSE_START,
    STAGE_PARSE_DONE,
    STAGE_FILL_DONE,
    STAGE_SPAWNED,
    STAGE_COUNT
};

inline const char* stageName(int s) {
    static const char* names[STAGE_COUNT] = {
        "notify", "queue", "dl_start", "dl_done",
        "parse_start", "parse_done", "fill_done", "spawned"
    };
    return (s >= 0 && s < STAGE_COUNT) ? names[s] : "?";
}

class Diagnostics {
public:
    static constexpr size_t kCapacity = 512;

    static Diagnostics& instance() {
        static Diagnostics d;
        return d;
    }

    static int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void setEnabled(bool v) { enabled_.store(v, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    void mark(const std::string& file, int stage, uint64_t bytes = 0,
              uint64_t hashLo = 0, uint64_t hashHi = 0) {
        if (!enabled() || file.empty() || stage < 0 || stage >= STAGE_COUNT) return;
        const int64_t t = nowNs();
        std::lock_guard<std::mutex> lock(mu_);
        auto it = traces_.find(file);
        if (it == traces_.end()) {
            it = traces_.emplace(file, Trace{}).first;
            it->second.filename = file;
            order_.push_back(file);
            while (order_.size() > kCapacity) {
                traces_.erase(order_.front());
                order_.pop_front();
            }
        } else if (stage == STAGE_NOTIFY) {
            it->second.t.fill(0);
            it->second.bytes = 0;
            it->second.hashLo = 0;
            it->second.hashHi = 0;
        }
        Trace& tr = it->second;
        if (tr.t[stage] == 0) tr.t[stage] = t;
        if (bytes)  tr.bytes = bytes;
        if (hashLo) tr.hashLo = hashLo;
        if (hashHi) tr.hashHi = hashHi;
    }

    double gapMs(const std::string& file, int from, int to) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = traces_.find(file);
        return (it == traces_.end()) ? -1.0 : diffMs(it->second.t, from, to);
    }

    double totalMs(const std::string& file) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = traces_.find(file);
        return (it == traces_.end()) ? -1.0 : totalMsLocked(it->second);
    }

    std::string summary(const std::string& file) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = traces_.find(file);
        if (it == traces_.end()) return std::string();
        const Trace& tr = it->second;
        char line[640];
        std::snprintf(line, sizeof(line),
            "%s %.0fKB q=%.1f dl=%.1f parse=%.1f fill=%.1f spawn=%.1f total=%.1f ms%s",
            baseName(tr.filename), tr.bytes / 1024.0,
            diffMs(tr.t, STAGE_NOTIFY, STAGE_QUEUE),
            diffMs(tr.t, STAGE_DL_START, STAGE_DL_DONE),
            diffMs(tr.t, STAGE_PARSE_START, STAGE_PARSE_DONE),
            diffMs(tr.t, STAGE_PARSE_DONE, STAGE_FILL_DONE),
            diffMs(tr.t, STAGE_FILL_DONE, STAGE_SPAWNED),
            totalMsLocked(tr),
            tr.t[STAGE_SPAWNED] ? "" : " (in flight)");
        return std::string(line);
    }

    std::string dump(size_t lastN = 20) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::string out;
        const size_t n = order_.size();
        const size_t begin = (n > lastN) ? n - lastN : 0;
        char line[512];
        std::snprintf(line, sizeof(line),
            "%-40s %10s %7s %9s %8s %7s %8s %9s\n",
            "file", "KB", "queue", "transfer", "parse", "fill", "spawn", "total");
        out += line;
        for (size_t i = begin; i < n; ++i) {
            auto it = traces_.find(order_[i]);
            if (it == traces_.end()) continue;
            const Trace& tr = it->second;
            std::string name(baseName(tr.filename));
            if (name.size() > 40) name = name.substr(name.size() - 40);
            std::snprintf(line, sizeof(line),
                "%-40s %10.0f %7.1f %9.1f %8.1f %7.1f %8.1f %9.1f\n",
                name.c_str(), tr.bytes / 1024.0,
                diffMs(tr.t, STAGE_NOTIFY, STAGE_QUEUE),
                diffMs(tr.t, STAGE_DL_START, STAGE_DL_DONE),
                diffMs(tr.t, STAGE_PARSE_START, STAGE_PARSE_DONE),
                diffMs(tr.t, STAGE_PARSE_DONE, STAGE_FILL_DONE),
                diffMs(tr.t, STAGE_FILL_DONE, STAGE_SPAWNED),
                totalMsLocked(tr));
            out += line;
        }
        return out;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        traces_.clear();
        order_.clear();
    }

private:
    struct Trace {
        std::string filename;
        std::array<int64_t, STAGE_COUNT> t{};
        uint64_t bytes = 0;
        uint64_t hashLo = 0;
        uint64_t hashHi = 0;
        Trace() { t.fill(0); }
    };

    static double diffMs(const std::array<int64_t, STAGE_COUNT>& t, int a, int b) {
        if (a < 0 || a >= STAGE_COUNT || b < 0 || b >= STAGE_COUNT) return -1.0;
        if (t[a] == 0 || t[b] == 0 || t[b] < t[a]) return -1.0;
        return static_cast<double>(t[b] - t[a]) / 1e6;
    }

    static double totalMsLocked(const Trace& tr) {
        int64_t lo = 0, hi = 0;
        for (int i = 0; i < STAGE_COUNT; ++i) {
            const int64_t v = tr.t[i];
            if (v == 0) continue;
            if (lo == 0 || v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (lo == 0) return -1.0;
        return static_cast<double>(hi - lo) / 1e6;
    }

    static const char* baseName(const std::string& f) {
        const size_t p = f.find_last_of("/\\");
        return (p == std::string::npos) ? f.c_str() : f.c_str() + p + 1;
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, Trace> traces_;
    std::deque<std::string> order_;
    std::atomic<bool> enabled_{true};
};

} // namespace jusync_diag
