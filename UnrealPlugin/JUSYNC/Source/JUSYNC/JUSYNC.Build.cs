using UnrealBuildTool;
using System;
using System.IO;

public class JUSYNC : ModuleRules
{
    public JUSYNC(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;
        bEnableExceptions = true;
        bUseRTTI = false;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "RealtimeMeshComponent"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate",
            "SlateCore",
            "RenderCore",
            "RHI",
            "GameplayTasks",
            "LidarPointCloudRuntime"
        });

        // Setup third-party includes
        string ThirdPartyPath = Path.Combine(ModuleDirectory, "..", "ThirdParty");
        string AnariUsdPath = Path.Combine(ThirdPartyPath, "AnariUsdMiddleware");

        // Add GLM headers (points to your glm/glm/ directory)
        PublicIncludePaths.Add(Path.Combine(ThirdPartyPath, "glm"));

        // Add middleware headers  
        PublicIncludePaths.Add(Path.Combine(AnariUsdPath, "Include"));

        // Platform configuration
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            ConfigureWindows(AnariUsdPath);
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            ConfigureLinux(AnariUsdPath);
        }
        else
        {
            PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=0");
            Console.WriteLine("JUSYNC: Unsupported platform - middleware disabled");
        }
    }

    private void ConfigureWindows(string AnariUsdPath)
    {
        // Windows system libraries
        PublicSystemLibraries.AddRange(new string[]
        {
            "kernel32.lib",
            "ws2_32.lib",
            "iphlpapi.lib",
            "userenv.lib",
            "DXGI.lib"  // Added for GPU memory queries
        });

        string LibDir = Path.Combine(AnariUsdPath, "Lib", "Win64");
        string LibFile = Path.Combine(LibDir, "anari_usd_middleware.lib");

        if (File.Exists(LibFile))
        {
            PublicAdditionalLibraries.Add(LibFile);

            // Stage required DLLs with enhanced multi-location staging
            string[] RequiredDlls = new string[]
            {
                "anari_usd_middleware.dll",
            };

            StageDllsEnhanced(LibDir, RequiredDlls);

            PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=1");
            Console.WriteLine("JUSYNC: ✅ Windows middleware enabled with enhanced staging");
        }
        else
        {
            PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=0");
            Console.WriteLine($"JUSYNC: ❌ Windows library not found: {LibFile}");
        }
    }

    private void ConfigureLinux(string AnariUsdPath)
    {
        // Linux system libraries
        PublicSystemLibraries.AddRange(new string[]
        {
            "pthread",
            "dl",
            "rt",
            "m"
        });

        string LibDir = Path.Combine(AnariUsdPath, "Lib", "Linux");
        string LibFile = Path.Combine(LibDir, "libanari_usd_middleware.so");

        if (File.Exists(LibFile))
        {
            PublicAdditionalLibraries.Add(LibFile);
            RuntimeDependencies.Add(LibFile);

            PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=1");
            Console.WriteLine("JUSYNC: ✅ Linux middleware enabled with static ZeroMQ");
        }
        else
        {
            PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=0");
            Console.WriteLine($"JUSYNC: ❌ Linux library not found: {LibFile}");
        }
    }

    // ✅ ENHANCED: Multi-location DLL staging for comprehensive coverage
    private void StageDllsEnhanced(string LibDir, string[] RequiredDlls)
    {
        Console.WriteLine("JUSYNC: Starting enhanced DLL staging...");

        foreach (string dll in RequiredDlls)
        {
            string sourceDll = Path.Combine(LibDir, dll);

            if (File.Exists(sourceDll))
            {
                // Stage to multiple locations for comprehensive coverage

                // 1. Binary output directory (for packaged builds)
                RuntimeDependencies.Add(
                    Path.Combine("$(BinaryOutputDir)", dll),
                    sourceDll,
                    StagedFileType.NonUFS
                );

                // 2. Project binaries directory (for editor and PIE)
                RuntimeDependencies.Add(
                    Path.Combine("$(ProjectDir)", "Binaries", "Win64", dll),
                    sourceDll,
                    StagedFileType.NonUFS
                );

                // 3. Plugin binaries directory (for plugin-specific loading)
                RuntimeDependencies.Add(
                    Path.Combine("$(PluginDir)", "Binaries", "Win64", dll),
                    sourceDll,
                    StagedFileType.NonUFS
                );

                Console.WriteLine($"JUSYNC: ✅ Multi-staged: {dll}");
            }
            else
            {
                Console.WriteLine($"JUSYNC: ⚠️ Missing DLL: {dll}");
            }
        }

        Console.WriteLine("JUSYNC: Enhanced DLL staging complete");
    }
}
