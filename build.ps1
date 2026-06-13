<#
.SYNOPSIS
  Full build: compiles the driver, C++ core, then packages the Electron UI.

.DESCRIPTION
  1. Builds the VirtualAudioDriver (Kernel Mode)
  2. Locates MSBuild via vswhere and builds the C++ core
  3. Runs Vite then electron-builder for the UI -> output lands in build\

.REQUIREMENTS
  - Visual Studio 2022 with "Desktop development with C++" workload
  - Windows Driver Kit (WDK) for driver compilation
  - Node.js 18+ on PATH

.EXAMPLE
  .\build.ps1             # full build
  .\build.ps1 -Optimize   # full build with kernel-mode optimizations (/O2)
  .\build.ps1 -SkipDriver # build core and UI only
  .\build.ps1 -SkipCpp    # skip C++ compilation, re-package UI only
  .\build.ps1 -SkipUi     # compile driver and core only
#>

[CmdletBinding()]
param(
  [switch]$SkipCpp,
  [switch]$SkipUi,
  [switch]$SkipDriver,
  [switch]$Optimize
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root       = $PSScriptRoot
$SolnFile   = Join-Path $Root 'core\theaudioapp.sln'
$ReleaseDir = Join-Path $Root 'core\x64\Release'
$DriverDir  = Join-Path $Root 'Driver\VirtualAudioDriver'
$UiDir      = Join-Path $Root 'ui'
$BuildDir   = Join-Path $Root 'build'

function Step  ($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan   }
function Ok    ($msg) { Write-Host "    $msg"   -ForegroundColor Green  }
function Warn  ($msg) { Write-Host "    !! $msg" -ForegroundColor Yellow }
function Abort ($msg) { Write-Host "`n[FAIL] $msg" -ForegroundColor Red; exit 1 }

# -- 0. Driver build --------------------------------------------------
if (-not $SkipDriver) {
  Step "Building VirtualAudioDriver (Kernel Mode)"
  if (Test-Path (Join-Path $DriverDir 'build.ps1')) {
    Push-Location $DriverDir
    try {
      $driverArgs = if ($Optimize) { "-Optimize" } else { "" }
      Invoke-Expression ".\build.ps1 $driverArgs"
      if ($LASTEXITCODE -ne 0) { Abort "Driver build failed." }
      Ok "Driver build succeeded -> $DriverDir\out\VirtualAudioDriver.sys"
    } finally {
      Pop-Location
    }
  } else {
    Warn "Driver build script not found at $DriverDir\build.ps1"
  }
}

# -- 1. C++ build -----------------------------------------------------
if (-not $SkipCpp) {
  Step "Locating MSBuild"

  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) {
    Abort "vswhere.exe not found. Install Visual Studio 2022 or later."
  }

  $msbuild = (& $vswhere -latest -requires Microsoft.Component.MSBuild `
              -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null) | Select-Object -First 1

  if (-not $msbuild -or -not (Test-Path $msbuild)) {
    Abort "MSBuild not found. Install the 'Desktop development with C++' workload."
  }
  Ok $msbuild

  Step "Building C++ (Release | x64)"
  & $msbuild $SolnFile /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
  if ($LASTEXITCODE -ne 0) { Abort "MSBuild failed (exit $LASTEXITCODE)." }
  Ok "C++ build succeeded -> $ReleaseDir"
}

# -- 2. Build Electron UI ---------------------------------------------
if (-not $SkipUi) {
  Step "Installing Node dependencies"
  & npm --prefix $UiDir install --prefer-offline
  if ($LASTEXITCODE -ne 0) { Abort "npm install failed." }
  Ok "Dependencies up to date"

  Push-Location $UiDir
  try {
    Step "Building renderer (Vite)"
    & npm run build
    if ($LASTEXITCODE -ne 0) { Abort "Vite build failed." }
    Ok "Renderer built"

    Step "Stopping any running instance"
    @("TheAudio.app", "theaudioapp") | ForEach-Object {
      $procs = Get-Process -Name $_ -ErrorAction SilentlyContinue
      if ($procs) {
        $procs | Stop-Process -Force -ErrorAction SilentlyContinue
        Ok "Stopped: $_"
        Start-Sleep -Seconds 1
      }
    }
    Ok "Done"

    Step "Cleaning previous build output"
    if (Test-Path $BuildDir) {
      try {
        [System.IO.Directory]::Delete($BuildDir, $true)
      } catch {
        Start-Sleep -Seconds 2
        try {
          [System.IO.Directory]::Delete($BuildDir, $true)
        } catch {
          Abort "Could not clean $BuildDir - files still locked. Quit the app from the tray and retry."
        }
      }
      if (Test-Path $BuildDir) {
        Abort "Could not clean $BuildDir - files still locked. Quit the app from the tray and retry."
      }
    }
    Ok "Clean"

    Step "Packaging Electron app (electron-builder)"
    & npm run package
    if ($LASTEXITCODE -ne 0) { Abort "electron-builder failed." }
    Ok "Packaging complete"
  } finally {
    Pop-Location
  }
}

# -- 3. Summary -------------------------------------------------------
Step "Build complete"
Write-Host ""
Write-Host "  Output : $BuildDir" -ForegroundColor Cyan

$installer = Get-ChildItem -Path $BuildDir -Filter 'TheAudio.app Setup *.exe' -ErrorAction SilentlyContinue |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1 -ExpandProperty FullName

if ($installer -and (Test-Path $installer)) { Write-Host "  Installer : $installer" -ForegroundColor Green }
Write-Host ""
