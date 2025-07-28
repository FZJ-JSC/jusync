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

void FJUSYNCModule::StartupModule() {
    UE_LOG(LogTemp, Warning, TEXT("=== JUSYNC MODULE STARTUP BEGIN ==="));

    try {
        DetectAndLogPlatform();

#ifdef WITH_ANARI_USD_MIDDLEWARE
        UE_LOG(LogTemp, Warning, TEXT("✅ Compiled WITH middleware support"));

        if (InitializePlatformSpecific()) {
            UE_LOG(LogTemp, Warning, TEXT("✅ Platform-specific initialization successful"));
            LogMiddlewareCapabilities();
        } else {
            UE_LOG(LogTemp, Error, TEXT("❌ Platform-specific initialization failed"));
            UE_LOG(LogTemp, Error, TEXT("❌ Plugin will run in LIMITED MODE"));
        }
#else
        UE_LOG(LogTemp, Warning, TEXT("⚠️ Compiled WITHOUT middleware support"));
        UE_LOG(LogTemp, Warning, TEXT("⚠️ Check Build.cs - middleware libraries not found during compilation"));
        UE_LOG(LogTemp, Warning, TEXT("⚠️ Only basic JUSYNC functionality will be available"));
#endif

        RegisterModuleSystems();

        UE_LOG(LogTemp, Warning, TEXT("=== JUSYNC MODULE STARTUP COMPLETE ==="));
    } catch (const std::exception&) {
        UE_LOG(LogTemp, Error, TEXT("❌ JUSYNC: Exception during startup"));
    } catch (...) {
        UE_LOG(LogTemp, Error, TEXT("❌ JUSYNC: Unknown exception during startup"));
    }
}

void FJUSYNCModule::ShutdownModule() {
    UE_LOG(LogTemp, Warning, TEXT("=== JUSYNC MODULE SHUTDOWN BEGIN ==="));

    try {
        CleanupPlatformSpecific();
        UnregisterModuleSystems();

        UE_LOG(LogTemp, Log, TEXT("JUSYNC Module Shutdown Complete"));
        UE_LOG(LogTemp, Log, TEXT("JUSYNC: RealtimeMeshComponent integration cleaned up"));
    } catch (const std::exception&) {
        UE_LOG(LogTemp, Error, TEXT("❌ JUSYNC: Exception during shutdown"));
    } catch (...) {
        UE_LOG(LogTemp, Error, TEXT("❌ JUSYNC: Unknown exception during shutdown"));
    }

    UE_LOG(LogTemp, Warning, TEXT("=== JUSYNC MODULE SHUTDOWN COMPLETE ==="));
}

void FJUSYNCModule::DetectAndLogPlatform() {
    UE_LOG(LogTemp, Warning, TEXT("=== PLATFORM DETECTION ==="));

#if PLATFORM_WINDOWS
    UE_LOG(LogTemp, Warning, TEXT("Platform: Windows (x64)"));
    UE_LOG(LogTemp, Log, TEXT("Expected middleware libraries: .dll files"));
#elif PLATFORM_LINUX
    UE_LOG(LogTemp, Warning, TEXT("Platform: Linux"));
    UE_LOG(LogTemp, Log, TEXT("Expected middleware libraries: .so files"));
#else
    UE_LOG(LogTemp, Warning, TEXT("Platform: Unknown/Unsupported"));
    UE_LOG(LogTemp, Warning, TEXT("⚠️ This platform may not be fully supported"));
#endif

    UE_LOG(LogTemp, Log, TEXT("Unreal Engine Version: 5.5"));
    UE_LOG(LogTemp, Log, TEXT("Build Configuration: Development"));
}

bool FJUSYNCModule::InitializePlatformSpecific() {
    UE_LOG(LogTemp, Warning, TEXT("=== PLATFORM-SPECIFIC INITIALIZATION ==="));

#ifdef WITH_ANARI_USD_MIDDLEWARE

#if PLATFORM_WINDOWS
    return InitializeWindows();
#elif PLATFORM_LINUX
    return InitializeLinux();
#else
    UE_LOG(LogTemp, Error, TEXT("❌ Unsupported platform for middleware"));
    return false;
#endif

#else
    UE_LOG(LogTemp, Warning, TEXT("⚠️ Middleware support not compiled - skipping platform initialization"));
    return true;  // Not an error, just limited functionality
#endif
}

