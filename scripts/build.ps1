#Requires -Version 5.1
<#
.SYNOPSIS
    Build script for SteamDeckHID KMDF driver.

.DESCRIPTION
    Locates MSBuild and builds the driver via the WDK toolset.
    Stampinf, inf2cat, and test-signing are all handled by MSBuild/WDK targets.
    Run scripts\create-test-cert.ps1 once before using -Sign.

.PARAMETER Configuration
    Build configuration: Debug, Release, or Both (default: Both)

.PARAMETER Sign
    If set, builds with TestSign mode (requires a test cert in the local store).
    If omitted, signing is disabled.

.PARAMETER Clean
    If set, cleans output before building.

.EXAMPLE
    .\scripts\build.ps1
    .\scripts\build.ps1 -Configuration Debug -Sign
    .\scripts\build.ps1 -Configuration Release -Sign -Clean
#>

[CmdletBinding()]
param(
    [ValidateSet("Debug","Release","Both")]
    [string]$Configuration = "Both",

    [switch]$Sign,
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
$ScriptDir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot     = Split-Path -Parent $ScriptDir
$SolutionFile = Join-Path $RepoRoot "SteamDeckHID.sln"

# ---------------------------------------------------------------------------
# Locate MSBuild
# ---------------------------------------------------------------------------
Write-Host "`n==> Locating MSBuild" -ForegroundColor Cyan

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Host "[FAIL] vswhere.exe not found. Is Visual Studio 2022 installed?" -ForegroundColor Red
    exit 1
}

$vsInstallPath = & $vswhere -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop `
    -property installationPath 2>$null
if (-not $vsInstallPath) {
    Write-Host "[FAIL] Visual Studio 2022 with C++ workload not found." -ForegroundColor Red
    exit 1
}

$msbuild = Join-Path $vsInstallPath "MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild)) {
    Write-Host "[FAIL] MSBuild.exe not found at: $msbuild" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] MSBuild: $msbuild" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
if ($Clean) {
    Write-Host "`n==> Cleaning" -ForegroundColor Cyan
    foreach ($d in @("bin","obj","x64")) {
        $path = Join-Path $RepoRoot $d
        if (Test-Path $path) {
            Remove-Item -Path $path -Recurse -Force
            Write-Host "[OK] Removed: $path" -ForegroundColor Green
        }
    }
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
$configs = if ($Configuration -eq "Both") { @("Debug","Release") } else { @($Configuration) }
$signMode = if ($Sign) { "TestSign" } else { "Off" }

foreach ($cfg in $configs) {
    Write-Host "`n==> Building $cfg|x64 (SignMode=$signMode)" -ForegroundColor Cyan

    $msbuildArgs = @(
        $SolutionFile,
        "/p:Configuration=$cfg",
        "/p:Platform=x64",
        "/p:SignMode=$signMode",
        "/m",
        "/v:minimal",
        "/nologo"
    )

    & $msbuild @msbuildArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[FAIL] MSBuild failed for $cfg" -ForegroundColor Red
        exit 1
    }

    $outDir = Join-Path $RepoRoot "bin\$cfg\x64"
    Write-Host "[OK] Build succeeded: $outDir" -ForegroundColor Green
    if (Test-Path (Join-Path $outDir "SteamDeckHID\SteamDeckHID.inf")) {
        Write-Host "     - $outDir\SteamDeckHID\" -ForegroundColor Green
    }
    if (Test-Path (Join-Path $outDir "SteamDeckBus\SteamDeckBus.inf")) {
        Write-Host "     - $outDir\SteamDeckBus\" -ForegroundColor Green
    }
}

Write-Host "`n==> Build complete!" -ForegroundColor Green
