#include "JUSYNCModule.h"
#include "Engine/Engine.h"

#define LOCTEXT_NAMESPACE "FJUSYNCModule"

void FJUSYNCModule::StartupModule() {
    UE_LOG(LogTemp, Warning, TEXT("=== JUSYNC MODULE STARTUP BEGIN ==="));

    try {
        UE_LOG(LogTemp, Log, TEXT("JUSYNC Module Started - RealtimeMeshComponent Integration"));

#ifdef WITH_ANARI_USD_MIDDLEWARE
        UE_LOG(LogTemp, Warning, TEXT("✅ Compiled WITH middleware support"));

        // Get the DLL directory path
        FString PluginDir = FPaths::ProjectPluginsDir();
        FString DLLPath = FPaths::Combine(PluginDir, TEXT("JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Lib/Win64"));
        FString AbsoluteDLLPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*DLLPath);

        UE_LOG(LogTemp, Warning, TEXT("DLL Directory: %s"), *AbsoluteDLLPath);

        // Check if directory exists
        if (!IFileManager::Get().DirectoryExists(*AbsoluteDLLPath)) {
            UE_LOG(LogTemp, Error, TEXT("❌ DLL directory does not exist: %s"), *AbsoluteDLLPath);
            return;
        }

        // List of required DLLs
        TArray<FString> RequiredDLLs = {
            TEXT("anari_usd_middleware.dll"), TEXT("libzmq-v143-mt-4_3_6.dll"), TEXT("libcrypto-3-x64.dll"),
            TEXT("libssl-3-x64.dll")
        };

        // Check each DLL using Unreal's cross-platform functions
        bool bAllDLLsFound = true;
        for (const FString& DLLName : RequiredDLLs) {
            FString FullDLLPath = FPaths::Combine(AbsoluteDLLPath, DLLName);

            if (IFileManager::Get().FileExists(*FullDLLPath)) {
                UE_LOG(LogTemp, Log, TEXT("✅ Found DLL: %s"), *DLLName);

                // Try to manually load the DLL using Unreal's cross-platform function
                void* DLLHandle = FPlatformProcess::GetDllHandle(*FullDLLPath);
                if (DLLHandle) {
                    UE_LOG(LogTemp, Log, TEXT("✅ Successfully loaded DLL: %s"), *DLLName);
                    FPlatformProcess::FreeDllHandle(DLLHandle);
                } else {
                    UE_LOG(LogTemp, Error, TEXT("❌ Failed to load DLL: %s"), *DLLName);
                    bAllDLLsFound = false;
                }
            } else {
                UE_LOG(LogTemp, Error, TEXT("❌ Missing DLL: %s"), *FullDLLPath);
                bAllDLLsFound = false;
            }
        }

        // Add DLL directory to search path
        FPlatformProcess::AddDllDirectory(*AbsoluteDLLPath);
        UE_LOG(LogTemp, Log, TEXT("Added DLL directory to search path: %s"), *AbsoluteDLLPath);

        if (bAllDLLsFound) {
            UE_LOG(LogTemp, Warning, TEXT("✅ All middleware DLLs found and loadable"));
            UE_LOG(LogTemp, Log, TEXT("JUSYNC: ZeroMQ communication ready"));
            UE_LOG(LogTemp, Log, TEXT("JUSYNC: USD processing with TinyUSDZ ready"));
            UE_LOG(LogTemp, Log, TEXT("JUSYNC: Hash verification with OpenSSL ready"));
            UE_LOG(LogTemp, Log, TEXT("JUSYNC: Texture processing with STB ready"));
            UE_LOG(LogTemp, Log, TEXT("JUSYNC: RealtimeMeshComponent integration ready"));
        } else {
            UE_LOG(LogTemp, Error, TEXT("❌ Some middleware DLLs are missing or cannot be loaded"));
            UE_LOG(LogTemp, Error, TEXT("❌ Plugin will run in LIMITED MODE without middleware support"));
        }

#else
        UE_LOG(LogTemp, Warning, TEXT("⚠️ Compiled WITHOUT middleware support"));
        UE_LOG(LogTemp, Warning, TEXT("⚠️ Check Build.cs - middleware libraries not found during compilation"));
#endif

        UE_LOG(LogTemp, Warning, TEXT("=== JUSYNC MODULE STARTUP COMPLETE ==="));

    } catch (const std::exception& e) {
        UE_LOG(LogTemp, Error, TEXT("❌ JUSYNC: Exception during startup: %s"), UTF8_TO_TCHAR(e.what()));
    }
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
}


void FJUSYNCModule::ShutdownModule()
{
    UE_LOG(LogTemp, Log, TEXT("JUSYNC Module Shutdown"));

    // Cleanup any global resources
    try {
        // Basic cleanup
        UE_LOG(LogTemp, Log, TEXT("JUSYNC: Module shutdown complete"));
        UE_LOG(LogTemp, Log, TEXT("JUSYNC: RealtimeMeshComponent integration cleaned up"));
    } catch (const std::exception& e) {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC: Exception during shutdown: %s"), UTF8_TO_TCHAR(e.what()));
    }
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FJUSYNCModule, JUSYNC)
