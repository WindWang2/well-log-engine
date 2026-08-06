# Build WellPlot Desktop onedir bundle with PyInstaller (Windows).
# Usage (from monorepo root, in PowerShell):
#   powershell -ExecutionPolicy Bypass -File well_log_workstation/packaging/build.ps1
$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
Set-Location $Root

$Spec = "well_log_workstation\packaging\wellplot-desktop.spec"
$OutDir = if ($env:WELLPLOT_DIST) { $env:WELLPLOT_DIST } else { Join-Path $Root "dist" }
$WorkDir = if ($env:WELLPLOT_BUILD) { $env:WELLPLOT_BUILD } else { Join-Path $Root "build\pyinstaller" }

if (-not (Get-Command pyinstaller -ErrorAction SilentlyContinue)) {
    Write-Error "pyinstaller not found. Run: python -m pip install -r well_log_workstation/packaging/requirements-packaging.txt"
}

$env:WLWS_DISABLE_ENGINE = if ($env:WLWS_DISABLE_ENGINE) { $env:WLWS_DISABLE_ENGINE } else { "1" }

Write-Host "==> PyInstaller → $OutDir\WellPlotDesktop"
New-Item -ItemType Directory -Force -Path $OutDir, $WorkDir | Out-Null
pyinstaller --noconfirm --clean --distpath $OutDir --workpath $WorkDir $Spec

$App = Join-Path $OutDir "WellPlotDesktop"
$Exe = Join-Path $App "WellPlotDesktop.exe"
if (-not (Test-Path $Exe)) {
    Write-Error "Build failed: missing $Exe"
}

Copy-Item -Force well_log_workstation\packaging\windows\install.ps1 $App\
Copy-Item -Force well_log_workstation\packaging\windows\uninstall.ps1 $App\
if (Test-Path well_log_workstation\packaging\windows\wellplot-desktop.iss) {
    Copy-Item -Force well_log_workstation\packaging\windows\wellplot-desktop.iss $App\
}

Write-Host "==> Bundle ready: $App"
Write-Host "    Install (user):  powershell -ExecutionPolicy Bypass -File $App\install.ps1"
Write-Host "    Uninstall:       powershell -ExecutionPolicy Bypass -File $App\uninstall.ps1"
Get-ChildItem $App | Select-Object -First 20
