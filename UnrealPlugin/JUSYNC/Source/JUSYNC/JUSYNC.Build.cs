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
        }

        // Third-party library integration
        string ThirdPartyPath = Path.Combine(ModuleDirectory, "..", "ThirdParty");
        string AnariUsdPath = Path.Combine(ThirdPartyPath, "AnariUsdMiddleware");

        PublicIncludePaths.Add(Path.Combine(AnariUsdPath, "Include"));

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string LibPath = Path.Combine(AnariUsdPath, "Lib", "Win64");
            string LibFile = Path.Combine(LibPath, "anari_usd_middleware.lib");

            if (File.Exists(LibFile))
            {
                PublicAdditionalLibraries.Add(LibFile);

                RuntimeDependencies.Add(Path.Combine(LibPath, "anari_usd_middleware.dll"));
                RuntimeDependencies.Add(Path.Combine(LibPath, "libzmq-v143-mt-4_3_6.dll"));
                RuntimeDependencies.Add(Path.Combine(LibPath, "libcrypto-3-x64.dll"));
                RuntimeDependencies.Add(Path.Combine(LibPath, "libssl-3-x64.dll"));

                PublicDefinitions.Add("WITH_ANARI_USD_MIDDLEWARE=1");
                System.Console.WriteLine("JUSYNC: Middleware libraries linked successfully");
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
