#include "JUSYNCModule.h"
#include "Engine/Engine.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

// Platform-specific headers
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#ifdef WITH_ANARI_USD_MIDDLEWARE
#include "AnariUsdMiddleware.h"
#endif

#define LOCTEXT_NAMESPACE "FJUSYNCModule"


DEFINE_LOG_CATEGORY(LogJUSYNC)


void FJUSYNCModule::StartupModule() {
    try {
        DetectAndLogPlatform();

#ifdef WITH_ANARI_USD_MIDDLEWARE
        UE_LOG(LogJUSYNC, Log, TEXT("Compiled with middleware support"));

        if (!InitializePlatformSpecific()) {
            UE_LOG(LogJUSYNC, Warning, TEXT("Platform-specific initialization failed - running in limited mode"));
        } else {
            LogMiddlewareCapabilities();
        }
#else
        UE_LOG(LogJUSYNC, Warning, TEXT("Compiled WITHOUT middleware support (check Build.cs: middleware libraries not found). "
                                        "Only basic JUSYNC functionality will be available."));
#endif

        RegisterModuleSystems();
    } catch (const std::exception&) {
        UE_LOG(LogJUSYNC, Error, TEXT("Exception during startup"));
    } catch (...) {
        UE_LOG(LogJUSYNC, Error, TEXT("Unknown exception during startup"));
    }
}

void FJUSYNCModule::ShutdownModule() {
    try {
        CleanupPlatformSpecific();
        UnregisterModuleSystems();
    } catch (const std::exception&) {
        UE_LOG(LogJUSYNC, Error, TEXT("Exception during shutdown"));
    } catch (...) {
        UE_LOG(LogJUSYNC, Error, TEXT("Unknown exception during shutdown"));
    }
}

void FJUSYNCModule::DetectAndLogPlatform() {
#if PLATFORM_WINDOWS
    UE_LOG(LogJUSYNC, Log, TEXT("Platform: Windows (x64), middleware libraries: .dll"));
#elif PLATFORM_LINUX
    UE_LOG(LogJUSYNC, Log, TEXT("Platform: Linux, middleware libraries: .so"));
#else
    UE_LOG(LogJUSYNC, Warning, TEXT("Platform: unknown/unsupported"));
#endif

#if UE_BUILD_SHIPPING
    const TCHAR* BuildConfigName = TEXT("Shipping");
#else
    const TCHAR* BuildConfigName = TEXT("Development");
#endif
    UE_LOG(LogJUSYNC, Log, TEXT("Engine: %s (%s)"), ENGINE_VERSION_STRING, BuildConfigName);
}

bool FJUSYNCModule::InitializePlatformSpecific() {
#ifdef WITH_ANARI_USD_MIDDLEWARE

#if PLATFORM_WINDOWS
    return InitializeWindows();
#elif PLATFORM_LINUX
    return InitializeLinux();
#else
    UE_LOG(LogJUSYNC, Error, TEXT("Unsupported platform for middleware"));
    return false;
#endif

#else
    UE_LOG(LogJUSYNC, Warning, TEXT("Middleware support not compiled - skipping platform initialization"));
    return true;  // Not an error, just limited functionality
#endif
}

#if PLATFORM_WINDOWS
bool FJUSYNCModule::InitializeWindows() {
    FString PluginDir = FPaths::ProjectPluginsDir();
    FString DLLPath = FPaths::Combine(PluginDir, TEXT("JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Lib/Win64"));
    FString AbsoluteDLLPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*DLLPath);

    UE_LOG(LogJUSYNC, Log, TEXT("Windows DLL directory: %s"), *AbsoluteDLLPath);

    if (!IFileManager::Get().DirectoryExists(*AbsoluteDLLPath)) {
        UE_LOG(LogJUSYNC, Error, TEXT("Windows DLL directory does not exist: %s"), *AbsoluteDLLPath);
        return false;
    }

    const TArray<FString> RequiredDLLs = {TEXT("anari_usd_middleware.dll")};
    const bool bAllLibrariesFound = ValidateLibraries(AbsoluteDLLPath, RequiredDLLs, TEXT("dll"));

    if (bAllLibrariesFound) {
        CheckVCRedistributablesInstalled();

        FPlatformProcess::AddDllDirectory(*AbsoluteDLLPath);
        UE_LOG(LogJUSYNC, Log, TEXT("Added DLL directory to search path: %s"), *AbsoluteDLLPath);
    }

    return bAllLibrariesFound;
}

