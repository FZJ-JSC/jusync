#include "Platform/JUSYNCVideoMemory.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <dxgi1_4.h>  // IDXGIAdapter3 / DXGI_QUERY_VIDEO_MEMORY_INFO
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace JUSYNCVideoMemory
{
#if PLATFORM_WINDOWS
    namespace
    {
        IDXGIAdapter3* GetQueryAdapter()
        {
            static IDXGIAdapter3* Adapter = []() -> IDXGIAdapter3*
            {
                IDXGIFactory1* Factory = nullptr;
                if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&Factory))) || !Factory)
                {
                    return nullptr;
                }
                IDXGIAdapter1* Adapter1 = nullptr;
                IDXGIAdapter3* Result = nullptr;
                if (SUCCEEDED(Factory->EnumAdapters1(0, &Adapter1)) && Adapter1)
                {
                    if (FAILED(Adapter1->QueryInterface(IID_PPV_ARGS(&Result))))
                    {
                        Result = nullptr;
                    }
                    Adapter1->Release();
                }
                Factory->Release();
                return Result;
            }();
            return Adapter;
        }
    }
#endif

    bool QueryProcessLocalBudget(int64& OutFreeBytes, int64& OutBudgetBytes)
    {
        OutFreeBytes = 0;
        OutBudgetBytes = 0;
#if PLATFORM_WINDOWS
        IDXGIAdapter3* Adapter = GetQueryAdapter();
        if (!Adapter)
        {
            return false;
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO Info = {};
        if (FAILED(Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Info)))
        {
            return false;
        }
        OutBudgetBytes = static_cast<int64>(Info.Budget);
        OutFreeBytes = Info.CurrentUsage < Info.Budget
            ? static_cast<int64>(Info.Budget - Info.CurrentUsage) : 0;
        return true;
#else
        return false;
#endif
    }
}
