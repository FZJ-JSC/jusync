#include "JUSYNCModule.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/IConsoleManager.h"
#include "JUSYNCFileSpawnerActor.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Spawner/JUSYNCFileSpawnerCustomization.h"
#include "Misc/CoreDelegates.h"
#include "PropertyEditorModule.h"
#endif
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#ifdef WITH_ANARI_USD_MIDDLEWARE
#include "AnariUsdMiddleware.h"
#if WITH_ANARI_USD_MIDDLEWARE
#include "AnariUsdMiddleware_C.h"
#endif
#endif

#define LOCTEXT_NAMESPACE "FJUSYNCModule"

DEFINE_LOG_CATEGORY(LogJUSYNC)

#if WITH_ANARI_USD_MIDDLEWARE
static IConsoleObject* GJusyncDiagCmd = nullptr;

static void JusyncLogDiagTable(int32 Rows)
{
    TArray<char> Buf;
    Buf.SetNumUninitialized(128 * 1024);
    const int32 Written = JusyncDiagDump_C(Buf.GetData(), Buf.Num(), Rows);
    if (Written <= 0)
    {
        UE_LOG(LogJUSYNC, Display, TEXT("[jusync.diag] no traces recorded yet"));
        return;
    }
    Buf[Written] = '\0';
    const FString Table(UTF8_TO_TCHAR(Buf.GetData()));
    TArray<FString> Lines;
    Table.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
    UE_LOG(LogJUSYNC, Display,
        TEXT("===== JUSYNC streaming stage latency (ms, -1 = stage not reached) ====="));
    for (const FString& Line : Lines)
    {
        UE_LOG(LogJUSYNC, Display, TEXT("%s"), *Line);
    }
}
#endif


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
#if WITH_EDITOR
    if (!bPlaybackDetailsRegistered && !PostEngineInitHandle.IsValid())
    {
        auto RegisterPlaybackDetails = [this]()
        {
            if (bPlaybackDetailsRegistered)
            {
                return;
            }

            FPropertyEditorModule& PropertyModule =
                FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
            const FName ClassName = AJUSYNCFileSpawnerActor::StaticClass()->GetFName();

            UE_LOG(LogJUSYNC, Log, TEXT("Registering JUSYNC playback Details for class %s"),
                *ClassName.ToString());

            PropertyModule.RegisterCustomClassLayout(
                ClassName,
                FOnGetDetailCustomizationInstance::CreateStatic(
                    &FJUSYNCFileSpawnerCustomization::MakeInstance));

            // Existing Details views keep the layout they were built with until
            // they are told that customizations changed.
            PropertyModule.NotifyCustomizationModuleChanged();

            bPlaybackDetailsRegistered = true;
            UE_LOG(LogJUSYNC, Log, TEXT("Registered JUSYNC sample-playback Details timeline and buttons"));
        };

        if (GEditor != nullptr)
        {
            RegisterPlaybackDetails();
        }
        else
        {
            // JUSYNC loads at PostConfigInit, which is before the editor and
            // the details panels exist. Register once the engine is ready so
            // the PropertyEditor module used by the Details panel is the same
            // one that owns our customization.
            PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda(RegisterPlaybackDetails);
            UE_LOG(LogJUSYNC, Log, TEXT("Deferred JUSYNC playback Details registration until post engine init"));
        }
    }
#endif

#if WITH_ANARI_USD_MIDDLEWARE
    if (!GJusyncDiagCmd)
    {
        GJusyncDiagCmd = IConsoleManager::Get().RegisterConsoleCommand(
            TEXT("jusync.diag"),
            TEXT("Print JUSYNC streaming stage latency table (ms). Usage: jusync.diag [N|clear], default N=20"),
            FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) {
                if (Args.Num() > 0 && Args[0].Equals(TEXT("clear"), ESearchCase::IgnoreCase))
                {
                    JusyncDiagClear_C();
                    UE_LOG(LogJUSYNC, Log, TEXT("[jusync.diag] journal cleared"));
                    return;
                }
                const int32 N = (Args.Num() > 0)
                    ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 200)
                    : 20;
                JusyncLogDiagTable(N);
            }),
            ECVF_Default);
    }

    // (its ticker is tied to the process lifetime of the headless run).

    MaybeStartHeadlessHarness();
#endif
}

void FJUSYNCModule::UnregisterModuleSystems() {
#if WITH_EDITOR
    if (PostEngineInitHandle.IsValid())
    {
        FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
        PostEngineInitHandle.Reset();
    }

    if (bPlaybackDetailsRegistered)
    {
        if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
        {
            FPropertyEditorModule& PropertyModule =
                FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
            PropertyModule.UnregisterCustomClassLayout(
                AJUSYNCFileSpawnerActor::StaticClass()->GetFName());
            PropertyModule.NotifyCustomizationModuleChanged();
        }
        bPlaybackDetailsRegistered = false;
    }
#endif

#if WITH_ANARI_USD_MIDDLEWARE
    if (GJusyncDiagCmd)
    {
        IConsoleManager::Get().UnregisterConsoleObject(GJusyncDiagCmd);
        GJusyncDiagCmd = nullptr;
    }
#endif
}

