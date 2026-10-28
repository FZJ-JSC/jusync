#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogJUSYNC, Log, All)

#define JUSYNC_VERBOSE_LOGGING 0

#define JUSYNC_ENABLE_USD_PREVIEW 0

class FJUSYNCModule : public IModuleInterface {
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void DetectAndLogPlatform();
    bool InitializePlatformSpecific();
    void LogMiddlewareCapabilities();
    void RegisterModuleSystems();
    void UnregisterModuleSystems();
    void CleanupPlatformSpecific();

    bool bPlaybackDetailsRegistered = false;
    FDelegateHandle PostEngineInitHandle;

#if WITH_ANARI_USD_MIDDLEWARE
    void MaybeStartHeadlessHarness();
#endif

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
#endif

};
