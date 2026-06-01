#include "JUSYNCPointCloudTestActor.h"
#include "JUSYNCPointCloudSpawner.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"

AJUSYNCPointCloudTestActor::AJUSYNCPointCloudTestActor()
{
    PrimaryActorTick.bCanEverTick = false;

    UsdFilePath = TEXT("");
    UsdFilePaths.Empty();
    SpawnOffset = FVector::ZeroVector;
    bSpawnMeshes = true;
    bSpawnPointClouds = true;
    SpawnScaleFactor = 1.0f;
    bAutoLoadOnBegin = true;
    SpawnCount = 1;
    SpawnGap = 200.0f;
    GridColumns = 8;
    GridRowGap = 200.0f;
    TexturePngFilePath = TEXT("");
    TextureParameterName = TEXT("");
    SpawnMaterial = nullptr;
    bApplyGradientToPCs = true;
}

void AJUSYNCPointCloudTestActor::BeginPlay()
{
    Super::BeginPlay();

    if (!UJUSYNCBlueprintLibrary::InitializeJUSYNCMiddleware(TEXT("")))
    {
        UE_LOG(LogTemp, Error, TEXT("[TestActor] Failed to initialize JUSYNC middleware!"));
        GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("[TestActor] Middleware init failed"));
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("[TestActor] Middleware initialized successfully"));

    if (bAutoLoadOnBegin && (UsdFilePaths.Num() > 0 || !UsdFilePath.IsEmpty()))
    {
        LoadAndSpawnFromDisk();
    }
}

void AJUSYNCPointCloudTestActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ClearSpawnedActors();
    Super::EndPlay(EndPlayReason);
}

void AJUSYNCPointCloudTestActor::LoadAndSpawnFromDisk()
{
    // Build list of USD files to process
    TArray<FString> FilesToLoad = UsdFilePaths;
    if (FilesToLoad.Num() == 0 && !UsdFilePath.IsEmpty())
    {
        FilesToLoad.Add(UsdFilePath);
    }

    if (FilesToLoad.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("[TestActor] No USD files specified!"));
        GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("[TestActor] No USD file path set"));
        return;
    }

    // Load optional PNG texture buffer once (before file spawner starts)
    TArray<uint8> PngBuffer;
    if (!TexturePngFilePath.IsEmpty())
    {
        if (FPaths::FileExists(TexturePngFilePath))
        {
            FFileHelper::LoadFileToArray(PngBuffer, *TexturePngFilePath);
            UE_LOG(LogTemp, Log, TEXT("[TestActor] Loaded texture PNG '%s' (%d bytes)"),
                *TexturePngFilePath, PngBuffer.Num());
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[TestActor] Texture PNG not found: %s"), *TexturePngFilePath);
        }
    }

    // Set up Gradient LUT for point clouds from PNG (if enabled + PNG loaded)
    if (bApplyGradientToPCs && PngBuffer.Num() > 0)
    {
        TArray<FColor> GradientLUT = BuildGradientLUTFromPng(PngBuffer);
        if (GradientLUT.Num() > 0)
        {
            UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
            if (S && S->GetPointCloudSpawner())
            {
                S->GetPointCloudSpawner()->SetGradientLUT(GradientLUT);
                UE_LOG(LogTemp, Display, TEXT("[TestActor] Gradient LUT set on PC spawner: %d colors"), GradientLUT.Num());
            }
        }
    }

    FVector BaseLoc = GetActorLocation() + SpawnOffset;
    int32 TotalFiles = FilesToLoad.Num();

    UE_LOG(LogTemp, Display, TEXT("[TestActor] Queuing %d USD file(s), spawn count %d"), TotalFiles, SpawnCount);
    GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Cyan,
        FString::Printf(TEXT("[TestActor] Processing %d file(s), spawn x%d grid"), TotalFiles, SpawnCount));

    // Dispatch each file parse (all background, non-blocking)
    for (int32 i = 0; i < FilesToLoad.Num(); ++i)
    {
        const FString& FilePath = FilesToLoad[i];
        if (!FPaths::FileExists(FilePath))
        {
            UE_LOG(LogTemp, Error, TEXT("[TestActor] File not found: %s"), *FilePath);
            continue;
        }

        TArray<uint8> FileData;
        if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
        {
            UE_LOG(LogTemp, Error, TEXT("[TestActor] Failed to read: %s"), *FilePath);
            continue;
        }

        FString FileName = FPaths::GetCleanFilename(FilePath);
        FVector FileBaseLoc = BaseLoc + FVector(i * SpawnGap * FMath::Max(GridColumns, 1), 0.0f, 0.0f);

        TWeakObjectPtr<AJUSYNCPointCloudTestActor> WeakThis = this;

        AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis, FileData, FileName, FileBaseLoc, PngBuffer]() mutable
            {
                if (!WeakThis.IsValid()) return;

                TArray<FJUSYNCMeshData> MeshData;
                TArray<FJUSYNCPointCloudData> PointCloudData;
                FString Preview;
                bool bParsed = UJUSYNCBlueprintLibrary::LoadUSDFullFromBuffer(FileData, FileName, MeshData, PointCloudData, Preview);

                TWeakObjectPtr<AJUSYNCPointCloudTestActor> WeakCopy = WeakThis;
                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [WeakCopy, bParsed, MeshData = MoveTemp(MeshData), PointCloudData = MoveTemp(PointCloudData),
                     FileName, FileBaseLoc, PngBuf = MoveTemp(PngBuffer)]() mutable
                    {
                        if (!WeakCopy.IsValid()) return;
                        WeakCopy->SpawnFromParsedData(FileName, bParsed, MoveTemp(MeshData), MoveTemp(PointCloudData), FileBaseLoc, PngBuf);
                    },
                    TStatId(), nullptr, ENamedThreads::GameThread);
            });
    }
}

