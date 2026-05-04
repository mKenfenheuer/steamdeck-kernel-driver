#Requires -RunAsAdministrator
#Requires -Version 5.1
<#
.SYNOPSIS
    Creates a self-signed test certificate for driver signing.

.DESCRIPTION
    Generates a code-signing certificate, installs it into the
    TestCertStore certificate store, and exports it as a .cer file.
    Must be run as Administrator.

    Also enables test-signing mode via bcdedit if not already enabled.

.NOTES
    Only needed for development/testing.
    For production, obtain an EV code-signing certificate and submit to WHQL.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$CertSubject  = "CN=SteamDeckTest, O=SteamDeckHID, C=DE"
$CertStore    = "My"
$CertFile     = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "..\tools\SteamDeckTest.cer"
$CertFile     = [System.IO.Path]::GetFullPath($CertFile)

function Write-Step([string]$msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Write-Ok([string]$msg)   { Write-Host "[OK] $msg" -ForegroundColor Green }

# ---------------------------------------------------------------------------
# 1. Enable Test Signing
# ---------------------------------------------------------------------------
Write-Step "Checking test-signing state"
$bcdOutput = & bcdedit /enum "{current}" 2>&1
if ($bcdOutput -match "testsigning\s+Yes") {
    Write-Ok "Test-signing already enabled"
} else {
    Write-Host "Enabling test-signing mode (reboot required after this script)..."
    & bcdedit /set testsigning on
    if ($LASTEXITCODE -ne 0) { throw "bcdedit failed" }
    Write-Ok "Test-signing enabled - REBOOT REQUIRED before installing the driver"
}

# ---------------------------------------------------------------------------
# 2. Create certificate
# ---------------------------------------------------------------------------
Write-Step "Creating self-signed code-signing certificate"

# Remove existing cert with same subject to avoid duplicates
$existing = Get-ChildItem -Path "Cert:\CurrentUser\$CertStore" -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -eq $CertSubject }
if ($existing) {
    $existing | Remove-Item -Force
    Write-Host "Removed existing certificate"
}

# Create new cert
$cert = New-SelfSignedCertificate `
    -Subject          $CertSubject `
    -CertStoreLocation "Cert:\CurrentUser\$CertStore" `
    -KeyUsage         DigitalSignature `
    -KeyUsageProperty Sign `
    -Type             CodeSigningCert `
    -KeyAlgorithm     RSA `
    -KeyLength        2048 `
    -HashAlgorithm    SHA256 `
    -NotAfter         (Get-Date).AddYears(10)

Write-Ok "Certificate created: Thumbprint=$($cert.Thumbprint)"

# ---------------------------------------------------------------------------
# 3. Export .cer for distribution / VM import
# ---------------------------------------------------------------------------
Write-Step "Exporting certificate to $CertFile"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $CertFile) | Out-Null
Export-Certificate -Cert $cert -FilePath $CertFile -Type CERT | Out-Null
Write-Ok "Exported: $CertFile"

# ---------------------------------------------------------------------------
# 4. Also install into Trusted Publishers so Device Manager accepts it
# ---------------------------------------------------------------------------
Write-Step "Installing into Trusted Publishers store"
$tp = [System.Security.Cryptography.X509Certificates.X509Store]::new(
    "TrustedPublisher",
    [System.Security.Cryptography.X509Certificates.StoreLocation]::LocalMachine)
$tp.Open("ReadWrite")
$tp.Add($cert)
$tp.Close()
Write-Ok "Installed into TrustedPublisher (LocalMachine)"

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
$summary = @"

==> Test certificate setup complete!

Certificate store : $CertStore
Subject           : $CertSubject
Thumbprint        : $($cert.Thumbprint)
Exported to       : $CertFile

Next steps:
  1. Reboot if test-signing was just enabled
  2. Run: .\scripts\build.ps1 -Sign
  3. Run: .\scripts\install.ps1

To use the .cer on a test VM:
  Copy SteamDeckTest.cer to the VM and run:
  certutil -addstore TrustedPublisher SteamDeckTest.cer
  certutil -addstore Root SteamDeckTest.cer
"@
Write-Host $summary -ForegroundColor Yellow
