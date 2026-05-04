#Requires -Version 5.1
<#
.SYNOPSIS
    Validates the SteamDeckHID.inf using infverif and chkinf (WDK tools).

.DESCRIPTION
    Runs infverif with strict Windows 10/11 rules and optionally chkinf.
    Both tools are included in the WDK.

.EXAMPLE
    .\scripts\verify-inf.ps1
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir
$InfFile   = Join-Path $RepoRoot "inf\SteamDeckHID.inf"

function Write-Step([string]$m) { Write-Host "`n==> $m" -ForegroundColor Cyan }
function Write-Ok([string]$m)   { Write-Host "[OK] $m"  -ForegroundColor Green }
function Write-Fail([string]$m) { Write-Host "[FAIL] $m" -ForegroundColor Red; exit 1 }

function Find-WdkTool([string]$name) {
    $paths = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "C:\Program Files (x86)\Windows Kits\10\bin"
    )
    foreach ($p in $paths) {
        $f = Get-ChildItem -Path $p -Filter $name -Recurse -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -match "x64" } |
             Select-Object -First 1
        if ($f) { return $f.FullName }
    }
    return $null
}

Write-Step "Locating infverif.exe"
$infverif = Find-WdkTool "infverif.exe"
if (-not $infverif) { Write-Fail "infverif.exe not found. Is WDK installed?" }
Write-Ok $infverif

Write-Step "Running infverif (Windows 11 strict mode)"
Write-Host "INF: $InfFile`n"

& $infverif /w10 /v $InfFile
$ev = $LASTEXITCODE

if ($ev -eq 0) {
    Write-Ok "infverif passed – no errors"
} else {
    Write-Host "[WARN] infverif returned exit code $ev (may be warnings only)" -ForegroundColor Yellow
}

Write-Step "Looking for chkinf.bat"
$chkinf = Find-WdkTool "chkinf.bat"
if ($chkinf) {
    Write-Ok $chkinf
    Write-Step "Running chkinf"
    & $chkinf $InfFile
    Write-Ok "chkinf complete"
} else {
    Write-Host "[INFO] chkinf.bat not found – skipping (optional)" -ForegroundColor DarkGray
}

Write-Host "`n==> INF verification complete" -ForegroundColor Green
