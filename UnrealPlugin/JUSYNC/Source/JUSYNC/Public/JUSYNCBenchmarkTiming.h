#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "JUSYNCBenchmarkTiming.generated.h"

USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCInteractiveStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	bool bValid = false;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	double FPS = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	double FrameTimeMeanMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	double FrameTimeMaxMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int64 FrameCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	double WindowMs = 0.0;
};

USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCBenchmarkTTF
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	bool bValid = false;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	double FirstFrameMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	double FullSceneMs = 0.0;
};

class JUSYNC_API FJUSYNCBenchmarkTiming
{
public:
	static FJUSYNCBenchmarkTiming& Get();


	void BeginInteractiveWindow(double DurationSeconds);

	void EndInteractiveWindow();

	FJUSYNCInteractiveStats GetInteractiveStats() const;


	void MarkConnectStart();

	void MarkFirstActorSpawned();

	void MarkFullScene();

	FJUSYNCBenchmarkTTF GetTTF() const;

	void Reset();

private:
	FJUSYNCBenchmarkTiming();
	~FJUSYNCBenchmarkTiming();

	void TickFrame(float DeltaTime);

	FTSTicker::FDelegateHandle TickerHandle;

	bool bWindowOpen = false;
	double WindowStartGameTime = 0.0;
	double WindowEndGameTime = 0.0;
	double LastTickGameTime = 0.0;
	int64 FramesInWindow = 0;
	double FrameTimeSumMs = 0.0;
	double FrameTimeMaxMs = 0.0;
	double WindowActualMs = 0.0;
	bool bWindowMeasured = false;

	bool bConnectStarted = false;
	double ConnectStartWall = 0.0;
	bool bFirstSpawnPending = false;
	double FirstFrameWall = -1.0;
	double FullSceneWall = -1.0;

	bool bFullScenePending = false;
	double FullSceneCandidateWall = -1.0;
};
