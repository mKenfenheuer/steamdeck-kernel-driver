#Requires -RunAsAdministrator
#Requires -Version 5.1
<#
.SYNOPSIS
    Configure kernel debugging for the Steam Deck (target machine).

.DESCRIPTION
    Sets up WinDbg-over-network (KDNET) kernel debugging.
    Run this on the STEAM DECK (target), not on your dev machine.

.PARAMETER HostIP
    IP address of your development machine running WinDbg.

.PARAMETER Port
    KDNET port (default: 50000). Must be open in dev machine firewall.

.PARAMETER Key
    KDNET connection key in format a.b.c.d (default: auto-generated).

.EXAMPLE
    # On the Steam Deck:
    .\scripts\debug-setup.ps1 -HostIP 192.168.1.100
    .\scripts\debug-setup.ps1 -HostIP 192.168.1.100 -Port 50001 -Key 1.2.3.4
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$HostIP,

    [int]$Port = 50000,
    [string]$Key = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Step([string]$m) { Write-Host "`n==> $m" -ForegroundColor Cyan }
function Write-Ok([string]$m)   { Write-Host "[OK] $m"  -ForegroundColor Green }

# ---------------------------------------------------------------------------
# Enable debug mode
# ---------------------------------------------------------------------------
Write-Step "Enabling kernel debug mode"
& bcdedit /debug on
if ($LASTEXITCODE -ne 0) { throw "bcdedit /debug on failed" }
Write-Ok "Kernel debug enabled"

# ---------------------------------------------------------------------------
# Configure KDNET (network kernel debugging)
# ---------------------------------------------------------------------------
Write-Step "Configuring KDNET: host=$HostIP port=$Port"

if ($Key -ne "") {
    & bcdedit /dbgsettings net hostip:$HostIP port:$Port key:$Key
} else {
    & bcdedit /dbgsettings net hostip:$HostIP port:$Port
}

if ($LASTEXITCODE -ne 0) { throw "bcdedit /dbgsettings failed" }

# Read back the generated key
$bcdOutput = & bcdedit /dbgsettings
$keyLine   = $bcdOutput | Where-Object { $_ -match "key" }
Write-Ok "KDNET configured"
Write-Host $keyLine -ForegroundColor Yellow

# ---------------------------------------------------------------------------
# Enable test signing (needed for our test-signed driver)
# ---------------------------------------------------------------------------
Write-Step "Enabling test-signing mode"
$tsOutput = & bcdedit /enum "{current}" 2>&1
if ($tsOutput -match "testsigning\s+Yes") {
    Write-Ok "Test-signing already enabled"
} else {
    & bcdedit /set testsigning on
    if ($LASTEXITCODE -ne 0) { throw "bcdedit /set testsigning failed" }
    Write-Ok "Test-signing enabled"
}

# ---------------------------------------------------------------------------
# Enable verbose driver verifier for SteamDeckHID
# ---------------------------------------------------------------------------
Write-Step "Enabling Driver Verifier for SteamDeckHID.sys"
# Standard flags: special pool, force IRQL checking, pool tracking, I/O verification
& verifier /standard /driver SteamDeckHID.sys
if ($LASTEXITCODE -ne 0) {
    Write-Host "[WARN] verifier command failed - Driver Verifier not enabled" -ForegroundColor Yellow
} else {
    Write-Ok "Driver Verifier enabled for SteamDeckHID.sys"
}

# ---------------------------------------------------------------------------
# Print WinDbg command for dev machine
# ---------------------------------------------------------------------------
$actualKey = if ($Key -ne "") { $Key } else { "(see key output above)" }

$debugMsg = @"

==> Debug setup complete!  REBOOT REQUIRED.

After reboot, connect from your dev machine with WinDbg:
  windbg -k net:port=$Port,key=$actualKey

Useful WinDbg commands once connected:
  .reload /f SteamDeckHID.sys        - load symbols
  bu SteamDeckHID!DriverEntry        - break on driver load
  bu SteamDeckHID!SteamDeckUsb_ReadComplete  - break on every USB report
  !devobj <addr>                     - inspect device object
  !wdfkd.wdfdevice <handle>          - inspect WDF device
  ed nt!Kd_IHVDRIVER_Mask 0xFFFFFFFF - enable KdPrintEx output
  g                                  - continue

Driver log output is also visible in DebugView (Sysinternals):
  Run DebugView as Admin -> Capture -> Capture Kernel
"@
Write-Host $debugMsg -ForegroundColor Yellow
