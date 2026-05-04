#Requires -RunAsAdministrator
#Requires -Version 5.1
<#
.SYNOPSIS
    Install, update or uninstall the SteamDeckHID + SteamDeckBus drivers.

.DESCRIPTION
    Two driver packages are produced by the build:
      - SteamDeckHID.sys  (HID minidriver, attaches to the real USB device)
      - SteamDeckBus.sys  (root-enumerated bus, exposes a virtual Xbox 360
                            controller that XInput games see)
    Install does:
      1. pnputil /add-driver SteamDeckHID.inf /install
      2. pnputil /add-driver SteamDeckBus.inf /install
      3. Create the root device node Root\SteamDeckBus via SetupAPI
         (similar to "devcon install Root\SteamDeckBus")

.PARAMETER Action
    install   - add both packages and create the root bus device (default)
    update    - re-add packages over an existing install
    uninstall - remove both packages and the bus device node

.PARAMETER Configuration
    Which build output to install: Debug or Release (default: Debug)
#>

[CmdletBinding()]
param(
    [ValidateSet("install","update","uninstall")]
    [string]$Action = "install",

    [ValidateSet("Debug","Release")]
    [string]$Configuration = "Debug",

    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot   = Split-Path -Parent $ScriptDir
$BinDir     = Join-Path $RepoRoot "bin\$Configuration\x64"

$HidPkgDir  = Join-Path $BinDir "SteamDeckHID"
$HidInfPath = Join-Path $HidPkgDir "SteamDeckHID.inf"
$BusPkgDir  = Join-Path $BinDir "SteamDeckBus"
$BusInfPath = Join-Path $BusPkgDir "SteamDeckBus.inf"

$BusHardwareId = "Root\SteamDeckBus"

$pnputil = "$env:SystemRoot\System32\pnputil.exe"
if (-not (Test-Path $pnputil)) { throw "pnputil.exe not found" }

function Write-Step([string]$m) { Write-Host "`n==> $m" -ForegroundColor Cyan }
function Write-Ok([string]$m)   { Write-Host "[OK]   $m" -ForegroundColor Green }
function Write-Warn([string]$m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }

# ---------------------------------------------------------------------------
# SetupAPI helper. Creates / removes a Root\SteamDeckBus device node so
# Windows binds SteamDeckBus.sys against the just-staged INF. Equivalent to
# "devcon install <inf> Root\SteamDeckBus".
# ---------------------------------------------------------------------------
$SdSetupApiCs = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class SdSetupApi
{
    [DllImport("setupapi.dll", SetLastError=true)]
    public static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid ClassGuid, IntPtr Parent);

    [DllImport("setupapi.dll", SetLastError=true)]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SetupDiClassNameFromGuid(ref Guid ClassGuid,
        StringBuilder ClassName, int ClassNameSize, IntPtr RequiredSize);

    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SetupDiCreateDeviceInfo(IntPtr DeviceInfoSet,
        string DeviceName, ref Guid ClassGuid, string DeviceDescription,
        IntPtr hwndParent, int CreationFlags, ref SP_DEVINFO_DATA DeviceInfoData);

    // Unicode variant. Without CharSet=Unicode, the loader picks the ANSI
    // function and parses our UTF-16 byte buffer as ANSI multi-sz, which
    // splits every wide char (whose high byte is 0x00) into its own string.
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode, EntryPoint="SetupDiSetDeviceRegistryPropertyW")]
    public static extern bool SetupDiSetDeviceRegistryProperty(IntPtr DeviceInfoSet,
        ref SP_DEVINFO_DATA DeviceInfoData, int Property, byte[] PropertyBuffer,
        int PropertyBufferSize);

    [DllImport("setupapi.dll", SetLastError=true)]
    public static extern bool SetupDiCallClassInstaller(int InstallFunction,
        IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("newdev.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool UpdateDriverForPlugAndPlayDevices(IntPtr hwndParent,
        string HardwareId, string FullInfPath, int InstallFlags,
        out bool RebootRequired);

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVINFO_DATA {
        public int cbSize;
        public Guid ClassGuid;
        public int DevInst;
        public IntPtr Reserved;
    }

    public const int SPDRP_HARDWAREID            = 0x00000001;
    public const int DICD_GENERATE_ID            = 0x00000001;
    public const int DIF_REGISTERDEVICE          = 0x00000019;
    public const int INSTALLFLAG_FORCE           = 0x00000001;

    // {4D36E97D-E325-11CE-BFC1-08002BE10318}  System
    public static Guid SystemClass = new Guid(
        0x4D36E97D, 0xE325, 0x11CE, 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18);
}
'@

if (-not ([System.Management.Automation.PSTypeName]'SdSetupApi').Type) {
    Add-Type -TypeDefinition $SdSetupApiCs -Language CSharp
}

