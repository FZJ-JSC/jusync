using UnrealBuildTool;
using System.IO;

public class JUSYNC : ModuleRules
{
    public JUSYNC(ReadOnlyTargetRules Target) : base(Target)
    {
        // Fix PCH issues by using explicit PCH mode
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // UE5.5 requires C++20
        CppStandard = CppStandardVersion.Cpp20;

        // Critical: Disable Unity builds and enable STL support
        bUseUnity = false;
        bUseRTTI = true;
        bEnableExceptions = true;

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
            "GameplayTasks"
        });

        // Critical STL linking definitions
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

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            // Add required system libraries for STL
            PublicSystemLibraries.AddRange(new string[]
            {
                "kernel32.lib",
                "user32.lib",
                "gdi32.lib",
                "winspool.lib",
                "comdlg32.lib",
                "advapi32.lib",
                "shell32.lib",
                "ole32.lib",
                "oleaut32.lib",
                "uuid.lib",
                "odbc32.lib",
                "odbccp32.lib"
            });

            // Third-party library integration
            string ThirdPartyPath = Path.Combine(ModuleDirectory, "..", "ThirdParty");
            string AnariUsdPath = Path.Combine(ThirdPartyPath, "AnariUsdMiddleware");
            PublicIncludePaths.Add(Path.Combine(AnariUsdPath, "Include"));

            string LibPath = Path.Combine(AnariUsdPath, "Lib", "Win64");
            string LibFile = Path.Combine(LibPath, "anari_usd_middleware.lib");

            if (File.Exists(LibFile))
            {
                PublicAdditionalLibraries.Add(LibFile);

                // List of DLLs to copy
                string[] Dlls = new string[]
                {
                    "anari_usd_middleware.dll",
                    "libzmq-v143-mt-4_3_6.dll",
                    "libcrypto-3-x64.dll",
                    "libssl-3-x64.dll"
                };

                foreach (string Dll in Dlls)
                {
                    // Source: ThirdParty/AnariUsdMiddleware/Lib/Win64/DLL
                    string SourceDll = Path.Combine(LibPath, Dll);
                    // Destination: Plugins/JUSYNC/Binaries/Win64/DLL
                    string DestDll = Path.Combine(ModuleDirectory, "../../Binaries/Win64", Dll);

                    // Register for runtime & packaging, and copy to Binaries/Win64
                    RuntimeDependencies.Add(DestDll, SourceDll);
                }

                PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=1");
                System.Console.WriteLine("JUSYNC: Middleware libraries linked and DLLs set for runtime copy.");
            }
            else
            {
                System.Console.WriteLine("JUSYNC: Library file not found: " + LibFile);
                PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=0");
            }
        }

        UndefinedIdentifierWarningLevel = WarningLevel.Off;
    }
}
