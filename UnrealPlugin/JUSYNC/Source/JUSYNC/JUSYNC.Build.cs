using UnrealBuildTool;
using System.IO;

public class JUSYNC : ModuleRules
{
    public JUSYNC(ReadOnlyTargetRules Target) : base(Target)
    {
        // Build settings
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;
        bUseUnity = false;
        bUseRTTI = false;
        bEnableExceptions = true;
        UndefinedIdentifierWarningLevel = WarningLevel.Off;

        // Core runtime dependencies
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "RealtimeMeshComponent"
        });

        // Private runtime dependencies
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate",
            "SlateCore",
            "RenderCore",
            "RHI",
            "GameplayTasks",
            "BlueprintGraph"
        });

        // Compiler definitions
        PublicDefinitions.AddRange(new string[]
        {
            "NOMINMAX",
            "WIN32_LEAN_AND_MEAN",
            "_CRT_SECURE_NO_WARNINGS=1",
            "_SCL_SECURE_NO_WARNINGS=1",
            "ANARI_USD_MIDDLEWARE_SAFE_MODE=1",
            "_ITERATOR_DEBUG_LEVEL=0",
            "_HAS_EXCEPTIONS=1"
        });

        // Setup third-party libraries
        string ThirdPartyPath = Path.Combine(ModuleDirectory, "..", "ThirdParty");
        string AnariUsdPath = Path.Combine(ThirdPartyPath, "AnariUsdMiddleware");

        // Add ANARI includes
        PublicIncludePaths.Add(Path.Combine(AnariUsdPath, "Include"));

        // Platform configuration
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            ConfigureWindows(AnariUsdPath);
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            ConfigureLinux(ThirdPartyPath, AnariUsdPath);
        }
        else
        {
            PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=0");
            System.Console.WriteLine("JUSYNC: Unsupported platform - middleware disabled");
        }
    }

    private void ConfigureWindows(string AnariUsdPath)
    {
        System.Console.WriteLine("JUSYNC: Configuring Windows");

        // Windows system libraries
        PublicSystemLibraries.AddRange(new string[]
        {
            "kernel32.lib", "user32.lib", "gdi32.lib", "winspool.lib",
            "comdlg32.lib", "advapi32.lib", "shell32.lib", "ole32.lib",
            "oleaut32.lib", "uuid.lib", "odbc32.lib", "odbccp32.lib"
        });

        // ANARI middleware
        string LibDir = Path.Combine(AnariUsdPath, "Lib", "Win64");
        string LibFile = Path.Combine(LibDir, "anari_usd_middleware.lib");

        if (File.Exists(LibFile))
        {
            PublicAdditionalLibraries.Add(LibFile);

            // Delay load DLLs
            PublicDelayLoadDLLs.AddRange(new string[]
            {
                "anari_usd_middleware.dll",
                "libzmq-v143-mt-4_3_6.dll",
                "libcrypto-3-x64.dll",
                "libssl-3-x64.dll"
            });

            // Stage Windows DLLs
            StageWindowsDLLs(LibDir);

            PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=1");
            System.Console.WriteLine("JUSYNC: ✅ Windows libraries configured");
        }
        else
        {
            PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=0");
            System.Console.WriteLine($"JUSYNC: ❌ Windows library not found: {LibFile}");
        }
    }

    private void ConfigureLinux(string ThirdPartyPath, string AnariUsdPath)
    {
        System.Console.WriteLine("JUSYNC: Configuring Linux");

        // Linux system libraries
        PublicSystemLibraries.AddRange(new string[] { "dl", "pthread", "rt", "m" });

        // ANARI middleware
        string LibPath = Path.Combine(AnariUsdPath, "Lib", "Linux");
        string LibFile = Path.Combine(LibPath, "libanari_usd_middleware.so");

        if (File.Exists(LibFile))
        {
            PublicAdditionalLibraries.Add(LibFile);
            RuntimeDependencies.Add(LibFile);
            System.Console.WriteLine("JUSYNC: ✅ Linux ANARI middleware linked");
        }
        else
        {
            PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=0");
            System.Console.WriteLine($"JUSYNC: ❌ Linux library not found: {LibFile}");
            return;
        }

        // Configure ZeroMQ (dynamic linking only)
        if (ConfigureZeroMQ(ThirdPartyPath))
        {
            PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=1");
            System.Console.WriteLine("JUSYNC: ✅ Linux configuration complete");
        }
        else
        {
            PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=0");
            System.Console.WriteLine("JUSYNC: ❌ Linux configuration failed");
        }
    }

    /// <summary>
    /// Configures ZeroMQ dynamic linking only.
    /// Removed static linking to avoid sodium/pgm/norm dependency issues.
    /// </summary>
    private bool ConfigureZeroMQ(string ThirdPartyPath)
    {
        string ZmqPath = Path.Combine(ThirdPartyPath, "ZeroMQ");
        string ZmqIncludePath = Path.Combine(ZmqPath, "include");
        string ZmqLibPath = Path.Combine(ZmqPath, "lib");

        System.Console.WriteLine($"JUSYNC: Configuring ZeroMQ from: {ZmqPath}");

        // Add includes
        if (Directory.Exists(ZmqIncludePath))
        {
            PublicIncludePaths.Add(ZmqIncludePath);
            System.Console.WriteLine("JUSYNC: ✅ ZeroMQ includes added");
        }
        else
        {
            System.Console.WriteLine($"JUSYNC: ❌ ZeroMQ includes not found: {ZmqIncludePath}");
            return false;
        }

        // Method 1: Use unversioned symlink (preferred)
        string BaseLib = Path.Combine(ZmqLibPath, "libzmq.so");
        if (File.Exists(BaseLib))
        {
            PublicAdditionalLibraries.Add(BaseLib);
            string RuntimeLib = Path.Combine(ZmqLibPath, "libzmq.so.5");
            if (File.Exists(RuntimeLib))
            {
                RuntimeDependencies.Add(Path.Combine("$(BinaryOutputDir)", "libzmq.so.5"), RuntimeLib, StagedFileType.NonUFS);
                System.Console.WriteLine("JUSYNC: ⚠️  ZeroMQ runtime library staged for deployment");
            }
            System.Console.WriteLine("JUSYNC: ✅ ZeroMQ linked (unversioned symlink)");
            return true;
        }

        // Method 2: Use versioned symlink
        string VersionedLib = Path.Combine(ZmqLibPath, "libzmq.so.5");
        if (File.Exists(VersionedLib))
        {
            PublicAdditionalLibraries.Add(VersionedLib);
            RuntimeDependencies.Add(Path.Combine("$(BinaryOutputDir)", "libzmq.so.5"), VersionedLib, StagedFileType.NonUFS);
            System.Console.WriteLine("JUSYNC: ✅ ZeroMQ linked (versioned symlink)");
            return true;
        }

        // Method 3: Use actual file
        string ActualLib = Path.Combine(ZmqLibPath, "libzmq.so.5.2.6");
        if (File.Exists(ActualLib))
        {
            PublicAdditionalLibraries.Add(ActualLib);
            RuntimeDependencies.Add(Path.Combine("$(BinaryOutputDir)", "libzmq.so.5"), ActualLib, StagedFileType.NonUFS);
            System.Console.WriteLine("JUSYNC: ✅ ZeroMQ linked (direct file)");
            return true;
        }

        // All methods failed
        System.Console.WriteLine($"JUSYNC: ❌ No ZeroMQ library found in: {ZmqLibPath}");
        if (Directory.Exists(ZmqLibPath))
        {
            System.Console.WriteLine("JUSYNC: Available files:");
            foreach (string file in Directory.GetFiles(ZmqLibPath))
            {
                System.Console.WriteLine($"JUSYNC:   - {Path.GetFileName(file)}");
            }

            System.Console.WriteLine("");
            System.Console.WriteLine("JUSYNC: For GitLab CI, ensure LD_LIBRARY_PATH includes:");
            System.Console.WriteLine($"JUSYNC:   $CI_PROJECT_DIR/Plugins/JUSYNC/Source/ThirdParty/ZeroMQ/lib");
            System.Console.WriteLine("JUSYNC: Or install ZeroMQ system-wide: apt install libzmq5");
        }
        return false;
    }

    private void StageWindowsDLLs(string LibPath)
    {
        string[] RequiredDlls = new string[]
        {
            "anari_usd_middleware.dll",
            "libzmq-v143-mt-4_3_6.dll",
            "libcrypto-3-x64.dll",
            "libssl-3-x64.dll"
        };

        foreach (string dll in RequiredDlls)
        {
            string sourceDll = Path.Combine(LibPath, dll);
            string destDll = Path.Combine("$(BinaryOutputDir)", dll);

            if (File.Exists(sourceDll))
            {
                RuntimeDependencies.Add(destDll, sourceDll, StagedFileType.NonUFS);
                System.Console.WriteLine($"JUSYNC: ✅ Staged: {dll}");
            }
            else
            {
                System.Console.WriteLine($"JUSYNC: ⚠️  Missing: {dll}");
            }
        }
    }
}