function Add-DriverAndGetOemPath([string]$infPath) {
    # pnputil emits "Published Name: oemNNN.inf" (or its localised equivalent)
    # in /add-driver output. We don't trust the label - locale changes it -
    # but the oemNNN.inf token is locale-independent, so we just grep for it.
    $output = & $pnputil /add-driver $infPath /install 2>&1
    $output | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -notin @(0, 259, 3010)) {
        throw "pnputil /add-driver failed for $infPath (exit $LASTEXITCODE)"
    }

    $oem = $null
    foreach ($line in $output) {
        if ($line -match '(oem\d+\.inf)') { $oem = $Matches[1]; break }
    }
    if (-not $oem) {
        # Already-up-to-date case: pnputil doesn't print the oem name. Fall
        # back to scanning %SystemRoot%\INF for an INF whose body references
        # the original filename (locale-independent).
        $stem = [System.IO.Path]::GetFileNameWithoutExtension($infPath)
        $candidates = Get-ChildItem "$env:SystemRoot\INF\oem*.inf" -ErrorAction SilentlyContinue
        foreach ($c in $candidates) {
            $head = Get-Content $c.FullName -TotalCount 60 -ErrorAction SilentlyContinue
            if ($head -match $stem) { $oem = $c.Name; break }
        }
    }
    if (-not $oem) {
        throw "Could not determine published OEM INF name for $infPath"
    }
    return Join-Path "$env:SystemRoot\INF" $oem
}

