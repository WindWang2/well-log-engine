# Install WellPlot Desktop onedir for the current user (Windows).
# Usage (from the dist\WellPlotDesktop folder after build.ps1):
#   powershell -ExecutionPolicy Bypass -File .\install.ps1
# Options:
#   -Prefix <path>   default: $env:LOCALAPPDATA\WellPlotDesktop
#   -DryRun
param(
    [string]$Prefix = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$BundleDir = $PSScriptRoot
$ExeName = "WellPlotDesktop.exe"
$SrcExe = Join-Path $BundleDir $ExeName
if (-not (Test-Path $SrcExe)) {
    Write-Error "Bundle binary not found: $SrcExe. Run packaging\build.ps1 first."
}

if (-not $Prefix) {
    $Prefix = Join-Path $env:LOCALAPPDATA "WellPlotDesktop"
}

$StartMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
$ShortcutPath = Join-Path $StartMenu "WellPlot Desktop.lnk"
$MetaPath = Join-Path $Prefix ".wellplot-install-meta.txt"

function Invoke-Step([string]$Message, [scriptblock]$Action) {
    if ($DryRun) {
        Write-Host "DRY: $Message"
    } else {
        Write-Host "==> $Message"
        & $Action
    }
}

Write-Host "Install WellPlot Desktop"
Write-Host "  bundle: $BundleDir"
Write-Host "  prefix: $Prefix"
Write-Host "  start:  $ShortcutPath"

Invoke-Step "Copy onedir → $Prefix" {
    New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
    # Mirror bundle into prefix
    robocopy $BundleDir $Prefix /MIR /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed with code $LASTEXITCODE" }
}

Invoke-Step "Start Menu shortcut" {
    New-Item -ItemType Directory -Force -Path $StartMenu | Out-Null
    $Wsh = New-Object -ComObject WScript.Shell
    $Sc = $Wsh.CreateShortcut($ShortcutPath)
    $Sc.TargetPath = Join-Path $Prefix $ExeName
    $Sc.WorkingDirectory = $Prefix
    $Sc.Description = "WellPlot Desktop"
    $Sc.Save()
}

Invoke-Step "Write install meta" {
    @"
prefix=$Prefix
shortcut=$ShortcutPath
exe=$(Join-Path $Prefix $ExeName)
installed_at=$(Get-Date -Format o)
"@ | Set-Content -Path $MetaPath -Encoding UTF8
}

Write-Host "Installed."
Write-Host "  Start: Start Menu → WellPlot Desktop"
Write-Host "  Or:    $(Join-Path $Prefix $ExeName)"
Write-Host "  Uninstall: powershell -ExecutionPolicy Bypass -File $(Join-Path $Prefix 'uninstall.ps1')"