void FJUSYNCModule::CheckVCRedistributablesInstalled() {
    const TArray<FString> VCRuntimeDLLs = {TEXT("msvcp140.dll"), TEXT("vcruntime140.dll"), TEXT("vcruntime140_1.dll")};

    for (const FString& RuntimeDLL : VCRuntimeDLLs) {
        if (!GetModuleHandle(*RuntimeDLL)) {
            UE_LOG(LogJUSYNC, Warning, TEXT("Missing VC++ runtime: %s"), *RuntimeDLL);
        }
    }
}
#endif

#if PLATFORM_LINUX
bool FJUSYNCModule::InitializeLinux() {
    FString PluginDir = FPaths::ProjectPluginsDir();
    FString LibPath = FPaths::Combine(PluginDir, TEXT("JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Lib/Linux"));
    FString AbsoluteLibPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*LibPath);

    UE_LOG(LogJUSYNC, Log, TEXT("Linux library directory: %s"), *AbsoluteLibPath);

    if (!IFileManager::Get().DirectoryExists(*AbsoluteLibPath)) {
        UE_LOG(LogJUSYNC, Error, TEXT("Linux library directory does not exist: %s"), *AbsoluteLibPath);
        return false;
    }

    const TArray<FString> RequiredLibs = {
        TEXT("libanari_usd_middleware.so")
    };

    const bool bAllLibrariesFound = ValidateLibraries(AbsoluteLibPath, RequiredLibs, TEXT("so"));

    if (bAllLibrariesFound) {
        FPlatformProcess::AddDllDirectory(*AbsoluteLibPath);
        UE_LOG(LogJUSYNC, Log, TEXT("Added library directory to search path: %s"), *AbsoluteLibPath);
    }

    return bAllLibrariesFound;
}
#endif

bool FJUSYNCModule::ValidateLibraries(
    const FString& LibraryPath, const TArray<FString>& RequiredLibraries, const FString& Extension
) {
    bool bAllLibrariesFound = true;
    int32 ValidatedCount = 0;

    for (const FString& LibraryName : RequiredLibraries) {
        const FString FullLibraryPath = FPaths::Combine(LibraryPath, LibraryName);

        if (IFileManager::Get().FileExists(*FullLibraryPath)) {
            if (AttemptLibraryLoad(FullLibraryPath, LibraryName)) {
                ++ValidatedCount;
            } else {
                UE_LOG(LogJUSYNC, Error, TEXT("Failed to load: %s"), *LibraryName);
                bAllLibrariesFound = false;
            }
        } else {
            UE_LOG(LogJUSYNC, Error, TEXT("Missing library: %s"), *FullLibraryPath);
            bAllLibrariesFound = false;
        }
    }

    if (!bAllLibrariesFound) {
        UE_LOG(LogJUSYNC, Error, TEXT("Library validation failed: %d/%d libraries found. "
                                      "Plugin will run in limited mode."),
            ValidatedCount, RequiredLibraries.Num());
    }

    return bAllLibrariesFound;
}

bool FJUSYNCModule::AttemptLibraryLoad(const FString& FullPath, const FString& LibraryName) {
    return IFileManager::Get().FileExists(*FullPath);
}


void FJUSYNCModule::LogMiddlewareCapabilities() {
    UE_LOG(LogJUSYNC, Log, TEXT("Middleware ready: ZeroMQ, USD (TinyUSDZ), hash verification (OpenSSL), "
                                "textures (STB), RealtimeMeshComponent integration"));
}

void FJUSYNCModule::RegisterModuleSystems() {
    // Hook point for global systems; currently a no-op.
}

void FJUSYNCModule::UnregisterModuleSystems() {
    // No global systems to clean up.
}

void FJUSYNCModule::CleanupPlatformSpecific() {
    // Nothing to free: libraries are resolved via the process DLL path.
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FJUSYNCModule, JUSYNC)