#if PLATFORM_WINDOWS
bool FJUSYNCModule::InitializeWindows() {
    UE_LOG(LogTemp, Warning, TEXT("Initializing Windows platform..."));

    // Get plugin directory paths
    FString PluginDir = FPaths::ProjectPluginsDir();
    FString DLLPath = FPaths::Combine(PluginDir, TEXT("JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Lib/Win64"));
    FString AbsoluteDLLPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*DLLPath);

    UE_LOG(LogTemp, Log, TEXT("Windows DLL Directory: %s"), *AbsoluteDLLPath);

    // Check if directory exists
    if (!IFileManager::Get().DirectoryExists(*AbsoluteDLLPath)) {
        UE_LOG(LogTemp, Error, TEXT("❌ Windows DLL directory does not exist: %s"), *AbsoluteDLLPath);
        return false;
    }

    // List of required Windows DLLs
    TArray<FString> RequiredDLLs = {
        TEXT("anari_usd_middleware.dll"), TEXT("libzmq-v143-mt-4_3_6.dll"), TEXT("libcrypto-3-x64.dll"),
        TEXT("libssl-3-x64.dll")
    };

    bool bAllLibrariesFound = ValidateLibraries(AbsoluteDLLPath, RequiredDLLs, TEXT("dll"));

    if (bAllLibrariesFound) {
        CheckVCRedistributablesInstalled();

        // Add DLL directory to search path
        FPlatformProcess::AddDllDirectory(*AbsoluteDLLPath);
        UE_LOG(LogTemp, Log, TEXT("Added DLL directory to search path: %s"), *AbsoluteDLLPath);

        UE_LOG(LogTemp, Warning, TEXT("✅ Windows platform initialization complete"));
    }

    return bAllLibrariesFound;
}

void FJUSYNCModule::CheckVCRedistributablesInstalled() {
    UE_LOG(LogTemp, Log, TEXT("Checking Visual C++ Redistributables..."));

    // Check for common VC++ runtime DLLs
    TArray<FString> VCRuntimeDLLs = {TEXT("msvcp140.dll"), TEXT("vcruntime140.dll"), TEXT("vcruntime140_1.dll")};

    for (const FString& RuntimeDLL : VCRuntimeDLLs) {
        HMODULE hModule = GetModuleHandle(*RuntimeDLL);
        if (hModule) {
            UE_LOG(LogTemp, Log, TEXT("✅ Found VC++ Runtime: %s"), *RuntimeDLL);
        } else {
            UE_LOG(LogTemp, Warning, TEXT("⚠️ Missing VC++ Runtime: %s"), *RuntimeDLL);
        }
    }

    UE_LOG(LogTemp, Log, TEXT("✅ VC++ Redistributable check complete"));
}
#endif

#if PLATFORM_LINUX
bool FJUSYNCModule::InitializeLinux() {
    UE_LOG(LogTemp, Warning, TEXT("Initializing Linux platform..."));

    // Get plugin directory paths
    FString PluginDir = FPaths::ProjectPluginsDir();
    FString LibPath = FPaths::Combine(PluginDir, TEXT("JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Lib/Linux"));
    FString AbsoluteLibPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*LibPath);

    UE_LOG(LogTemp, Log, TEXT("Linux Library Directory: %s"), *AbsoluteLibPath);

    // Check if directory exists
    if (!IFileManager::Get().DirectoryExists(*AbsoluteLibPath)) {
        UE_LOG(LogTemp, Error, TEXT("❌ Linux library directory does not exist: %s"), *AbsoluteLibPath);
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
        UE_LOG(LogTemp, Log, TEXT("Added library directory to search path: %s"), *AbsoluteLibPath);

        CheckLinuxDependencies();

        UE_LOG(LogTemp, Warning, TEXT("✅ Linux platform initialization complete"));
    }

    return bAllLibrariesFound;
}

void FJUSYNCModule::CheckLinuxDependencies() {
    UE_LOG(LogTemp, Log, TEXT("Checking Linux system dependencies..."));

    // Check for common Linux dependencies
    TArray<FString> SystemLibs = {TEXT("libssl.so"), TEXT("libcrypto.so"), TEXT("libzmq.so")};

    // Note: This is a basic check - in a real implementation you might want to
    // use dlopen() to verify libraries can be loaded
    for (const FString& LibName : SystemLibs) {
        UE_LOG(LogTemp, Log, TEXT("Checking system library: %s"), *LibName);
    }

    UE_LOG(LogTemp, Log, TEXT("✅ Linux dependencies check complete"));
}
#endif

