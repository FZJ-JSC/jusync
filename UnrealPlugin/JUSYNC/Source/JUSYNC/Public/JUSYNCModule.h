#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FJUSYNCModule : public IModuleInterface {
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    virtual bool IsGameModule() const override {
        return true;
    }

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
