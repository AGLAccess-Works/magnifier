<#
.SYNOPSIS
    Removes what tools\install_uiaccess.ps1 installed.

.DESCRIPTION
    Step 1  Closes the installed Magnifier if it is running.
    Step 2  Deletes C:\Program Files\Magnifier and the "Magnifier (UIAccess)"
            Start menu shortcut.
    Step 3  Removes the "Magnifier Local UIAccess Signing" certificate from
            Local Machine > Personal, Trusted Root Certification Authorities
            and Trusted Publishers, so this PC no longer trusts it.

    Your settings and log in %APPDATA%\Magnifier and the build folders are
    left alone. Must run from an elevated (Run as administrator) PowerShell.

.PARAMETER Yes
    Skip the confirmation prompt.
#>
#Requires -Version 5.1
[CmdletBinding()]
param([switch]$Yes)

$ErrorActionPreference = 'Stop'

$CertSubject  = 'CN=Magnifier Local UIAccess Signing'
$InstallDir   = Join-Path $env:ProgramFiles 'Magnifier'
$ShortcutPath = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Magnifier (UIAccess).lnk'
$SettingsShortcutPath = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Magnifier settings.lnk'
$StartupPath = Join-Path ([Environment]::GetFolderPath('Startup')) 'Magnifier.lnk'  # "Start Magnifier when I sign in"

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'This script changes machine-wide certificate trust and Program Files, so it must run' -ForegroundColor Yellow
    Write-Host 'from an elevated PowerShell: right-click PowerShell, "Run as administrator", then run:' -ForegroundColor Yellow
    Write-Host "  powershell -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -ForegroundColor Yellow
    exit 1
}

Write-Host 'Magnifier UIAccess uninstall' -ForegroundColor Cyan
Write-Host @"
This will close the installed Magnifier, delete $InstallDir and its Start menu
shortcut, and remove the "$($CertSubject.Substring(3))" certificate from
this PC's certificate stores. Settings in %APPDATA%\Magnifier are kept.
"@
if (-not $Yes) {
    $answer = Read-Host 'Continue? Type Y and press Enter'
    if ($answer -notmatch '^(y|yes)$') { Write-Host 'Nothing changed.'; exit 1 }
}

Write-Host ''
Write-Host '== Step 1 of 3: close the installed Magnifier' -ForegroundColor Cyan
$installedExe = Join-Path $InstallDir 'Magnifier.exe'
$running = Get-Process -Name Magnifier -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $installedExe }
if ($running) {
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Write-Host 'Closed. Its watchdog restores your pointer.'
} else {
    Write-Host 'Not running.'
}

Write-Host ''
Write-Host '== Step 2 of 3: remove the installed files' -ForegroundColor Cyan
if (Test-Path $InstallDir) {
    Remove-Item -Recurse -Force $InstallDir
    Write-Host "Deleted $InstallDir"
} else {
    Write-Host "$InstallDir was not there."
}
foreach ($lnk in $ShortcutPath, $SettingsShortcutPath) {
    if (Test-Path $lnk) {
        Remove-Item -Force $lnk
        Write-Host "Deleted $lnk"
    }
}
# The sign-in shortcut, only if it starts the copy just removed.
if (Test-Path $StartupPath) {
    $target = (New-Object -ComObject WScript.Shell).CreateShortcut($StartupPath).TargetPath
    if ($target -eq $installedExe) {
        Remove-Item -Force $StartupPath
        Write-Host "Deleted $StartupPath"
    }
}

Write-Host ''
Write-Host '== Step 3 of 3: remove the certificate' -ForegroundColor Cyan
# Personal store: delete the private key along with the certificate.
$mine = @(Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $CertSubject })
foreach ($c in $mine) {
    Remove-Item -Path "Cert:\LocalMachine\My\$($c.Thumbprint)" -DeleteKey
    Write-Host "Removed $($c.Thumbprint) and its private key from LocalMachine\My."
}
if (-not $mine) { Write-Host 'None in LocalMachine\My.' }
foreach ($storeName in 'Root', 'TrustedPublisher') {
    $store = New-Object Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
    $store.Open('ReadWrite')
    try {
        $found = @($store.Certificates | Where-Object { $_.Subject -eq $CertSubject })
        foreach ($c in $found) {
            $store.Remove($c)
            Write-Host "Removed $($c.Thumbprint) from LocalMachine\$storeName."
        }
        if (-not $found) { Write-Host "None in LocalMachine\$storeName." }
    } finally {
        $store.Close()
    }
}

Write-Host ''
Write-Host 'Done. The uninstalled UIAccess build can no longer start; the normal build\Magnifier.exe still works.' -ForegroundColor Green