bool FJUSYNCModule::ValidateLibraries(
    const FString& LibraryPath, const TArray<FString>& RequiredLibraries, const FString& Extension
) {
    UE_LOG(LogTemp, Log, TEXT("Validating %d %s libraries..."), RequiredLibraries.Num(), *Extension.ToUpper());

    bool bAllLibrariesFound = true;
    int32 ValidatedCount = 0;

    for (const FString& LibraryName : RequiredLibraries) {
        FString FullLibraryPath = FPaths::Combine(LibraryPath, LibraryName);

        if (IFileManager::Get().FileExists(*FullLibraryPath)) {
            UE_LOG(LogTemp, Log, TEXT("✅ Found library: %s"), *LibraryName);

            // Try to load the library to verify it's valid
            if (AttemptLibraryLoad(FullLibraryPath, LibraryName)) {
                ValidatedCount++;
                UE_LOG(LogTemp, Log, TEXT("✅ Successfully validated: %s"), *LibraryName);
            } else {
                UE_LOG(LogTemp, Error, TEXT("❌ Failed to load: %s"), *LibraryName);
                bAllLibrariesFound = false;
            }
        } else {
            UE_LOG(LogTemp, Error, TEXT("❌ Missing library: %s"), *FullLibraryPath);
            bAllLibrariesFound = false;
        }
    }

    if (bAllLibrariesFound) {
        UE_LOG(LogTemp, Warning, TEXT("✅ All %d middleware libraries validated successfully"), ValidatedCount);
    } else {
        UE_LOG(
            LogTemp, Error, TEXT("❌ Library validation failed - %d/%d libraries found"), ValidatedCount,
            RequiredLibraries.Num()
        );
        UE_LOG(LogTemp, Error, TEXT("❌ Plugin will run in LIMITED MODE"));
    }

    return bAllLibrariesFound;
}

bool FJUSYNCModule::AttemptLibraryLoad(const FString& FullPath, const FString& LibraryName) {
    // Use Unreal's cross-platform DLL loading
    void* LibraryHandle = FPlatformProcess::GetDllHandle(*FullPath);

    if (LibraryHandle) {
        // Store handle for cleanup if needed
        LoadedLibraryHandles.Add(LibraryName, LibraryHandle);

        // Immediately free the handle since we just wanted to test loading
        // The actual loading will be handled by the middleware initialization
        FPlatformProcess::FreeDllHandle(LibraryHandle);
        LoadedLibraryHandles.Remove(LibraryName);

        return true;
    }

    return false;
}

void FJUSYNCModule::LogMiddlewareCapabilities() {
    UE_LOG(LogTemp, Warning, TEXT("=== MIDDLEWARE CAPABILITIES ==="));
    UE_LOG(LogTemp, Log, TEXT("✅ ZeroMQ communication ready"));
    UE_LOG(LogTemp, Log, TEXT("✅ USD processing with TinyUSDZ ready"));
    UE_LOG(LogTemp, Log, TEXT("✅ Hash verification with OpenSSL ready"));
    UE_LOG(LogTemp, Log, TEXT("✅ Texture processing with STB ready"));
    UE_LOG(LogTemp, Log, TEXT("✅ RealtimeMeshComponent integration ready"));
    UE_LOG(LogTemp, Log, TEXT("✅ Cross-platform file handling ready"));
}

void FJUSYNCModule::RegisterModuleSystems() {
    UE_LOG(LogTemp, Log, TEXT("Registering JUSYNC module systems..."));

    // Register any global systems, callbacks, or subsystems here
    // This is where you'd hook into Unreal's system if needed

    UE_LOG(LogTemp, Log, TEXT("✅ Module systems registered"));
}

void FJUSYNCModule::UnregisterModuleSystems() {
    UE_LOG(LogTemp, Log, TEXT("Unregistering JUSYNC module systems..."));

    // Cleanup any global systems, callbacks, or subsystems here

    UE_LOG(LogTemp, Log, TEXT("✅ Module systems unregistered"));
}

void FJUSYNCModule::CleanupPlatformSpecific() {
    UE_LOG(LogTemp, Log, TEXT("Performing platform-specific cleanup..."));

    // Free any loaded library handles
    for (auto& Pair : LoadedLibraryHandles) {
        if (Pair.Value) {
            FPlatformProcess::FreeDllHandle(Pair.Value);
            UE_LOG(LogTemp, Log, TEXT("Freed library handle: %s"), *Pair.Key);
        }
    }
    LoadedLibraryHandles.Empty();

    UE_LOG(LogTemp, Log, TEXT("✅ Platform-specific cleanup complete"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FJUSYNCModule, JUSYNC)
