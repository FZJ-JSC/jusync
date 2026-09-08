#pragma once

#include <cstddef>
#include <atomic>
#include <mutex>

namespace anari_usd_middleware {

/**
 * @brief Monitors available system RAM and tracks allocations
 * 
 * This class provides RAM-aware memory management for parallel downloads.
 * It tracks how much memory is currently allocated and prevents
 * allocations that would exceed available system RAM.
 */
class MemoryMonitor {
public:
    /**
     * @brief Construct a MemoryMonitor with system reserve
     * @param system_reserve_bytes Amount of RAM to reserve for system (default: 1GB)
     */
    explicit MemoryMonitor(size_t system_reserve_bytes = 1024 * 1024 * 1024);
    
    /**
     * @brief Get total system RAM in bytes
     */
    static size_t getTotalSystemMemory();
    
    /**
     * @brief Get currently available RAM in bytes
     */
    size_t getAvailableMemory() const;
    
    /**
     * @brief Check if we can allocate size bytes
     * @param size Bytes to allocate
     * @return true if allocation would succeed
     */
    bool canAllocate(size_t size) const;
    
    /**
     * @brief Reserve memory (call before allocation)
     * @param size Bytes to reserve
     * @return true if reservation succeeded
     */
    bool allocate(size_t size);
    
    /**
     * @brief Release reserved memory (call after deallocation)
     * @param size Bytes to release
     */
    void release(size_t size);
    
    /**
     * @brief Get currently allocated memory in bytes
     */
    size_t getAllocatedMemory() const { return allocated_memory.load(); }
    
    /**
     * @brief Get memory usage percentage (0-100)
     */
    float getUsagePercentage() const;
    
private:
    size_t total_system_memory;
    size_t system_reserve;
    std::atomic<size_t> allocated_memory{0};
    mutable std::mutex allocation_mutex;
    
    // Platform-specific memory query
    static size_t queryAvailableMemory();
};

} // namespace anari_usd_middleware