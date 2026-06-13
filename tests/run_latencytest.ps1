#Requires -RunAsAdministrator
# Compiles and runs latencytest.exe
# Usage:
#   .\run_latencytest.ps1                                          # auto-pick
#   .\run_latencytest.ps1 "TheAudio.app Games"                    # specific virtual
#   .\run_latencytest.ps1 "TheAudio.app Games" "VG259QM"          # specific pair

param(
    [string]$VirtualChannel = "",
    [string]$PhysicalDevice = ""
)

$ErrorActionPreference = "Stop"
$TestDir   = $PSScriptRoot
$ProjectRoot = Split-Path $TestDir -Parent

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found. Install Visual Studio 2022." }

$vsRoot = & $vswhere -latest -property installationPath
$vcvars = "$vsRoot\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$src = Join-Path $TestDir    "latencytest.cpp"
$out = Join-Path $ProjectRoot "core\x64\Release\latencytest.exe"

$null = New-Item -ItemType Directory -Force (Split-Path $out)

Write-Host "Compiling latencytest.cpp..." -ForegroundColor Cyan
$outDir = Split-Path $out -Parent
$compileCmd = "`"$vcvars`" > nul 2>&1 && cl.exe /nologo /EHsc /O2 /W3 `"$src`" /Fo:`"$outDir\`" /Fe:`"$out`" /link ole32.lib uuid.lib"
cmd /c $compileCmd
if ($LASTEXITCODE -ne 0) { throw "Compilation failed." }
Write-Host "Compiled -> $out" -ForegroundColor Green

Write-Host "`nRunning latencytest...`n" -ForegroundColor Cyan

$runArgs = @()
if ($VirtualChannel) { $runArgs += $VirtualChannel }
if ($PhysicalDevice) { $runArgs += $PhysicalDevice }

& $out @runArgs
