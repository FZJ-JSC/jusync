#include "JUSYNCBenchmarkTiming.h"

#include "Misc/CoreDelegates.h"
#include "HAL/PlatformTime.h"
#include "Engine/Engine.h"

FJUSYNCBenchmarkTiming& FJUSYNCBenchmarkTiming::Get()
{
	static FJUSYNCBenchmarkTiming Instance;
	return Instance;
}

FJUSYNCBenchmarkTiming::FJUSYNCBenchmarkTiming()
{
	// Per-frame game-thread tick. One registration for the process lifetime;
	// the per-frame work is a handful of atomic-free member reads/writes.
	// FTickerDelegate is TDelegate<bool(float)>: return true to keep ticking
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float DeltaTime) -> bool
		{
			TickFrame(DeltaTime);
			return true;
		}),
		0.0f);
}

FJUSYNCBenchmarkTiming::~FJUSYNCBenchmarkTiming()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	}
}

void FJUSYNCBenchmarkTiming::TickFrame(float DeltaTime)
{
	// Stamp the first rendered frame after the first actor spawn
	if (bFirstSpawnPending)
	{
		bFirstSpawnPending = false;
		if (FirstFrameWall < 0.0)
		{
			FirstFrameWall = FPlatformTime::Seconds();
		}
		// If full-scene completion was recorded while no frame had rendered
		// yet (backgrounded/throttled spawn), take the full-scene timestamp on
		// this same frame so FirstFrameMs <= FullSceneMs always holds.
		if (bFullScenePending)
		{
			bFullScenePending = false;
			FullSceneWall = FPlatformTime::Seconds();
		}
	}

	// Interactive window measurement (game time)
	if (!bWindowOpen)
	{
		return;
	}

	const double Now = FApp::GetCurrentTime();

	if (FramesInWindow == 0)
	{
		// First tick inside the window: no previous frame to diff against
		LastTickGameTime = Now;
	}
	else
	{
		const double DtMs = (Now - LastTickGameTime) * 1000.0;
		LastTickGameTime = Now;
		if (DtMs >= 0.0)
		{
			++FramesInWindow;
			FrameTimeSumMs += DtMs;
			FrameTimeMaxMs = FMath::Max(FrameTimeMaxMs, DtMs);
		}
	}

	if (Now >= WindowEndGameTime)
	{
		bWindowOpen = false;
		WindowActualMs = (Now - WindowStartGameTime) * 1000.0;
		bWindowMeasured = FramesInWindow > 0;
	}
}

void FJUSYNCBenchmarkTiming::BeginInteractiveWindow(double DurationSeconds)
{
	const double Now = FApp::GetCurrentTime();
	bWindowOpen = true;
	bWindowMeasured = false;
	WindowStartGameTime = Now;
	WindowEndGameTime = Now + FMath::Max(0.0, DurationSeconds);
	LastTickGameTime = Now;
	FramesInWindow = 0;
	FrameTimeSumMs = 0.0;
	FrameTimeMaxMs = 0.0;
	WindowActualMs = 0.0;

	UE_LOG(LogTemp, Display, TEXT("[JUSYNC Benchmark] interactive window open for %.1f s"), DurationSeconds);
}

void FJUSYNCBenchmarkTiming::EndInteractiveWindow()
{
	if (!bWindowOpen)
	{
		return;
	}
	const double Now = FApp::GetCurrentTime();
	bWindowOpen = false;
	WindowActualMs = (Now - WindowStartGameTime) * 1000.0;
	bWindowMeasured = FramesInWindow > 0;

	UE_LOG(LogTemp, Display, TEXT("[JUSYNC Benchmark] interactive window closed after %.0f ms (%lld frames)"),
		WindowActualMs, FramesInWindow);
}

FJUSYNCInteractiveStats FJUSYNCBenchmarkTiming::GetInteractiveStats() const
{
	FJUSYNCInteractiveStats Stats;
	if (!bWindowMeasured || FramesInWindow <= 0)
	{
		Stats.bValid = false;
		return Stats;
	}

	Stats.bValid = true;
	Stats.FrameCount = FramesInWindow;
	Stats.WindowMs = WindowActualMs;
	Stats.FrameTimeMeanMs = FrameTimeSumMs / static_cast<double>(FramesInWindow);
	Stats.FrameTimeMaxMs = FrameTimeMaxMs;
	Stats.FPS = (WindowActualMs > 0.0)
		? static_cast<double>(FramesInWindow) / (WindowActualMs / 1000.0)
		: 0.0;
	return Stats;
}

void FJUSYNCBenchmarkTiming::MarkConnectStart()
{
	bConnectStarted = true;
	ConnectStartWall = FPlatformTime::Seconds();
	bFirstSpawnPending = false;
	FirstFrameWall = -1.0;
	FullSceneWall = -1.0;
	bFullScenePending = false;
	FullSceneCandidateWall = -1.0;

	UE_LOG(LogTemp, Display, TEXT("[JUSYNC Benchmark] TTF session started (connect)"));
}

void FJUSYNCBenchmarkTiming::MarkFirstActorSpawned()
{
	if (!bConnectStarted)
	{
		return;
	}
	if (FirstFrameWall >= 0.0)
	{
		return; // already captured
	}
	// Actual timestamp is taken on the next rendered frame in TickFrame
	bFirstSpawnPending = true;
}

void FJUSYNCBenchmarkTiming::MarkFullScene()
{
	if (!bConnectStarted)
	{
		return;
	}
	if (FullSceneWall >= 0.0 || bFullScenePending)
	{
		return; // already captured (or already deferred to the first frame)
	}
	const double Now = FPlatformTime::Seconds();
	if (FirstFrameWall >= 0.0)
	{
		// A frame already rendered after the first spawn: full scene is now.
		FullSceneWall = Now;
		return;
	}
	// No rendered frame yet: defer the full-scene timestamp until the first
	// rendered frame so the FirstFrameMs <= FullSceneMs invariant holds.
	bFullScenePending = true;
	FullSceneCandidateWall = Now;
}

FJUSYNCBenchmarkTTF FJUSYNCBenchmarkTiming::GetTTF() const
{
	FJUSYNCBenchmarkTTF TTF;
	if (!bConnectStarted)
	{
		TTF.bValid = false;
		return TTF;
	}

	TTF.bValid = true;
	if (FirstFrameWall > 0.0)
	{
		TTF.FirstFrameMs = (FirstFrameWall - ConnectStartWall) * 1000.0;
	}
	if (FullSceneWall > 0.0)
	{
		TTF.FullSceneMs = (FullSceneWall - ConnectStartWall) * 1000.0;
	}
	else if (FullSceneCandidateWall > 0.0)
	{
		// Session ended before any frame rendered: report the scene creation
		// time (full_scene >= first_frame still holds, first may be 0).
		TTF.FullSceneMs = (FullSceneCandidateWall - ConnectStartWall) * 1000.0;
	}
	return TTF;
}

void FJUSYNCBenchmarkTiming::Reset()
{
	bWindowOpen = false;
	bWindowMeasured = false;
	FramesInWindow = 0;
	FrameTimeSumMs = 0.0;
	FrameTimeMaxMs = 0.0;
	WindowActualMs = 0.0;

	bConnectStarted = false;
	ConnectStartWall = 0.0;
	bFirstSpawnPending = false;
	FirstFrameWall = -1.0;
	FullSceneWall = -1.0;
	bFullScenePending = false;
	FullSceneCandidateWall = -1.0;
}
