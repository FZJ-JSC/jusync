#pragma once

#include "CoreMinimal.h"
#include "RealtimeMeshSimple.h"

inline bool JUSYNCRealtimeMeshHasSectionGroup(
	const URealtimeMeshSimple* RealtimeMesh, const FRealtimeMeshSectionGroupKey& GroupKey)
{
	if (!RealtimeMesh)
	{
		return false;
	}

	const FRealtimeMeshLODKey LODKey = GroupKey.LOD();
	if (!RealtimeMesh->GetLODs().Contains(LODKey))
	{
		return false;
	}

	return RealtimeMesh->GetSectionGroups(LODKey).Contains(GroupKey);
}
