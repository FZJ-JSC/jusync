#include "MemoryMonitor.h"
#include "MiddlewareLogging.h"

// Platform-specific includes
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/sysinfo.h>
#include <fstream>
#include <string>
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
    size_t systemAvail = queryAvailableMemory();
    size_t allocated = allocated_memory.load();
    if (systemAvail > allocated + system_reserve) {
        size_t freeForUs = systemAvail - allocated - system_reserve;
        return freeForUs >= size;
    }
    return false;
}

bool MemoryMonitor::allocate(size_t size) {
    if (size == 0) return true;

    std::lock_guard<std::mutex> lock(allocation_mutex);

    size_t currentAllocated = allocated_memory.load();
    size_t systemAvail = queryAvailableMemory();
    size_t freeForUs = (systemAvail > currentAllocated + system_reserve)
        ? systemAvail - currentAllocated - system_reserve : 0;

    if (freeForUs < size) {
        MIDDLEWARE_LOG_WARNING("Cannot allocate %zu bytes. Free for us: %zu bytes",
                              size, freeForUs);
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
    // Read MemAvailable from /proc/meminfo (includes reclaimable caches)
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.find("MemAvailable:") == 0) {
            // Format: "MemAvailable:   12345678 kB"
            size_t kb = 0;
            for (char c : line) {
                if (c >= '0' && c <= '9') {
                    kb = kb * 10 + (c - '0');
                }
            }
            return kb * 1024;
        }
    }
    // Fallback: sysinfo (less accurate)
    struct sysinfo memInfo;
    if (sysinfo(&memInfo) == 0) {
        return static_cast<size_t>(memInfo.freeram) * memInfo.mem_unit;
    }
    return getTotalSystemMemory(); // Fallback using static method
#endif
}

} // namespace anari_usd_middleware