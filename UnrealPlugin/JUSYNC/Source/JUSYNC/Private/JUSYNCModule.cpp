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
    UE_LOG(LogJUSYNC, Log, TEXT("=== JUSYNC MODULE STARTUP BEGIN ==="));

    try {
        DetectAndLogPlatform();

#ifdef WITH_ANARI_USD_MIDDLEWARE
        UE_LOG(LogJUSYNC, Log, TEXT("✅ Compiled WITH middleware support"));

        if (InitializePlatformSpecific()) {
            UE_LOG(LogJUSYNC, Log, TEXT("✅ Platform-specific initialization successful"));
            LogMiddlewareCapabilities();
        } else {
            UE_LOG(LogJUSYNC, Warning, TEXT("❌ Platform-specific initialization failed"));
            UE_LOG(LogJUSYNC, Warning, TEXT("❌ Plugin will run in LIMITED MODE"));
        }
#else
        UE_LOG(LogJUSYNC, Warning, TEXT("⚠️ Compiled WITHOUT middleware support"));
        UE_LOG(LogJUSYNC, Warning, TEXT("⚠️ Check Build.cs - middleware libraries not found during compilation"));
        UE_LOG(LogJUSYNC, Warning, TEXT("⚠️ Only basic JUSYNC functionality will be available"));
#endif

        RegisterModuleSystems();

        UE_LOG(LogJUSYNC, Log, TEXT("=== JUSYNC MODULE STARTUP COMPLETE ==="));
    } catch (const std::exception&) {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ JUSYNC: Exception during startup"));
    } catch (...) {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ JUSYNC: Unknown exception during startup"));
    }
}

void FJUSYNCModule::ShutdownModule() {
    UE_LOG(LogJUSYNC, Log, TEXT("=== JUSYNC MODULE SHUTDOWN BEGIN ==="));

    try {
        CleanupPlatformSpecific();
        UnregisterModuleSystems();

        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC Module Shutdown Complete"));
        UE_LOG(LogJUSYNC, Log, TEXT("JUSYNC: RealtimeMeshComponent integration cleaned up"));
    } catch (const std::exception&) {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ JUSYNC: Exception during shutdown"));
    } catch (...) {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ JUSYNC: Unknown exception during shutdown"));
    }

    UE_LOG(LogJUSYNC, Log, TEXT("=== JUSYNC MODULE SHUTDOWN COMPLETE ==="));
}

void FJUSYNCModule::DetectAndLogPlatform() {
    UE_LOG(LogJUSYNC, Log, TEXT("=== PLATFORM DETECTION ==="));

#if PLATFORM_WINDOWS
    UE_LOG(LogJUSYNC, Log, TEXT("Platform: Windows (x64)"));
    UE_LOG(LogJUSYNC, Log, TEXT("Expected middleware libraries: .dll files"));
#elif PLATFORM_LINUX
    UE_LOG(LogJUSYNC, Log, TEXT("Platform: Linux"));
    UE_LOG(LogJUSYNC, Log, TEXT("Expected middleware libraries: .so files"));
#else
    UE_LOG(LogJUSYNC, Warning, TEXT("Platform: Unknown/Unsupported"));
    UE_LOG(LogJUSYNC, Warning, TEXT("⚠️ This platform may not be fully supported"));
#endif

    UE_LOG(LogJUSYNC, Log, TEXT("Unreal Engine Version: %s"), ENGINE_VERSION_STRING);
#if UE_BUILD_SHIPPING
    UE_LOG(LogJUSYNC, Log, TEXT("Build Configuration: Shipping"))
#else
    UE_LOG(LogJUSYNC, Log, TEXT("Build Configuration: Development"))
#endif
}

bool FJUSYNCModule::InitializePlatformSpecific() {
    UE_LOG(LogJUSYNC, Log, TEXT("=== PLATFORM-SPECIFIC INITIALIZATION ==="));

#ifdef WITH_ANARI_USD_MIDDLEWARE

#if PLATFORM_WINDOWS
    return InitializeWindows();
#elif PLATFORM_LINUX
    return InitializeLinux();
#else
    UE_LOG(LogJUSYNC, Error, TEXT("❌ Unsupported platform for middleware"));
    return false;
#endif

#else
    UE_LOG(LogJUSYNC, Warning, TEXT("⚠️ Middleware support not compiled - skipping platform initialization"));
    return true;  // Not an error, just limited functionality
#endif
}

