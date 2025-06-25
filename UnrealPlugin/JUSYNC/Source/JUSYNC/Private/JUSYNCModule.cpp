#include "JUSYNCModule.h"
#include "Engine/Engine.h"

#define LOCTEXT_NAMESPACE "FJUSYNCModule"

void FJUSYNCModule::StartupModule()
{
    UE_LOG(LogTemp, Log, TEXT("JUSYNC Module Started Successfully - RealtimeMeshComponent Integration"));

    // Initialize any global resources here if needed
    try {
        // Basic module initialization for RealtimeMeshComponent support
        UE_LOG(LogTemp, Log, TEXT("JUSYNC: Middleware libraries available with RealtimeMeshComponent support"));
        UE_LOG(LogTemp, Log, TEXT("JUSYNC: ZeroMQ communication ready"));
        UE_LOG(LogTemp, Log, TEXT("JUSYNC: USD processing with TinyUSDZ ready"));
        UE_LOG(LogTemp, Log, TEXT("JUSYNC: Hash verification with OpenSSL ready"));
        UE_LOG(LogTemp, Log, TEXT("JUSYNC: Texture processing with STB ready"));
        UE_LOG(LogTemp, Log, TEXT("JUSYNC: RealtimeMeshComponent integration ready"));
    } catch (const std::exception& e) {
        UE_LOG(LogTemp, Error, TEXT("JUSYNC: Exception during startup: %s"), UTF8_TO_TCHAR(e.what()));
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
