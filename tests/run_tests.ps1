# run_tests.ps1 - compile and run all tests
# Requires VS 2022 (uses vswhere to locate cl.exe)

Set-StrictMode -Version 1
$ErrorActionPreference = "Stop"

$root  = Split-Path $PSScriptRoot -Parent
$tests = $PSScriptRoot
$out   = Join-Path $tests "bin"

# --- Locate MSVC ---

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found - is Visual Studio 2022 installed?"
    exit 1
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath 2>$null
if (-not $vsPath) {
    Write-Error "No Visual Studio installation with C++ tools found."
    exit 1
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    Write-Error "vcvars64.bat not found at: $vcvars"
    exit 1
}

# Import MSVC environment via a temp batch file (avoids PS5.1 parsing && in cmd /c strings)
$batFile = Join-Path $env:TEMP "vcvars_env.bat"
$batContent = "@call `"$vcvars`" >nul 2>&1" + "`r`n" + "@set"
[System.IO.File]::WriteAllText($batFile, $batContent, [System.Text.Encoding]::ASCII)
$envLines = cmd /c $batFile
Remove-Item $batFile -ErrorAction SilentlyContinue
foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Error "cl.exe still not found after sourcing vcvars64.bat"
    exit 1
}

# --- Prepare output dir ---

New-Item -ItemType Directory -Force -Path $out | Out-Null

# --- Test definitions ---

$testCases = @(
    @{ src = "test_routing.cpp"; exe = "test_routing.exe" },
    @{ src = "test_utils.cpp";   exe = "test_utils.exe"   }
)

# --- Compile and run ---

$totalPassed  = 0
$totalFailed  = 0
$anyBuildFail = $false

foreach ($tc in $testCases) {
    $srcPath = Join-Path $tests $tc.src
    $exePath = Join-Path $out  $tc.exe
    $objPath = Join-Path $out  ($tc.exe -replace '\.exe$', '.obj')

    Write-Host ""
    Write-Host "--- $($tc.src) ---" -ForegroundColor Cyan

    $clArgs = @(
        "/nologo", "/EHsc", "/W3", "/std:c++17",
        "/I`"$root\Source\theaudioapp`"",
        "`"$srcPath`"",
        "/Fe`"$exePath`"",
        "/Fo`"$objPath`"",
        "/link", "ole32.lib"
    )

    $proc = Start-Process -FilePath "cl.exe" `
        -ArgumentList $clArgs `
        -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $out "cl_out.txt") `
        -RedirectStandardError  (Join-Path $out "cl_err.txt")

    $clOut = Get-Content (Join-Path $out "cl_out.txt") -Raw -ErrorAction SilentlyContinue
    $clErr = Get-Content (Join-Path $out "cl_err.txt") -Raw -ErrorAction SilentlyContinue
    if ($clOut) { Write-Host $clOut }
    if ($clErr) { Write-Host $clErr }

    if ($proc.ExitCode -ne 0) {
        Write-Host "BUILD FAILED (exit $($proc.ExitCode))" -ForegroundColor Red
        $anyBuildFail = $true
        continue
    }

    $run = Start-Process -FilePath $exePath `
        -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $out "run_out.txt") `
        -RedirectStandardError  (Join-Path $out "run_err.txt")

    $runOut = Get-Content (Join-Path $out "run_out.txt") -Raw -ErrorAction SilentlyContinue
    if ($runOut) { Write-Host $runOut }

    if ($runOut -match '(\d+) passed, (\d+) failed') {
        $totalPassed += [int]$Matches[1]
        $totalFailed += [int]$Matches[2]
    } elseif ($run.ExitCode -ne 0) {
        $totalFailed++
    }
}

# --- Summary ---

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
if ($anyBuildFail) {
    Write-Host "One or more test files failed to compile." -ForegroundColor Red
}
$allOk = ($totalFailed -eq 0 -and -not $anyBuildFail)
$color = if ($allOk) { "Green" } else { "Red" }
Write-Host "Total: $totalPassed passed, $totalFailed failed" -ForegroundColor $color
Write-Host "========================================" -ForegroundColor Cyan

$buildPenalty = if ($anyBuildFail) { 1 } else { 0 }
exit ($totalFailed + $buildPenalty)
