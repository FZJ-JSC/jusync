#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "JUSYNCBenchmarkTiming.generated.h"

/**
 * Measured interactive-window statistics.
 *
 * Values are measured from real game-thread ticks only. If the window was
 * not opened or produced no frames, bValid is false and all values are 0.
 * There is intentionally no synthetic fallback: an unmeasured FPS must be
 * reported as 0, never invented.
 */
USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCInteractiveStats
{
	GENERATED_BODY()

	// True only if at least one frame was measured inside the window
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	bool bValid = false;

	// Frames per second over the whole window
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	double FPS = 0.0;

	// Mean frame time in ms over the window
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	double FrameTimeMeanMs = 0.0;

	// Worst (slowest) frame in the window, ms
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	double FrameTimeMaxMs = 0.0;

	// Number of frames counted
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	int64 FrameCount = 0;

	// Actual wall length of the measurement window, ms
	UPROPERTY(BlueprintReadOnly, Category = "JUSYNC|Benchmark")
	double WindowMs = 0.0;
};

/**
 * Time-to-frame session statistics (wall clock, ms).
 *
 * FirstFrameMs: connect start -> first rendered frame after at least one
 *               spawned actor existed in the scene.
 * FullSceneMs:  connect start -> last actor of the initial spawn created.
 */
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

/**
 * Singleton holder for all benchmark timing state.
 *
 * All public methods must be called from the game thread (blueprint calls
 * and the spawner pipeline already run there). The per-frame tick is driven
 * by FTSTicker on the game thread.
 */
class JUSYNC_API FJUSYNCBenchmarkTiming
{
public:
	static FJUSYNCBenchmarkTiming& Get();

	// ---- interactive FPS window (game time, FApp::GetCurrentTime) ----

	// Opens a measurement window of the given duration. The window closes
	// itself when the duration elapses, or early via EndInteractiveWindow.
	void BeginInteractiveWindow(double DurationSeconds);

	void EndInteractiveWindow();

	FJUSYNCInteractiveStats GetInteractiveStats() const;

	// ---- time-to-frame session (wall clock, FPlatformTime::Seconds) ----

	// Call when the broker connection starts (pipeline cold start).
	void MarkConnectStart();

	// Call when the first spawned actor enters the scene. The actual
	// timestamp is taken on the next rendered frame (the first frame in
	// which that actor can draw).
	void MarkFirstActorSpawned();

	// Call when the initial spawn is fully complete.
	void MarkFullScene();

	FJUSYNCBenchmarkTTF GetTTF() const;

	// Clears all session state (interactive window + TTF).
	void Reset();

private:
	FJUSYNCBenchmarkTiming();
	~FJUSYNCBenchmarkTiming();

	void TickFrame(float DeltaTime);

	// ---- ticker ----
	// NOTE: FTSTicker::FDelegateHandle is a nested alias (TWeakPtr), NOT the
	// global FDelegateHandle class
	FTSTicker::FDelegateHandle TickerHandle;

	// ---- interactive window state (game thread) ----
	bool bWindowOpen = false;
	double WindowStartGameTime = 0.0;
	double WindowEndGameTime = 0.0;
	double LastTickGameTime = 0.0;
	int64 FramesInWindow = 0;
	double FrameTimeSumMs = 0.0;
	double FrameTimeMaxMs = 0.0;
	double WindowActualMs = 0.0;
	bool bWindowMeasured = false;

	// ---- TTF state (game thread, wall clock seconds) ----
	bool bConnectStarted = false;
	double ConnectStartWall = 0.0;
	bool bFirstSpawnPending = false;
	double FirstFrameWall = -1.0;
	double FullSceneWall = -1.0;

	// Full-scene completion that arrived before any frame rendered (e.g. the
	// engine was backgrounded/throttled while spawning). We remember the
	// creation moment here and take FullSceneWall on the first rendered frame
	// so that FirstFrameMs <= FullSceneMs always holds.
	bool bFullScenePending = false;
	double FullSceneCandidateWall = -1.0;
};
