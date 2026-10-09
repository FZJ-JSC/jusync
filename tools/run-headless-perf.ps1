# JUSYNC headless streaming perf run (no GUI).
#
# Runs the sample project in -game mode with -nullrhi through UnrealEditor-Cmd,
# auto-spawning a JUSYNCFileSpawnerActor, dumping jusync.diag tables on a timer
# and auto-quitting (see FJUSYNCModule::MaybeStartHeadlessHarness in the plugin).
#
# Example:
#   .\tools\run-headless-perf.ps1 -DiagEvery 10 -DiagCount 6 -QuitAfter 75
#
# Notes:
# - Requires the broker (sender/tunnel) reachable at tcp://localhost:5556.
# - Uses the EDITOR binaries with uncooked assets; no packaging needed.
# - The default map (/Game/Empty.Empty) is minimal on purpose: the harness
#   spawns its own spawner, so numbers reflect the pipeline, not level content.

param(
    [double]$DiagEvery = 10,
    [int]$DiagCount = 6,
    [int]$Rows = 60,
    [double]$QuitAfter = 75,
    [string]$Map = "/Game/Empty.Empty",
    [string]$UProject = "D:\JSC_Github\jusync-uesample\JusyncSampleProject.uproject",
    [string]$EngineDir = "C:\Program Files\Epic Games\UE_5.7",
    [string]$LogDir = (Join-Path $env:TEMP "jusync-perf"),
    [switch]$NoAutoSpawner
)

$ErrorActionPreference = 'Stop'

$exe = Join-Path $EngineDir "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path $exe)) { throw "UnrealEditor-Cmd.exe not found: $exe" }
if (-not (Test-Path $UProject)) { throw "Project not found: $UProject" }

$portOk = Test-NetConnection -ComputerName 127.0.0.1 -Port 5556 -InformationLevel Quiet -WarningAction SilentlyContinue
if (-not $portOk) {
    Write-Warning "tcp://localhost:5556 is not reachable - bring up the broker (SSH tunnel / sender container) first, or the run will only log failures."
}

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = Join-Path $LogDir "perf-$stamp.log"

$cmdArgs = @(
    "`"$UProject`"",
    $Map,
    '-game', '-nullrhi', '-unattended', '-nosplash', '-log', '-stdout',
    "`"-abslog=$log`"",
    "`"-JusyncAutoDiag=$DiagEvery,$DiagCount`"",
    "`"-JusyncAutoDiagRows=$Rows`"",
    "`"-JusyncAutoQuit=$QuitAfter`""
)
if (-not $NoAutoSpawner) { $cmdArgs += '-JusyncAutoSpawner' }

Write-Host "RUN  : $exe"
Write-Host "ARGS : $($cmdArgs -join ' ')"
Write-Host "LOG  : $log"

$proc = Start-Process -FilePath $exe -ArgumentList $cmdArgs -PassThru -Wait
Write-Host "EXIT : $($proc.ExitCode)"

Write-Host ""
Write-Host "===== harness timeline =====" -ForegroundColor Cyan
Select-String -Path $log -Pattern 'jusync\.harness' | ForEach-Object { $_.Line }

Write-Host ""
Write-Host "===== last diag table =====" -ForegroundColor Cyan
$marker = Select-String -Path $log -Pattern 'JUSYNC streaming stage latency' | Select-Object -Last 1
if ($marker) {
    $all = Get-Content $log
    $end = [Math]::Min($all.Count - 1, $marker.LineNumber + $Rows + 1)
    $all[($marker.LineNumber - 1)..$end] | Where-Object { $_ -match 'LogJUSYNC' } | ForEach-Object {
        $_ -replace '^.*LogJUSYNC: Display: ', ''
    }
} else {
    Write-Host "(no diag table found - did streaming never start?)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "===== errors (top 10 distinct) =====" -ForegroundColor Cyan
Select-String -Path $log -Pattern '\bError:' |
    ForEach-Object { ($_.Line -replace '^.*Error: ', '') } |
    Group-Object | Sort-Object Count -Descending | Select-Object -First 10 |
    ForEach-Object { Write-Host ("[{0}x] {1}" -f $_.Count, $_.Name) }