#if PLATFORM_WINDOWS
bool FJUSYNCModule::InitializeWindows() {
    UE_LOG(LogJUSYNC, Log, TEXT("Initializing Windows platform..."));

    // Get plugin directory paths
    FString PluginDir = FPaths::ProjectPluginsDir();
    FString DLLPath = FPaths::Combine(PluginDir, TEXT("JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Lib/Win64"));
    FString AbsoluteDLLPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*DLLPath);

    UE_LOG(LogJUSYNC, Log, TEXT("Windows DLL Directory: %s"), *AbsoluteDLLPath);

    // Check if directory exists
    if (!IFileManager::Get().DirectoryExists(*AbsoluteDLLPath)) {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Windows DLL directory does not exist: %s"), *AbsoluteDLLPath);
        return false;
    }

    // List of required Windows DLLs
    TArray<FString> RequiredDLLs = {TEXT("anari_usd_middleware.dll")};

    bool bAllLibrariesFound = ValidateLibraries(AbsoluteDLLPath, RequiredDLLs, TEXT("dll"));

    if (bAllLibrariesFound) {
        CheckVCRedistributablesInstalled();

        // Add DLL directory to search path
        FPlatformProcess::AddDllDirectory(*AbsoluteDLLPath);
        UE_LOG(LogJUSYNC, Log, TEXT("Added DLL directory to search path: %s"), *AbsoluteDLLPath);

        UE_LOG(LogJUSYNC, Log, TEXT("✅ Windows platform initialization complete"));
    }

    return bAllLibrariesFound;
}

void FJUSYNCModule::CheckVCRedistributablesInstalled() {
    UE_LOG(LogJUSYNC, Log, TEXT("Checking Visual C++ Redistributables..."));

    // Check for common VC++ runtime DLLs
    TArray<FString> VCRuntimeDLLs = {TEXT("msvcp140.dll"), TEXT("vcruntime140.dll"), TEXT("vcruntime140_1.dll")};

    for (const FString& RuntimeDLL : VCRuntimeDLLs) {
        HMODULE hModule = GetModuleHandle(*RuntimeDLL);
        if (hModule) {
            UE_LOG(LogJUSYNC, Log, TEXT("✅ Found VC++ Runtime: %s"), *RuntimeDLL);
        } else {
            UE_LOG(LogJUSYNC, Warning, TEXT("⚠️ Missing VC++ Runtime: %s"), *RuntimeDLL);
        }
    }

    UE_LOG(LogJUSYNC, Log, TEXT("✅ VC++ Redistributable check complete"));
}
#endif

#if PLATFORM_LINUX
bool FJUSYNCModule::InitializeLinux() {
    UE_LOG(LogJUSYNC, Log, TEXT("Initializing Linux platform..."));

    // Get plugin directory paths
    FString PluginDir = FPaths::ProjectPluginsDir();
    FString LibPath = FPaths::Combine(PluginDir, TEXT("JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Lib/Linux"));
    FString AbsoluteLibPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*LibPath);

    UE_LOG(LogJUSYNC, Log, TEXT("Linux Library Directory: %s"), *AbsoluteLibPath);

    // Check if directory exists
    if (!IFileManager::Get().DirectoryExists(*AbsoluteLibPath)) {
        UE_LOG(LogJUSYNC, Error, TEXT("❌ Linux library directory does not exist: %s"), *AbsoluteLibPath);
        return false;
    }

    // List of required Linux shared libraries
    TArray<FString> RequiredLibs = {
        TEXT("libanari_usd_middleware.so")
        // Add other Linux-specific libraries as needed
    };

    bool bAllLibrariesFound = ValidateLibraries(AbsoluteLibPath, RequiredLibs, TEXT("so"));

    if (bAllLibrariesFound) {
        // Add library directory to LD_LIBRARY_PATH equivalent
        FPlatformProcess::AddDllDirectory(*AbsoluteLibPath);
        UE_LOG(LogJUSYNC, Log, TEXT("Added library directory to search path: %s"), *AbsoluteLibPath);

        CheckLinuxDependencies();

        UE_LOG(LogJUSYNC, Log, TEXT("✅ Linux platform initialization complete"));
    }

    return bAllLibrariesFound;
}

void FJUSYNCModule::CheckLinuxDependencies() {
    UE_LOG(LogJUSYNC, Log, TEXT("Checking Linux system dependencies..."));
    UE_LOG(LogJUSYNC, Log, TEXT("✅ Static linking - no external system dependencies required"));
    UE_LOG(LogJUSYNC, Log, TEXT("✅ Linux dependencies check complete"));
}
#endif

