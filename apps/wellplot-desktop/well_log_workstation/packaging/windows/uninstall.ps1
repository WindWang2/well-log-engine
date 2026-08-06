# Uninstall WellPlot Desktop user install created by install.ps1.
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\uninstall.ps1
#   powershell -ExecutionPolicy Bypass -File .\uninstall.ps1 -PurgeConfig
#   powershell -ExecutionPolicy Bypass -File .\uninstall.ps1 -DryRun
param(
    [string]$Prefix = "",
    [switch]$PurgeConfig,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

if (-not $Prefix) {
    $cand = Join-Path $env:LOCALAPPDATA "WellPlotDesktop"
    if (Test-Path (Join-Path $cand ".wellplot-install-meta.txt")) {
        $Prefix = $cand
    } elseif (Test-Path (Join-Path $PSScriptRoot ".wellplot-install-meta.txt")) {
        $Prefix = $PSScriptRoot
    } else {
        $Prefix = $cand
    }
}

$MetaPath = Join-Path $Prefix ".wellplot-install-meta.txt"
$ShortcutPath = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\WellPlot Desktop.lnk"

if (Test-Path $MetaPath) {
    Get-Content $MetaPath | ForEach-Object {
        if ($_ -match '^shortcut=(.+)$') { $ShortcutPath = $Matches[1].Trim() }
        if ($_ -match '^prefix=(.+)$') { $Prefix = $Matches[1].Trim() }
    }
}

function Invoke-Step([string]$Message, [scriptblock]$Action) {
    if ($DryRun) {
        Write-Host "DRY: $Message"
    } else {
        Write-Host "==> $Message"
        & $Action
    }
}

Write-Host "Uninstall WellPlot Desktop"
Write-Host "  prefix:   $Prefix"
Write-Host "  shortcut: $ShortcutPath"

Invoke-Step "Remove Start Menu shortcut" {
    if (Test-Path $ShortcutPath) { Remove-Item -Force $ShortcutPath }
}

Invoke-Step "Remove install prefix" {
    if (Test-Path $Prefix) { Remove-Item -Recurse -Force $Prefix }
}

if ($PurgeConfig) {
    # Qt QSettings under org paleo-workbench (registry on Windows).
    $RegPath = "HKCU:\Software\paleo-workbench"
    Invoke-Step "Purge QSettings $RegPath" {
        if (Test-Path $RegPath) { Remove-Item -Recurse -Force $RegPath }
    }
}

Write-Host "Uninstalled."
Write-Host "  Residual (unless -PurgeConfig): HKCU\Software\paleo-workbench and user workspaces."
