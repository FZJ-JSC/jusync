#include "MemoryMonitor.h"
#include "MiddlewareLogging.h"

// Platform-specific includes
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/sysinfo.h>
#endif

namespace anari_usd_middleware {

MemoryMonitor::MemoryMonitor(size_t system_reserve_bytes) 
    : system_reserve(system_reserve_bytes) {
    
    total_system_memory = getTotalSystemMemory();
    
    MIDDLEWARE_LOG_INFO("MemoryMonitor initialized: Total RAM: %.2f GB, System reserve: %.2f GB",
                       total_system_memory / (1024.0 * 1024.0 * 1024.0),
                       system_reserve / (1024.0 * 1024.0 * 1024.0));
}

size_t MemoryMonitor::getTotalSystemMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        return static_cast<size_t>(memInfo.ullTotalPhys);
    }
    return 16ULL * 1024 * 1024 * 1024; // Default 16GB if query fails
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<size_t>(pages) * static_cast<size_t>(page_size);
    }
    return 16ULL * 1024 * 1024 * 1024; // Default 16GB
#endif
}

size_t MemoryMonitor::getAvailableMemory() const {
    size_t available = queryAvailableMemory();
    
    // Subtract our allocations and system reserve
    size_t allocated = allocated_memory.load();
    
    if (available > allocated + system_reserve) {
        return available - allocated - system_reserve;
    }
    return 0;
}

bool MemoryMonitor::canAllocate(size_t size) const {
    if (size == 0) return true;
    
    size_t available = getAvailableMemory();
    return available >= size;
}

bool MemoryMonitor::allocate(size_t size) {
    if (size == 0) return true;
    
    std::lock_guard<std::mutex> lock(allocation_mutex);
    
    if (!canAllocate(size)) {
        MIDDLEWARE_LOG_WARNING("Cannot allocate %zu bytes. Available: %zu bytes",
                              size, getAvailableMemory());
        return false;
    }
    
    allocated_memory.fetch_add(size);
    MIDDLEWARE_LOG_DEBUG("Allocated %zu bytes. Total allocated: %zu bytes",
                        size, allocated_memory.load());
    return true;
}

void MemoryMonitor::release(size_t size) {
    if (size == 0) return;
    
    std::lock_guard<std::mutex> lock(allocation_mutex);
    
    size_t current = allocated_memory.load();
    if (size > current) {
        MIDDLEWARE_LOG_WARNING("Trying to release %zu bytes but only %zu allocated",
                              size, current);
        allocated_memory.store(0);
    } else {
        allocated_memory.fetch_sub(size);
    }
    
    MIDDLEWARE_LOG_DEBUG("Released %zu bytes. Total allocated: %zu bytes",
                        size, allocated_memory.load());
}

float MemoryMonitor::getUsagePercentage() const {
    size_t allocated = allocated_memory.load();
    if (total_system_memory == 0) return 0.0f;
    
    return (static_cast<float>(allocated) / total_system_memory) * 100.0f;
}

size_t MemoryMonitor::queryAvailableMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        return static_cast<size_t>(memInfo.ullAvailPhys);
    }
    return getTotalSystemMemory(); // Fallback using static method
#else
    struct sysinfo memInfo;
    if (sysinfo(&memInfo) == 0) {
        return static_cast<size_t>(memInfo.freeram) * memInfo.mem_unit;
    }
    return getTotalSystemMemory(); // Fallback using static method
#endif
}

} // namespace anari_usd_middleware