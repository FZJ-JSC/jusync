# Deploy the JUSYNC UE plugin from this repo (source of truth) into the UE
# sample project, plus the freshly built middleware DLL. The plugin tree in
# the sample drifts (missing Phase 0/1 sources) without this.
#
#   .\tools\deploy-ue-plugin.ps1                      # copy sources + Release DLL
#   .\tools\deploy-ue-plugin.ps1 -WhatIf              # robocopy /L dry run
#   .\tools\deploy-ue-plugin.ps1 -Diagnostics         # take DLL from build-diag\Release
#
# Closes UE Editor first: running it with the editor open corrupts hot-loaded
# binaries. Built UE module DLLs (UnrealEditor-JUSYNC*.dll) are left untouched;
# rebuild them from the editor / UBT after syncing sources.
param(
    [string]$RepoRoot   = (Split-Path -Parent $PSScriptRoot),
    [string]$SampleRoot = 'D:\JSC_Github\jusync-uesample',
    [ValidateSet('Release','Diagnostics')][string]$DllConfig = 'Release',
    [switch]$WhatIf
)
$ErrorActionPreference = 'Stop'

$srcPlugin = Join-Path $RepoRoot 'UnrealPlugin\JUSYNC'
$dstPlugin = Join-Path $SampleRoot 'Plugins\JUSYNC'
if (-not (Test-Path $srcPlugin)) { throw "plugin source not found: $srcPlugin" }
if (-not (Test-Path $dstPlugin)) { throw "sample project plugin dir not found: $dstPlugin" }

$roboArgs = @($srcPlugin, $dstPlugin, '/E', '/NJH', '/NDL', '/NFL',
              '/XD', 'Binaries', 'Intermediate', 'Saved',
              '/XF', '*.dll', '*.lib', '*.pdb', '*.exp')
if ($WhatIf) { $roboArgs += '/L' }

Write-Host "== Sync plugin sources ==" -ForegroundColor Cyan
& robocopy @roboArgs | Out-Host
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with code $LASTEXITCODE" }

# Middleware DLL (Phase 0+1 API incl. JusyncDiag* exports) into the plugin's
# Binaries next to the UE module DLLs. Prefer the newest build available.
$dllCandidates = @(
    (Join-Path $RepoRoot "build-diag\Release\anari_usd_middleware.dll"),
    (Join-Path $RepoRoot "build\Release\anari_usd_middleware.dll")
) | Where-Object { Test-Path $_ } |
    Sort-Object { (Get-Item $_).LastWriteTime } -Descending
$dll = if ($DllConfig -eq 'Diagnostics') {
    Join-Path $RepoRoot 'build-diag\Release\anari_usd_middleware.dll'
} else {
    $dllCandidates | Select-Object -First 1
}
if (-not $dll) { throw 'no built anari_usd_middleware.dll found - build the middleware first (see docs/plans or build-diag)' }
$dstDll = Join-Path $dstPlugin 'Binaries\Win64\anari_usd_middleware.dll'
if ($WhatIf) {
    Write-Host "== [dry-run] would copy $dll -> $dstDll ==" -ForegroundColor Yellow
} else {
    Copy-Item $dll $dstDll -Force
    Write-Host "== Middleware DLL: $dll -> $dstDll ==" -ForegroundColor Green
}
Write-Host "Next: open the sample project (or Build.bat) so UBT recompiles UnrealEditor-JUSYNC from the synced sources." -ForegroundColor Cyan