void AJUSYNCPointCloudTestActor::SpawnFromParsedData(const FString& Filename, bool bParsed, TArray<FJUSYNCMeshData>&& MeshData, TArray<FJUSYNCPointCloudData>&& PointCloudData, FVector BaseSpawnLoc, TArray<uint8>& PngBuffer)
{
    if (!bParsed)
    {
        UE_LOG(LogTemp, Error, TEXT("[TestActor] Parse FAILED: %s"), *Filename);
        GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red,
            FString::Printf(TEXT("[TestActor] PARSE FAILED: %s"), *Filename));
        return;
    }

    // Load PNG texture from buffer (once per file parse result)
    UTexture2D* LoadedTex = nullptr;
    if (PngBuffer.Num() > 0)
    {
        LoadedTex = LoadPngAsTexture(PngBuffer);
        if (LoadedTex) LoadedTextures.Add(LoadedTex);
    }

    int32 MeshSpawned = 0;
    int32 PCSpawned = 0;

    // Resolve spawner for budgeted async PC conversion
    UJUSYNCSubsystem* S = UJUSYNCBlueprintLibrary::GetJUSYNCSubsystem();
    FJUSYNCPointCloudSpawner* PCSpawner = S ? S->GetPointCloudSpawner() : nullptr;

    // Spawn N instances in grid layout
    for (int32 Inst = 0; Inst < SpawnCount; ++Inst)
    {
        int32 Col = Inst % GridColumns;
        int32 Row = Inst / GridColumns;
        FVector Loc = BaseSpawnLoc + FVector(Col * SpawnGap, Row * GridRowGap, 0.0f);

        // Meshes
        if (bSpawnMeshes && MeshData.Num() > 0)
        {
            for (const FJUSYNCMeshData& M : MeshData)
            {
                if (!M.IsValid() || M.Vertices.Num() == 0 || M.Triangles.Num() == 0) continue;

                // Prepare custom material with texture parameter (if any)
                UMaterialInterface* SpawnMat = SpawnMaterial;
                if (SpawnMaterial && LoadedTex)
                {
                    UMaterialInstanceDynamic* MatInst = UMaterialInstanceDynamic::Create(SpawnMaterial, this);
                    if (MatInst)
                    {
                        FName ParamName = TextureParameterName.IsEmpty() ? TEXT("BaseColor") : *TextureParameterName;
                        MatInst->SetTextureParameterValue(ParamName, LoadedTex);
                        SpawnMat = MatInst;
                    }
                }

                AActor* Spawned = UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(M, Loc, FRotator::ZeroRotator, SpawnMat);
                if (Spawned)
                {
                    SpawnedActors.Add(Spawned);
                    MeshSpawned++;

                    if (SpawnScaleFactor > 0.0f && SpawnScaleFactor != 1.0f)
                    {
                        Spawned->SetActorScale3D(FVector(SpawnScaleFactor));
                    }

                    // If we only have texture but no custom material, apply texture to default material
                    if (!SpawnMaterial && LoadedTex)
                    {
                        ApplyTextureToMesh(Spawned, LoadedTex);
                    }

                    UE_LOG(LogTemp, Display, TEXT("[TestActor] Mesh #%d: %s inst=%d (%d verts, %d tris, grid %d,%d)"),
                        MeshSpawned, *M.ElementName, Inst, M.Vertices.Num(), M.Triangles.Num() / 3, Col, Row);
                    Loc += FVector(100.0f, 0.0f, 0.0f);
                }
            }
        }

        // Point clouds — use budgeted async spawner to avoid thread explosion
        if (bSpawnPointClouds && PointCloudData.Num() > 0)
        {
            for (const FJUSYNCPointCloudData& PC : PointCloudData)
            {
                if (!PC.IsValid()) continue;

                if (PCSpawner)
                {
                    // Spawner has built-in budget (16ms), gradient LUT, and pool management
                    PCSpawner->SetSpawnLocation(Loc);
                    PCSpawner->SetSpawnScale(SpawnScaleFactor);
                    PCSpawner->EnqueuePointCloud(PC, 0);
                    PCSpawned++;
                }
                else
                {
                    UE_LOG(LogTemp, Warning, TEXT("[TestActor] No PC spawner available, skipping PC spawn"));
                }

                UE_LOG(LogTemp, Display, TEXT("[TestActor] PC #%d: %s inst=%d (%d pts, grid %d,%d)"),
                    PCSpawned, *PC.ElementName, Inst, PC.PointCount, Col, Row);
                Loc += FVector(100.0f, 100.0f, 0.0f);
            }
        }
    }

    // Debug summary on screen
    GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green,
        FString::Printf(TEXT("[TestActor] Meshes: %d"), MeshSpawned));
    GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Blue,
        FString::Printf(TEXT("[TestActor] PCs: %d [async spawner]"), PCSpawned));
    UE_LOG(LogTemp, Display, TEXT("[TestActor] DONE: %s => meshes=%d, PCs=%d (inst=%d, grid %d cols)"),
        *Filename, MeshSpawned, PCSpawned, SpawnCount, GridColumns);
}
                }
                // If no custom material but we have a PNG texture, apply it to the default material after spawn

                AActor* Spawned = UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(M, Loc, FRotator::ZeroRotator, SpawnMat);
                if (Spawned)
                {
                    SpawnedActors.Add(Spawned);
                    MeshSpawned++;

                    if (SpawnScaleFactor > 0.0f && SpawnScaleFactor != 1.0f)
                    {
                        Spawned->SetActorScale3D(FVector(SpawnScaleFactor));
                    }

                    // If we only have texture but no custom material, apply texture to default material
                    if (!SpawnMaterial && LoadedTex)
                    {
                        ApplyTextureToMesh(Spawned, LoadedTex);
                    }

                    UE_LOG(LogTemp, Display, TEXT("[TestActor] Mesh #%d: %s inst=%d (%d verts, %d tris, grid %d,%d)"),
                        MeshSpawned, *M.ElementName, Inst, M.Vertices.Num(), M.Triangles.Num() / 3, Col, Row);
                    Loc += FVector(100.0f, 0.0f, 0.0f);
                }
            }
        }

        // Point clouds
        if (bSpawnPointClouds && PointCloudData.Num() > 0)
        {
            for (const FJUSYNCPointCloudData& PC : PointCloudData)
            {
                if (!PC.IsValid()) continue;

                if (bUseSpawner)
                {
                    // Use budget spawner (async, throttled, gradient LUT supported)
                    PCSpawner->SetSpawnLocation(Loc);
                    PCSpawner->SetSpawnScale(SpawnScaleFactor);
                    PCSpawner->EnqueuePointCloud(PC, 0);
                    PCSpawned++;

                    UE_LOG(LogTemp, Display, TEXT("[TestActor] PC #%d: %s inst=%d (%d pts, grid %d,%d) [async spawner]"),
                        PCSpawned, *PC.ElementName, Inst, PC.PointCount, Col, Row);
                }
                else
                {
                    // Fallback: direct spawn (unthrottled, use cautiously)
                    AActor* Spawned = UJUSYNCBlueprintLibrary::SpawnPointCloudAtLocation(PC, Loc, FRotator::ZeroRotator, FVector(1.0f));
                    if (Spawned)
                    {
                        SpawnedActors.Add(Spawned);
                        PCSpawned++;
                    }
                }
                Loc += FVector(100.0f, 100.0f, 0.0f);
            }
        }
    }

    // Debug summary on screen
    GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green,
        FString::Printf(TEXT("[TestActor] Meshes: %d"), MeshSpawned));
    GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Blue,
        FString::Printf(TEXT("[TestActor] PCs: %d (%s)"), PCSpawned, bUseSpawner ? TEXT("async spawner") : TEXT("direct")));
    UE_LOG(LogTemp, Display, TEXT("[TestActor] DONE: %s => meshes=%d, PCs=%d (inst=%d, grid %d cols)"),
        *Filename, MeshSpawned, PCSpawned, SpawnCount, GridColumns);
}
                }
                // If no custom material but we have a PNG texture, apply it to the default material after spawn

                AActor* Spawned = UJUSYNCBlueprintLibrary::SpawnRealtimeMeshAtLocation(M, Loc, FRotator::ZeroRotator, SpawnMat);
                if (Spawned)
                {
                    SpawnedActors.Add(Spawned);
                    MeshSpawned++;

                    if (SpawnScaleFactor > 0.0f && SpawnScaleFactor != 1.0f)
                    {
                        Spawned->SetActorScale3D(FVector(SpawnScaleFactor));
                    }

                    // If we only have texture but no custom material, apply texture to default material
                    if (!SpawnMaterial && LoadedTex)
                    {
                        ApplyTextureToMesh(Spawned, LoadedTex);
                    }

                    UE_LOG(LogTemp, Display, TEXT("[TestActor] Mesh #%d: %s inst=%d (%d verts, %d tris, grid %d,%d)"),
                        MeshSpawned, *M.ElementName, Inst, M.Vertices.Num(), M.Triangles.Num() / 3, Col, Row);
                    Loc += FVector(100.0f, 0.0f, 0.0f);
                }
            }
        }

        // Point clouds — use the budgeted spawner to avoid CPU/async thread explosion
        if (bSpawnPointClouds && PointCloudData.Num() > 0)
        {
            for (const FJUSYNCPointCloudData& PC : PointCloudData)
            {
                if (!PC.IsValid()) continue;

                // Budget: don't enqueue if spawner queue is too deep — throttle with short ticks
                while (PCSpawner && PCSpawner->GetReadyQueueCount() > 16 && PCSpawner->GetActiveActorCount() > 8)
                {
                    FPlatformProcess::Sleep(0.02f);
                }

                if (PCSpawner)
                {
                    PCSpawner->SetSpawnLocation(Loc);
                    PCSpawner->EnqueuePointCloud(PC, 0);
                    SpawnedActors.Add(nullptr);  // Placeholder; spawner tracks its own actors
                    PCSpawned++;

                    UE_LOG(LogTemp, Display, TEXT("[TestActor] PC #%d: %s inst=%d (%d pts, grid %d,%d) [async spawner]"),
                        PCSpawned, *PC.ElementName, Inst, PC.PointCount, Col, Row);
                }

                Loc += FVector(100.0f, 100.0f, 0.0f);
            }
        }
    }

    // Debug summary on screen
    GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green,
        FString::Printf(TEXT("[TestActor] Meshes: %d"), MeshSpawned));
    GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Blue,
        FString::Printf(TEXT("[TestActor] PCs: %d (via async spawner)"), PCSpawned));
    UE_LOG(LogTemp, Display, TEXT("[TestActor] DONE: %s => meshes=%d, PCs=%d (inst=%d, grid %d cols)"),
        *Filename, MeshSpawned, PCSpawned, SpawnCount, GridColumns);
}