#if WITH_ANARI_USD_MIDDLEWARE
namespace
{
    struct FJusyncHarnessState
    {
        double StartTime = 0.0;
        double DiagInterval = 0.0;
        double NextDiagAt = 0.0;
        int32 DiagRemaining = 0;
        int32 DiagRows = 40;
        double QuitAt = 0.0;
        bool bAutoSpawner = false;
        bool bSpawnerStarted = false;
        float PointSizeOverride = -1.f;
        float PointBiasOverride = -1.f;
        FTSTicker::FDelegateHandle TickerHandle;
    };
    FJusyncHarnessState GHarness;

    bool JusyncHarnessTick(float /*DeltaTime*/)
    {
        const double Elapsed = FPlatformTime::Seconds() - GHarness.StartTime;

        if (GHarness.bAutoSpawner && !GHarness.bSpawnerStarted && GEngine)
        {
            for (const FWorldContext& Context : GEngine->GetWorldContexts())
            {
                UWorld* World = Context.World();
                if (!World
                    || (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE)
                    || !World->HasBegunPlay())
                {
                    continue;
                }

                FActorSpawnParameters Params;
                Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                if (AJUSYNCFileSpawnerActor* Spawner = World->SpawnActor<AJUSYNCFileSpawnerActor>(
                        FVector::ZeroVector, FRotator::ZeroRotator, Params))
                {
                    Spawner->bEnablePerfLogging = true; // the harness is the perf instrument
                    if (GHarness.PointSizeOverride >= 0.f)
                    {
                        Spawner->PointCloudSize = GHarness.PointSizeOverride;
                    }
                    if (GHarness.PointBiasOverride >= 0.f)
                    {
                        Spawner->PointSizeBias = GHarness.PointBiasOverride;
                    }
                    Spawner->StartSpawning();
                    GHarness.bSpawnerStarted = true;
                    UE_LOG(LogJUSYNC, Display, TEXT("[jusync.harness] auto-spawner started streaming (world '%s', t=%.1fs)"),
                        *World->GetMapName(), Elapsed);
                }
                break;
            }
        }

        if (GHarness.DiagInterval > 0.0 && GHarness.DiagRemaining > 0 && Elapsed >= GHarness.NextDiagAt)
        {
            UE_LOG(LogJUSYNC, Display, TEXT("[jusync.harness] diag dump t=%.1fs"), Elapsed);
            JusyncLogDiagTable(GHarness.DiagRows);
            --GHarness.DiagRemaining;
            GHarness.NextDiagAt += GHarness.DiagInterval;
        }

        if (GHarness.QuitAt > 0.0 && Elapsed >= GHarness.QuitAt)
        {
            UE_LOG(LogJUSYNC, Display, TEXT("[jusync.harness] auto-quit at t=%.1fs"), Elapsed);
            GHarness.TickerHandle.Reset();
            FPlatformMisc::RequestExitWithStatus(true, 0);
            return false;
        }
        return true;
    }
}

void FJUSYNCModule::MaybeStartHeadlessHarness()
{
    FString Spec;
    if (FParse::Value(FCommandLine::Get(), TEXT("JusyncAutoDiag="), Spec))
    {
        FString IntervalStr = Spec;
        FString CountStr;
        if (Spec.Split(TEXT(","), &IntervalStr, &CountStr))
        {
            GHarness.DiagRemaining = FMath::Max(1, FCString::Atoi(*CountStr));
        }
        else
        {
            GHarness.DiagRemaining = TNumericLimits<int32>::Max();
        }
        GHarness.DiagInterval = FCString::Atof(*IntervalStr);
    }
    FString RowsStr;
    if (FParse::Value(FCommandLine::Get(), TEXT("JusyncAutoDiagRows="), RowsStr))
    {
        GHarness.DiagRows = FMath::Clamp(FCString::Atoi(*RowsStr), 1, 200);
    }
    FString QuitStr;
    if (FParse::Value(FCommandLine::Get(), TEXT("JusyncAutoQuit="), QuitStr))
    {
        GHarness.QuitAt = FCString::Atof(*QuitStr);
    }
    GHarness.bAutoSpawner = FParse::Param(FCommandLine::Get(), TEXT("JusyncAutoSpawner"));
    FString PSizeStr, PBiasStr;
    if (FParse::Value(FCommandLine::Get(), TEXT("JusyncPointSize="), PSizeStr))
    {
        GHarness.PointSizeOverride = FMath::Max(0.f, FCString::Atof(*PSizeStr));
    }
    if (FParse::Value(FCommandLine::Get(), TEXT("JusyncPointBias="), PBiasStr))
    {
        GHarness.PointBiasOverride = FMath::Clamp(FCString::Atof(*PBiasStr), 0.f, 0.15f);
    }

    if (GHarness.DiagInterval <= 0.0 && GHarness.QuitAt <= 0.0 && !GHarness.bAutoSpawner)
    {
        return; // nothing requested
    }

    GHarness.StartTime = FPlatformTime::Seconds();
    GHarness.NextDiagAt = GHarness.DiagInterval;
    UE_LOG(LogJUSYNC, Display,
        TEXT("[jusync.harness] armed: autoSpawner=%d diagEvery=%.1fs count=%d rows=%d quitAt=%.1fs"),
        GHarness.bAutoSpawner ? 1 : 0, GHarness.DiagInterval, GHarness.DiagRemaining,
        GHarness.DiagRows, GHarness.QuitAt);
    GHarness.TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateStatic(&JusyncHarnessTick));
}
#endif

void FJUSYNCModule::CleanupPlatformSpecific() {
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FJUSYNCModule, JUSYNC)
