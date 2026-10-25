<#
.SYNOPSIS
    Signs and installs Magnifier with UIAccess, so the magnifier can cover the
    Start menu, taskbar flyouts and notification centre.

.DESCRIPTION
    Windows only lets a window into the z-order band above the Start menu if
    its program has UIAccess. Windows grants UIAccess only when all three hold:
      1. the exe's manifest asks for it (uiAccess="true");
      2. the exe is signed by a certificate this PC trusts;
      3. the exe is installed in a protected folder such as Program Files.

    Build first, from an ordinary (not elevated) command prompt:
        build.cmd UIAccess
    Then run this script from an elevated PowerShell. It:
      Step 1  Checks build-uiaccess\Magnifier.exe asks for UIAccess.
      Step 2  Copies it to C:\Program Files\Magnifier\, which only
              administrators can change, and adds Start menu shortcuts.
      Step 3  Creates a new self-signed code-signing certificate, trusts it on
              this PC (Local Machine > Trusted Root Certification Authorities
              and Trusted Publishers), signs the installed copy with it, then
              deletes the certificate's private key. Certificates from earlier
              installs are removed first.

    Because the private key is deleted straight after signing, nothing on this
    PC can sign anything else with the trusted certificate. Each install makes
    a new one. tools\uninstall_uiaccess.ps1 undoes all of it.

.PARAMETER Yes
    Skip the confirmation prompt.

.EXAMPLE
    Elevated PowerShell, from the repository folder:
        powershell -ExecutionPolicy Bypass -File tools\install_uiaccess.ps1
#>
#Requires -Version 5.1
[CmdletBinding()]
param([switch]$Yes)

$ErrorActionPreference = 'Stop'

$CertSubject  = 'CN=Magnifier Local UIAccess Signing'
$InstallDir   = Join-Path $env:ProgramFiles 'Magnifier'
$InstalledExe = Join-Path $InstallDir 'Magnifier.exe'
$ShortcutPath = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Magnifier (UIAccess).lnk'
$SettingsShortcutPath = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Magnifier settings.lnk'
$RepoRoot     = Split-Path -Parent $PSScriptRoot
$BuiltExe     = Join-Path $RepoRoot 'build-uiaccess\Magnifier.exe'

function Step([string]$title, [string]$explanation) {
    Write-Host ''
    Write-Host "== $title" -ForegroundColor Cyan
    Write-Host $explanation
}

# --- Elevation ---------------------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'This script changes machine-wide certificate trust and writes to Program Files,' -ForegroundColor Yellow
    Write-Host 'so it must run from an elevated PowerShell: right-click PowerShell, "Run as administrator",' -ForegroundColor Yellow
    Write-Host "then run:  powershell -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path $BuiltExe)) {
    Write-Host "Not found: $BuiltExe" -ForegroundColor Yellow
    Write-Host 'Build it first from an ordinary (not elevated) command prompt:  build.cmd UIAccess' -ForegroundColor Yellow
    exit 1
}

Write-Host 'Magnifier UIAccess install' -ForegroundColor Cyan
Write-Host @"
This will:
  1. check $BuiltExe asks for UIAccess;
  2. copy it to $InstallDir and add Start menu shortcuts;
  3. create a self-signed code-signing certificate named
     "$($CertSubject.Substring(3))", trust it on this PC (Trusted Root
     Certification Authorities and Trusted Publishers), sign the installed
     copy, and delete the certificate's private key.

Run tools\uninstall_uiaccess.ps1 to remove the certificate and the installed copy.
"@
if (-not $Yes) {
    $answer = Read-Host 'Continue? Type Y and press Enter'
    if ($answer -notmatch '^(y|yes)$') { Write-Host 'Nothing changed.'; exit 1 }
}

