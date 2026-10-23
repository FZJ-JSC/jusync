#pragma once

#include "CoreMinimal.h"
#include "JUSYNCTypes.h"

class UMaterialInterface;
class UWorld;
class FJUSYNCPointCloudSpawner;

/**
 * User-facing pathline controls. These mirror the details panel controls on
 * AJUSYNCFileSpawnerActor; the manager itself has no UI.
 */
struct JUSYNC_API FJUSYNCPathlineSettings
{
	bool bEnabled = false;

	int32 MaxTracks = 4096;
	int32 HistorySamples = 128;
	int32 MaxNodes = 524288;
	// A track that is not present in N consecutive emitted samples is removed.
	int32 TrackExpirySamples = 32;

	int32 TubeSides = 4;
	float Radius = 0.5f;
	float RadiusScale = 1.0f;
	bool bUsePointWidth = false;

	FString IdAttribute = TEXT("particle_id");
	FString ScalarAttribute = TEXT("attribute0");
	int32 ScalarComponent = 0;

	bool bFilterEnabled = false;
	float FilterMin = 0.0f;
	float FilterMax = 1.0f;

	bool bColorByScalar = true;
	float ColorMin = 0.0f;
	float ColorMax = 1.0f;
	FLinearColor FlatColor = FLinearColor::White;

	// Stable hash rank. Changing the seed re-samples which IDs are tracked.
	uint64 SelectionSeed = 1337;

	UMaterialInterface* Material = nullptr;
};

struct JUSYNC_API FJUSYNCPathlineStats
{
	int32 ActiveTracks = 0;
	int64 TotalNodes = 0;
	int32 VisibleTracks = 0;
	int64 DuplicateIds = 0;
	int64 DroppedSamples = 0;
	int64 IgnoredSamples = 0;
	double LastIngestMs = 0.0;
	double LastBuildMs = 0.0;
};

/**
 * CPU pathline store and mesh builder.
 *
 * Ingestion and geometry construction run on a serialized background worker.
 * The game thread only applies finished meshes (see PumpGameThread). Tests can
 * use IngestSampleNow/BuildGeometry directly without starting the worker.
 *
 * Threading contract: the point-cloud and LUT arguments are copied into the
 * work item. FJUSYNCPointCloudData is treated as immutable after enqueue.
 */
class JUSYNC_API FJUSYNCPathlineManager
{
public:
	FJUSYNCPathlineManager();
	~FJUSYNCPathlineManager();

	FJUSYNCPathlineManager(const FJUSYNCPathlineManager&) = delete;
	FJUSYNCPathlineManager& operator=(const FJUSYNCPathlineManager&) = delete;

	void SetSettings(const FJUSYNCPathlineSettings& NewSettings);
	const FJUSYNCPathlineSettings& GetSettings() const;

	// True when structural settings changed and the file spawner should replay
	// its retained samples through the manager.
	bool ConsumeResampleRequest();

	void Reset();

	void EnqueueSample(const FString& ElementName, const FJUSYNCPointCloudRef& Cloud,
		int32 Rank, uint64 SampleId, const TArray<FColor>& Lut);
	void IngestSampleNow(const FString& ElementName, const FJUSYNCPointCloudRef& Cloud,
		int32 Rank, uint64 SampleId, const TArray<FColor>& Lut);

	// Builds geometry for one canonical element at a sample cutoff.
	// MAX_uint64 means "live". Returns false when the element has no state.
	bool BuildGeometry(const FString& ElementName, uint64 CutoffSampleId,
		FJUSYNCCompactMeshData& OutMesh, int32& OutVisibleTracks);

	void SetDisplaySampleId(uint64 SampleId);
	uint64 GetDisplaySampleId() const;

	// Queue a rebuild from retained state (playback scrub or visual change).
	void RequestRebuild();

	// Drains worker results and uploads them to pathline actors through the
	// spawner. The manager never touches UObjects off the game thread.
	void PumpGameThread(UWorld* World, FJUSYNCPointCloudSpawner* Spawner,
		UMaterialInterface* Material);

	FJUSYNCPathlineStats GetStats() const;

	TArray<FString> GetElementKeys() const;

	// Deterministic selection inspection (tests/tools).
	TArray<uint64> GetTrackIds(const FString& ElementName, int32 Rank) const;
	int32 GetElementNodeCount(const FString& ElementName) const;
	int32 GetTrackNodeCount(const FString& ElementName, int32 Rank, uint64 Id) const;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
