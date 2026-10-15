#pragma once

#include <cstddef>
#include <atomic>
#include <mutex>

namespace anari_usd_middleware {

class MemoryMonitor {
public:
    explicit MemoryMonitor(size_t system_reserve_bytes = 1024 * 1024 * 1024);

    static size_t getTotalSystemMemory();

    size_t getAvailableMemory() const;

    bool canAllocate(size_t size) const;

    bool allocate(size_t size);

    void release(size_t size);

    size_t getAllocatedMemory() const { return allocated_memory.load(); }

    float getUsagePercentage() const;

private:
    size_t total_system_memory;
    size_t system_reserve;
    std::atomic<size_t> allocated_memory{0};
    mutable std::mutex allocation_mutex;

    static size_t queryAvailableMemory();
};

} // namespace anari_usd_middleware