# --- Step 1: check the build -------------------------------------------------
Step 'Step 1 of 3: check the build' @"
The build made by "build.cmd UIAccess" asks Windows for UIAccess in its
manifest, so it only starts once it is signed and installed.
"@
# The manifest is embedded as plain text, so a byte search is enough to check it.
$exeText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($BuiltExe))
if ($exeText -notmatch 'uiAccess="true"') {
    throw "$BuiltExe does not ask for UIAccess. Delete build-uiaccess\ and run build.cmd UIAccess again."
}
Write-Host "OK: $BuiltExe asks for UIAccess."

# --- Step 2: install ---------------------------------------------------------
Step "Step 2 of 3: install to $InstallDir" @"
Closes any running Magnifier (its watchdog restores your pointer), copies the
exe into Program Files and adds Start menu shortcuts.
"@
$running = Get-Process -Name Magnifier -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "Closing $(@($running).Count) running Magnifier process(es)."
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item -Force $BuiltExe $InstalledExe

$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($ShortcutPath)
$link.TargetPath = $InstalledExe
$link.WorkingDirectory = $InstallDir
$link.Description = 'Full-screen magnifier'
$link.Save()
$settingsLink = $shell.CreateShortcut($SettingsShortcutPath)
$settingsLink.TargetPath = $InstalledExe
$settingsLink.Arguments = '--settings'
$settingsLink.WorkingDirectory = $InstallDir
$settingsLink.Description = 'Magnifier settings (starts Magnifier if it is not running)'
$settingsLink.Save()
Write-Host "Installed $InstalledExe"
Write-Host "Shortcut  $ShortcutPath"
Write-Host "Shortcut  $SettingsShortcutPath"

# --- Step 3: certificate and signature --------------------------------------
Step 'Step 3 of 3: sign the installed copy' @"
Removes certificates left by earlier installs, creates a new one, trusts it,
signs $InstalledExe with signtool from the Windows SDK (SHA-256, no
timestamp server, so nothing is sent over the network), checks the signature
and deletes the private key.
"@
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Directory.Parent.Name } -Descending | Select-Object -First 1
if (-not $signtool) { throw 'signtool.exe not found. Install the Windows SDK (part of the VS 2022 C++ Build Tools).' }

function Remove-OldCertificates {
    foreach ($c in @(Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $CertSubject })) {
        Remove-Item -Path "Cert:\LocalMachine\My\$($c.Thumbprint)" -DeleteKey
    }
    foreach ($storeName in 'Root', 'TrustedPublisher') {
        $store = New-Object Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
        $store.Open('ReadWrite')
        try {
            foreach ($c in @($store.Certificates | Where-Object { $_.Subject -eq $CertSubject })) { $store.Remove($c) }
        } finally {
            $store.Close()
        }
    }
}
Remove-OldCertificates

$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject `
    -CertStoreLocation Cert:\LocalMachine\My -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
    -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(10)
try {
    $publicCert = New-Object Security.Cryptography.X509Certificates.X509Certificate2(, $cert.RawData)
    foreach ($storeName in 'Root', 'TrustedPublisher') {
        $store = New-Object Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
        $store.Open('ReadWrite')
        try { $store.Add($publicCert) } finally { $store.Close() }
    }
    & $signtool.FullName sign /sm /s My /sha1 $cert.Thumbprint /fd SHA256 $InstalledExe
    if ($LASTEXITCODE -ne 0) { throw "signtool sign failed (exit $LASTEXITCODE)." }
} finally {
    # Whatever happened, no usable signing key is left behind.
    Remove-Item -Path "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey
}
& $signtool.FullName verify /pa /q $InstalledExe
if ($LASTEXITCODE -ne 0) { throw "signtool verify failed (exit $LASTEXITCODE): the signature is not trusted." }
Write-Host "OK: signed with certificate $($cert.Thumbprint); its private key is deleted."

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host @"
Start it from the Start menu ("Magnifier (UIAccess)") or by double-clicking
$InstalledExe. Start it as yourself, not from this elevated window.
The log (%APPDATA%\Magnifier\log.txt) says "Running with UIAccess" when it worked.
Only one Magnifier runs at a time, so close any development copy first.
After rebuilding, run this script again to re-sign and re-install.
"@
