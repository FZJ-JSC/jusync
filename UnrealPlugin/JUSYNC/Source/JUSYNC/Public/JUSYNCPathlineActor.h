#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "JUSYNCTypes.h"
#include "JUSYNCPathlineActor.generated.h"

class UMaterialInterface;
class URealtimeMeshComponent;

/**
 * One RealtimeMesh actor per canonical logical element. The manager owns the
 * CPU pathline state; this actor only owns the GPU mirror on the game thread.
 */
UCLASS()
class JUSYNC_API AJUSYNCPathlineActor : public AActor
{
	GENERATED_BODY()

public:
	AJUSYNCPathlineActor();

	void ApplyMesh(const FJUSYNCCompactMeshData& Mesh, UMaterialInterface* Material);
	void ClearMesh();

private:
	UPROPERTY(VisibleAnywhere, Category = "JUSYNC")
	TObjectPtr<URealtimeMeshComponent> MeshComponent;
};
