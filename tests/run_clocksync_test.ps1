#Requires -RunAsAdministrator
# Compiles and runs clocksync_test.exe

$ErrorActionPreference = "Stop"
$TestDir     = $PSScriptRoot
$ProjectRoot = Split-Path $TestDir -Parent

# ── Find vcvars64.bat ─────────────────────────────────────────────────────────
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found. Install Visual Studio 2022." }

$vsRoot  = & $vswhere -latest -property installationPath
$vcvars  = "$vsRoot\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# ── Compile via cmd so vcvars sets up the full env ───────────────────────────
$src = Join-Path $TestDir    "clocksync_test.cpp"
$out = Join-Path $ProjectRoot "core\x64\Release\clocksync_test.exe"

$null = New-Item -ItemType Directory -Force (Split-Path $out)

Write-Host "Compiling clocksync_test.cpp..." -ForegroundColor Cyan

$outDir = Split-Path $out -Parent
$compileCmd = "`"$vcvars`" > nul 2>&1 && cl.exe /nologo /EHsc /O2 /W3 `"$src`" /Fo:`"$outDir\`" /Fe:`"$out`" /link ole32.lib uuid.lib"
cmd /c $compileCmd
if ($LASTEXITCODE -ne 0) { throw "Compilation failed." }
Write-Host "Compiled -> $out" -ForegroundColor Green

# ── Run ───────────────────────────────────────────────────────────────────────
Write-Host "`nRunning clocksync_test...`n" -ForegroundColor Cyan

& $out