UTexture2D* AJUSYNCPointCloudTestActor::LoadPngAsTexture(const TArray<uint8>& PngData)
{
    IImageWrapperModule& ImgMod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));

    TSharedPtr<IImageWrapper> Img = ImgMod.CreateImageWrapper(EImageFormat::PNG);
    if (!Img.IsValid() || !Img->SetCompressed(PngData.GetData(), PngData.Num())) return nullptr;

    TArray<uint8> Raw;
    if (!Img->GetRaw(ERGBFormat::BGRA, 8, Raw)) return nullptr;

    int32 W = Img->GetWidth();
    int32 H = Img->GetHeight();

    TArray<FColor> Colors;
    Colors.SetNum(Raw.Num() / 4);
    for (int32 i = 0; i < Colors.Num(); ++i)
    {
        Colors[i] = FColor(Raw[i*4+2], Raw[i*4+1], Raw[i*4+0], Raw[i*4+3]);
    }

    UTexture2D* Tex = FImageUtils::CreateTexture2D(W, H, Colors, this, TEXT("JUSYNCTex"), RF_Transient, FCreateTexture2DParameters());
    if (!Tex) return nullptr;

    Tex->SRGB = true;
    Tex->Filter = TF_Default;
    Tex->AddToRoot();
    Tex->UpdateResource();
    return Tex;
}