function New-RootBusDevice([string]$hwid, [string]$oemFull) {
    # Two-phase create:
    #   1) SetupAPI registers a phantom Root\SteamDeckBus node (no driver).
    #   2) pnputil /scan-devices triggers PnP rematch; the staged OEM INF
    #      in the driver store has the matching hwid, so PnP binds it.
    # This sidesteps UpdateDriverForPlugAndPlayDevices, which on Windows 11
    # frequently fails with SPAPI_E_NOT_AN_INSTALLED_OEM_INF (0xE000020B)
    # even when the OEM INF is correctly staged.
    Write-Host "  (Driver will bind from store; staged at $oemFull)"

    $classGuid = [SdSetupApi]::SystemClass
    $className = New-Object System.Text.StringBuilder 32
    [void][SdSetupApi]::SetupDiClassNameFromGuid([ref]$classGuid, $className, 32, [IntPtr]::Zero)

    $set = [SdSetupApi]::SetupDiCreateDeviceInfoList([ref]$classGuid, [IntPtr]::Zero)
    if ($set -eq [IntPtr]::Zero -or $set -eq -1) {
        throw "SetupDiCreateDeviceInfoList failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }

    try {
        $devInfo = New-Object SdSetupApi+SP_DEVINFO_DATA
        $devInfo.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($devInfo)

        if (-not [SdSetupApi]::SetupDiCreateDeviceInfo($set, $className.ToString(),
            [ref]$classGuid, $null, [IntPtr]::Zero, [SdSetupApi]::DICD_GENERATE_ID, [ref]$devInfo))
        {
            throw "SetupDiCreateDeviceInfo failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }

        # Hardware ID: REG_MULTI_SZ, double-null terminated.
        $bytes = [System.Text.Encoding]::Unicode.GetBytes($hwid + "`0`0")
        if (-not [SdSetupApi]::SetupDiSetDeviceRegistryProperty($set, [ref]$devInfo,
            [SdSetupApi]::SPDRP_HARDWAREID, $bytes, $bytes.Length))
        {
            throw "SetupDiSetDeviceRegistryProperty failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }

        if (-not [SdSetupApi]::SetupDiCallClassInstaller([SdSetupApi]::DIF_REGISTERDEVICE, $set, [ref]$devInfo))
        {
            throw "SetupDiCallClassInstaller(DIF_REGISTERDEVICE) failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
    } finally {
        [void][SdSetupApi]::SetupDiDestroyDeviceInfoList($set)
    }

    # Trigger PnP rematch so the staged INF binds to the new device node.
    & $pnputil /scan-devices | Out-Null

    # Verify the device actually has the driver bound now.
    Start-Sleep -Milliseconds 1500
    $dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
        $_.HardwareID -contains $hwid
    } | Select-Object -First 1
    if (-not $dev) {
        throw "Device created but not visible after scan-devices."
    }
    if ($dev.Status -ne "OK") {
        Write-Warn "Root\SteamDeckBus is in state '$($dev.Status)'. Problem code: $($dev.ProblemDescription)"
        Write-Warn "If this is a fresh install, a reboot may be required to fully start the bus."
    }
    return $false   # we don't track reboot-required in this path
}

function Remove-RootBusDevice([string]$hwid) {
    $devs = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
        $_.InstanceId -like "ROOT\SYSTEM\*" -and $_.HardwareID -contains $hwid
    }
    foreach ($d in $devs) {
        Write-Host "  Removing: $($d.InstanceId)"
        & $pnputil /remove-device $d.InstanceId | Out-Null
    }
}

# Earlier versions of this script wrote the hwid through the ANSI variant of
# SetupDiSetDeviceRegistryProperty, producing phantom ROOT\SYSTEM nodes whose
# HardwareID got stored as one-letter strings ({R, o, o, t, ...}). Wipe any
# such residue before installing.
function Remove-LegacyZombieDevices {
    $zombies = Get-PnpDevice -InstanceId "ROOT\SYSTEM\*" -ErrorAction SilentlyContinue | Where-Object {
        $hw = $_.HardwareID
        $hw -and ($hw.Count -ge 4) -and ($hw[0]) -and ($hw[0].Length -eq 1)
    }
    foreach ($z in $zombies) {
        Write-Host "  Removing legacy zombie: $($z.InstanceId)"
        & $pnputil /remove-device $z.InstanceId | Out-Null
    }
}

# ---------------------------------------------------------------------------
# Existing helpers reused
# ---------------------------------------------------------------------------
function Invoke-ForceReenumerate {
    Write-Step "Force re-enumerating Neptune devices"
    $devices = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_28DE.*PID_1205' }
    if (-not $devices) { Write-Warn "No Neptune devices in PnP tree"; return }
    foreach ($dev in $devices) {
        Write-Host "  Removing: $($dev.InstanceId)"
        & $pnputil /remove-device $dev.InstanceId | Out-Null
    }
    & $pnputil /scan-devices | Out-Null
    Write-Ok "Re-enumeration complete"
}

function Get-InstalledOemInf([string]$stem) {
    # Locale-independent: scan %SystemRoot%\INF for oem*.inf whose body
    # references the original INF file name (e.g. "SteamDeckHID").
    $candidates = Get-ChildItem "$env:SystemRoot\INF\oem*.inf" -ErrorAction SilentlyContinue
    foreach ($c in $candidates) {
        $head = Get-Content $c.FullName -TotalCount 60 -ErrorAction SilentlyContinue
        if ($head -match $stem) { return $c.Name }
    }
    return $null
}

# ---------------------------------------------------------------------------
# Test-signing check
# ---------------------------------------------------------------------------
$testSigningOn = & bcdedit /enum "{current}" | Select-String "testsigning\s+Yes"
if (-not $testSigningOn) {
    Write-Warn "Test signing is NOT enabled. Run 'bcdedit /set testsigning on' and reboot first."
}

# ===========================================================================
# INSTALL
# ===========================================================================
if ($Action -eq "install" -or $Action -eq "update") {
    Write-Step "Adding driver packages ($Configuration)"

    if (-not (Test-Path $HidInfPath)) { throw "HID package missing: $HidInfPath" }
    if (-not (Test-Path $BusInfPath)) { throw "Bus package missing: $BusInfPath" }

    Write-Host "  pnputil /add-driver $HidInfPath /install"
    [void](Add-DriverAndGetOemPath $HidInfPath)

    Write-Host "  pnputil /add-driver $BusInfPath /install"
    $busOemFull = Add-DriverAndGetOemPath $BusInfPath
    Write-Ok "Both packages added to driver store"

    Write-Step "Cleaning up legacy zombie devices (if any)"
    Remove-LegacyZombieDevices

    Write-Step "Creating Root\SteamDeckBus device"
    $existing = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
        $_.HardwareID -contains $BusHardwareId
    }
    if ($existing -and $Action -eq "install") {
        Write-Warn "Root\SteamDeckBus already exists ($($existing.InstanceId)) - use -Action update to refresh"
    } else {
        if ($existing) { Remove-RootBusDevice $BusHardwareId }
        $rebootBus = New-RootBusDevice $BusHardwareId $busOemFull
        Write-Ok "Root\SteamDeckBus created"
        if ($rebootBus) { Write-Warn "Reboot required to start the bus driver" }
    }

    if ($Force) { Invoke-ForceReenumerate } else { & $pnputil /scan-devices | Out-Null }
    Write-Host "`n==> Install complete." -ForegroundColor Green
    Write-Host "    Replug the Steam Deck if it was already connected."
}

# ===========================================================================
# UNINSTALL
# ===========================================================================
elseif ($Action -eq "uninstall") {
    Write-Step "Removing Root\SteamDeckBus device"
    Remove-RootBusDevice $BusHardwareId
    Remove-LegacyZombieDevices

    Write-Step "Removing driver packages"
    foreach ($pat in @("SteamDeckHID", "SteamDeckBus")) {
        $oem = Get-InstalledOemInf $pat
        if ($oem) {
            Write-Host "  pnputil /delete-driver $oem"
            & $pnputil /delete-driver $oem /uninstall /force | Out-Null
        }
    }
    & $pnputil /scan-devices | Out-Null
    Write-Host "`n==> Uninstall complete." -ForegroundColor Green
}
