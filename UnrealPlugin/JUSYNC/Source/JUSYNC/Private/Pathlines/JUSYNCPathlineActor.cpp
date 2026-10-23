#include "JUSYNCPathlineActor.h"

#include "JUSYNCStreamBuilder.h"
#include "Materials/MaterialInterface.h"
#include "RealtimeMeshComponent.h"
#include "RealtimeMeshSimple.h"

AJUSYNCPathlineActor::AJUSYNCPathlineActor()
{
	PrimaryActorTick.bCanEverTick = false;

	MeshComponent = CreateDefaultSubobject<URealtimeMeshComponent>(TEXT("PathlineMesh"));
	RootComponent = MeshComponent;
	MeshComponent->SetMobility(EComponentMobility::Movable);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MeshComponent->SetGenerateOverlapEvents(false);
	MeshComponent->SetCastShadow(true);
	MeshComponent->SetReceivesDecals(false);

	SetActorEnableCollision(false);
}

void AJUSYNCPathlineActor::ApplyMesh(const FJUSYNCCompactMeshData& Mesh,
	UMaterialInterface* Material)
{
	if (!MeshComponent)
	{
		return;
	}

	if (!Mesh.IsValid())
	{
		ClearMesh();
		return;
	}

	URealtimeMeshSimple* RealtimeMesh = Cast<URealtimeMeshSimple>(MeshComponent->GetRealtimeMesh());
	if (!RealtimeMesh)
	{
		RealtimeMesh = MeshComponent->InitializeRealtimeMesh<URealtimeMeshSimple>();
	}
	if (!RealtimeMesh)
	{
		return;
	}

	RealtimeMesh::FRealtimeMeshStreamSet Streams;
	if (!JUSYNCBuildRealtimeMeshStreams(Mesh, Streams))
	{
		ClearMesh();
		return;
	}

	const FRealtimeMeshSectionGroupKey GroupKey =
		FRealtimeMeshSectionGroupKey::Create(0, TEXT("USDGroup"));
	const FRealtimeMeshSectionKey SectionKey =
		FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);

	if (RealtimeMesh->GetSectionGroup(GroupKey))
	{
		RealtimeMesh->UpdateSectionGroup(GroupKey, MoveTemp(Streams));
	}
	else
	{
		RealtimeMesh->CreateSectionGroup(GroupKey, MoveTemp(Streams));
	}

	FRealtimeMeshSectionConfig SectionConfig(0);
	SectionConfig.bIsVisible = true;
	SectionConfig.bCastsShadow = true;
	RealtimeMesh->UpdateSectionConfig(SectionKey, SectionConfig, false);

	if (Material)
	{
		MeshComponent->SetMaterial(0, Material);
	}

	MeshComponent->SetVisibility(true, true);
	MeshComponent->MarkRenderStateDirty();
}

void AJUSYNCPathlineActor::ClearMesh()
{
	if (!MeshComponent)
	{
		return;
	}

	if (URealtimeMeshSimple* RealtimeMesh = Cast<URealtimeMeshSimple>(MeshComponent->GetRealtimeMesh()))
	{
		const FRealtimeMeshSectionGroupKey GroupKey =
			FRealtimeMeshSectionGroupKey::Create(0, TEXT("USDGroup"));
		if (RealtimeMesh->GetSectionGroup(GroupKey))
		{
			RealtimeMesh->RemoveSectionGroup(GroupKey);
		}
	}

	MeshComponent->SetVisibility(false, true);
	MeshComponent->MarkRenderStateDirty();
}