void AJUSYNCPointCloudTestActor::ApplyTextureToMesh(AActor* Spawned, UTexture2D* Tex)
{
    if (!Spawned) return;
    UPrimitiveComponent* RootComp = Cast<UPrimitiveComponent>(Spawned->GetRootComponent());
    if (!RootComp) return;

    UMaterialInterface* BaseMatIntf = RootComp->GetMaterial(0);
    if (!BaseMatIntf) return;

    UMaterialInstanceDynamic* MatInst = UMaterialInstanceDynamic::Create(BaseMatIntf, this);
    if (!MatInst) return;

    // Determine parameter name
    FName ParamName = TEXT("BaseColor");
    if (!TextureParameterName.IsEmpty())
    {
        ParamName = *TextureParameterName;
    }

    // Try to set texture parameter (silently handles missing params)
    MatInst->SetTextureParameterValue(ParamName, Tex);
    RootComp->SetMaterial(0, MatInst);
}

TArray<FColor> AJUSYNCPointCloudTestActor::BuildGradientLUTFromPng(const TArray<uint8>& PngData)
{
    TArray<FColor> OutLUT;
    IImageWrapperModule& ImgMod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));

    TSharedPtr<IImageWrapper> Img = ImgMod.CreateImageWrapper(EImageFormat::PNG);
    if (!Img.IsValid() || !Img->SetCompressed(PngData.GetData(), PngData.Num())) return OutLUT;

    TArray<uint8> Raw;
    if (!Img->GetRaw(ERGBFormat::BGRA, 8, Raw)) return OutLUT;

    int32 W = Img->GetWidth();
    int32 H = Img->GetHeight();
    if (W < 1 || H < 1) return OutLUT;

    const int32 LUTSize = FMath::Min(W, 256);
    TArray<int32> R, G, B, A;
    R.SetNumZeroed(LUTSize);
    G.SetNumZeroed(LUTSize);
    B.SetNumZeroed(LUTSize);
    A.SetNumZeroed(LUTSize);

    for (int32 y = 0; y < H; ++y)
    {
        for (int32 x = 0; x < LUTSize; ++x)
        {
            const uint8* Px = &Raw[(y * W + x) * 4];
            R[x] += Px[0];
            G[x] += Px[1];
            B[x] += Px[2];
            A[x] += Px[3];
        }
    }

    const int32 Rows = H;
    OutLUT.Reserve(LUTSize);
    for (int32 x = 0; x < LUTSize; ++x)
    {
        OutLUT.Add(FColor(R[x] / Rows, G[x] / Rows, B[x] / Rows, A[x] / Rows));
    }

    UE_LOG(LogTemp, Display, TEXT("[TestActor] Built Gradient LUT from PNG: %d x %d → %d colors"), W, H, LUTSize);
    return OutLUT;
}

void AJUSYNCPointCloudTestActor::ClearSpawnedActors()
{
    for (AActor* Actor : SpawnedActors)
    {
        if (Actor) Actor->Destroy();
    }
    SpawnedActors.Empty();

    for (UTexture2D* Txt : LoadedTextures)
    {
        if (Txt) Txt->RemoveFromRoot();
    }
    LoadedTextures.Empty();
}