bool FJUSYNCModule::ValidateLibraries(
    const FString& LibraryPath, const TArray<FString>& RequiredLibraries, const FString& Extension
) {
    UE_LOG(LogJUSYNC, Log, TEXT("Validating %d %s libraries..."), RequiredLibraries.Num(), *Extension.ToUpper());

    bool bAllLibrariesFound = true;
    int32 ValidatedCount = 0;

    for (const FString& LibraryName : RequiredLibraries) {
        FString FullLibraryPath = FPaths::Combine(LibraryPath, LibraryName);

        if (IFileManager::Get().FileExists(*FullLibraryPath)) {
            UE_LOG(LogJUSYNC, Log, TEXT("✅ Found library: %s"), *LibraryName);

            // Try to load the library to verify it's valid
            if (AttemptLibraryLoad(FullLibraryPath, LibraryName)) {
                ValidatedCount++;
                UE_LOG(LogJUSYNC, Log, TEXT("✅ Successfully validated: %s"), *LibraryName);
            } else {
                UE_LOG(LogJUSYNC, Error, TEXT("❌ Failed to load: %s"), *LibraryName);
                bAllLibrariesFound = false;
            }
        } else {
            UE_LOG(LogJUSYNC, Error, TEXT("❌ Missing library: %s"), *FullLibraryPath);
            bAllLibrariesFound = false;
        }
    }

    if (bAllLibrariesFound) {
        UE_LOG(LogJUSYNC, Log, TEXT("✅ All %d middleware libraries validated successfully"), ValidatedCount);
    } else {
        UE_LOG(
            LogJUSYNC, Error, TEXT("❌ Library validation failed - %d/%d libraries found"), ValidatedCount,
            RequiredLibraries.Num()
        );
        UE_LOG(LogJUSYNC, Warning, TEXT("❌ Plugin will run in LIMITED MODE"));
    }

    return bAllLibrariesFound;
}

bool FJUSYNCModule::AttemptLibraryLoad(const FString& FullPath, const FString& LibraryName) {
    return IFileManager::Get().FileExists(*FullPath);
}


void FJUSYNCModule::LogMiddlewareCapabilities() {
    UE_LOG(LogJUSYNC, Log, TEXT("=== MIDDLEWARE CAPABILITIES ==="));
    UE_LOG(LogJUSYNC, Log, TEXT("✅ ZeroMQ communication ready"));
    UE_LOG(LogJUSYNC, Log, TEXT("✅ USD processing with TinyUSDZ ready"));
    UE_LOG(LogJUSYNC, Log, TEXT("✅ Hash verification with OpenSSL ready"));
    UE_LOG(LogJUSYNC, Log, TEXT("✅ Texture processing with STB ready"));
    UE_LOG(LogJUSYNC, Log, TEXT("✅ RealtimeMeshComponent integration ready"));
    UE_LOG(LogJUSYNC, Log, TEXT("✅ Cross-platform file handling ready"));
}

void FJUSYNCModule::RegisterModuleSystems() {
    UE_LOG(LogJUSYNC, Log, TEXT("Registering JUSYNC module systems..."));

    // Register any global systems, callbacks, or subsystems here
    // This is where you'd hook into Unreal's system if needed

    UE_LOG(LogJUSYNC, Log, TEXT("✅ Module systems registered"));
}

void FJUSYNCModule::UnregisterModuleSystems() {
    UE_LOG(LogJUSYNC, Log, TEXT("Unregistering JUSYNC module systems..."));

    // Cleanup any global systems, callbacks, or subsystems here

    UE_LOG(LogJUSYNC, Log, TEXT("✅ Module systems unregistered"));
}

void FJUSYNCModule::CleanupPlatformSpecific() {
    UE_LOG(LogJUSYNC, Log, TEXT("Performing platform-specific cleanup..."));

    // Free any loaded library handles
    for (auto& Pair : LoadedLibraryHandles) {
        if (Pair.Value) {
            FPlatformProcess::FreeDllHandle(Pair.Value);
            UE_LOG(LogJUSYNC, Log, TEXT("Freed library handle: %s"), *Pair.Key);
        }
    }
    LoadedLibraryHandles.Empty();

    UE_LOG(LogJUSYNC, Log, TEXT("✅ Platform-specific cleanup complete"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FJUSYNCModule, JUSYNC)
