#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

// Add Windows API includes for DLL checking (Windows only)
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

class FJUSYNCModule : public IModuleInterface {
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    virtual bool IsGameModule() const override {
        return true;
    }

private:
    // Helper function to check VC++ redistributables (Windows only)
#if PLATFORM_WINDOWS
    void CheckVCRedistributablesInstalled();
#endif
};
