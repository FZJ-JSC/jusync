#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"


DECLARE_LOG_CATEGORY_EXTERN(LogJUSYNC, Log, All)

// Verbose logging control - set to 0 for production, 1 for debugging
#define JUSYNC_VERBOSE_LOGGING 0

// USD preview extraction control - set to 0 to disable preview extraction for performance
#define JUSYNC_ENABLE_USD_PREVIEW 0


class FJUSYNCModule : public IModuleInterface {
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    // Platform detection and logging
    void DetectAndLogPlatform();
    bool InitializePlatformSpecific();
    void LogMiddlewareCapabilities();
    void RegisterModuleSystems();
    void UnregisterModuleSystems();
    void CleanupPlatformSpecific();

    // Cross-platform library validation
    bool ValidateLibraries(
        const FString& LibraryPath, const TArray<FString>& RequiredLibraries, const FString& Extension
    );
    bool AttemptLibraryLoad(const FString& FullPath, const FString& LibraryName);

#if PLATFORM_WINDOWS
    bool InitializeWindows();
    void CheckVCRedistributablesInstalled();
#endif

#if PLATFORM_LINUX
    bool InitializeLinux();
    void CheckLinuxDependencies();
#endif

    // Storage for loaded library handles
    TMap<FString, void*> LoadedLibraryHandles;
};
