#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "JUSYNCTypes.h"

class URealtimeMeshComponent;
class UMaterialInstanceDynamic;

class JUSYNC_API FJUSYNCMeshMorpher : public FTickableGameObject
{
public:
    static const FName MorphTParameter;  // "JusyncMorphT"
    static const FName MorphActiveParameter;  // "JusyncMorphActive"

    explicit FJUSYNCMeshMorpher(TWeakObjectPtr<UObject> InOwner);

    enum class EApplyMode : uint8 { Normal, Blend };

    static bool IsMaterialMorphCapable(const UMaterialInterface* Material);

    EApplyMode BeginUpdate(URealtimeMeshComponent* Comp, UMaterialInterface* IntentMaterial,
                           const FJUSYNCCompactMeshData& MeshData,
                           double NowSeconds, TArray<FVector3f>& OutPrevPoints);

    void Commit(URealtimeMeshComponent* Comp, const FJUSYNCCompactMeshData& MeshData,
                double NowSeconds, bool bBlended, TArray<FVector3f>&& UploadedPrev);

    void Seed(URealtimeMeshComponent* Comp, const FJUSYNCCompactMeshData& MeshData, double NowSeconds);

    void Forget(URealtimeMeshComponent* Comp);

    bool IsEnabled() const;

    virtual void Tick(float DeltaTime) override;
    virtual bool IsTickable() const override;
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FJUSYNCMeshMorpher, STATGROUP_Tickables); }
#if WITH_EDITOR
    virtual bool IsTickableInEditor() const override { return true; }
#endif

private:
    struct FEntry
    {
        TArray<FVector3f> PrevPoints;
        TArray<FVector3f> CurrPoints;
        uint64 TriHash = 0;

        double LastArrival = 0.0;
        double BlendStart = 0.0;
        double Duration = 0.0;
        bool bBlending = false;
        bool bHasTriHash = false;
        bool bBlendByMatch = false;

        TWeakObjectPtr<UMaterialInstanceDynamic> MorphMID;
    };

    static uint64 ComputeTriHash(const TArray<int32>& Triangles);

    UMaterialInstanceDynamic* ResolveMorphMID(URealtimeMeshComponent* Comp, FEntry& Entry);

    TWeakObjectPtr<UObject> Owner;
    TMap<TWeakObjectPtr<URealtimeMeshComponent>, FEntry> Entries;
};
