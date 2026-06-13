# deploy_driver.ps1 — build and deploy to the test VM
#
# Usage:
#   .\deploy_driver.ps1                        # build driver + deploy artifacts only
#   .\deploy_driver.ps1 -Install               # build driver + deploy + install driver on VM
#   .\deploy_driver.ps1 -App                   # deploy C++ exe only (fast iteration on backend)
#   .\deploy_driver.ps1 -Installer             # deploy full Electron installer + run it on VM
#   .\deploy_driver.ps1 -Install -Installer    # full: driver install + full app install
#   .\deploy_driver.ps1 -SkipBuild             # skip local builds (re-deploy last artifacts)

param(
    [switch]$Install,
    [switch]$App,
    [switch]$Installer,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$VM          = "dev@10.0.4.55"
$VMDriver    = "C:/Users/dev/Documents/Project/VirtualAudioDriver"
$VMApp       = "C:/Users/dev/Documents/Project/TheAudioApp"
$LocalDriver = "$PSScriptRoot\Driver\VirtualAudioDriver"
$LocalApp    = "$PSScriptRoot\core\x64\Release"
$LocalBuild  = "$PSScriptRoot\build"

# ---------------------------------------------------------------------------
# Step 1: Build driver locally
# ---------------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host "=== Building driver locally ===" -ForegroundColor Cyan
    & "$LocalDriver\build.ps1"
    if ($LASTEXITCODE -ne 0) { throw "Driver build failed" }
} else {
    Write-Host "=== Skipping driver build ===" -ForegroundColor Yellow
}

$SysBin = "$LocalDriver\out\VirtualAudioDriver.sys"
$InfSrc = "$LocalDriver\VirtualAudioDriver.inf"
if (-not (Test-Path $SysBin)) { throw "No .sys at $SysBin - run without -SkipBuild first" }
if (-not (Test-Path $InfSrc)) { throw "No .inf at $InfSrc" }

# ---------------------------------------------------------------------------
# Step 2: Deploy driver artifacts to VM
# ---------------------------------------------------------------------------
Write-Host "=== Deploying driver artifacts ===" -ForegroundColor Cyan

ssh $VM "powershell -Command `"`$null = New-Item -ItemType Directory -Force '$VMDriver/out'`""
scp $SysBin                    "${VM}:${VMDriver}/out/VirtualAudioDriver.sys"
scp $InfSrc                    "${VM}:${VMDriver}/VirtualAudioDriver.inf"
scp "$LocalDriver\install.ps1" "${VM}:${VMDriver}/install.ps1"
Write-Host "  .sys + .inf + install.ps1 deployed" -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# Step 3: Deploy C++ exe only (fast backend iteration)
# ---------------------------------------------------------------------------
if ($App) {
    Write-Host "=== Deploying app exe ===" -ForegroundColor Cyan

    $ExeSrc = "$LocalApp\theaudioapp.exe"
    if (-not (Test-Path $ExeSrc)) { throw "No exe at $ExeSrc - run build.ps1 -SkipUi first" }

    ssh $VM "powershell -Command `"`$null = New-Item -ItemType Directory -Force '$VMApp'`""
    scp $ExeSrc "${VM}:${VMApp}/theaudioapp.exe"

    $settingsExists = ssh $VM "powershell -Command `"(Test-Path '$VMApp/settings.json').ToString()`""
    if ($settingsExists -notmatch 'True') {
        $default = '{"routing":{},"channelOutputs":{"Games":"","Media":"","Voice":""},"defaultDevice":""}'
        ssh $VM "powershell -Command `"Set-Content -Path '$VMApp/settings.json' -Value '$default' -Encoding utf8`""
        Write-Host "  settings.json created (default)" -ForegroundColor DarkGray
    }

    Write-Host "  theaudioapp.exe deployed -> $VMApp" -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# Step 4: Deploy full Electron installer
# ---------------------------------------------------------------------------
if ($Installer) {
    Write-Host "=== Deploying full app installer ===" -ForegroundColor Cyan

    # Find the installer — pick the most recently built one
    $setupExe = Get-ChildItem -Path $LocalBuild -Filter "TheAudio.app Setup *.exe" -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1 -ExpandProperty FullName

    if (-not $setupExe) { throw "No installer found in $LocalBuild - run build.ps1 first" }

    $sizeMB = [math]::Round((Get-Item $setupExe).Length / 1MB, 1)
    Write-Host "  Copying installer ($sizeMB MB)..." -ForegroundColor DarkGray
    scp $setupExe "${VM}:C:/Users/dev/Desktop/TheAudioAppSetup.exe"

    Write-Host "  Running installer silently on VM..." -ForegroundColor DarkGray
    # /S = silent NSIS install; /ALLUSERS = per-machine (matches perMachine:true in config)
    $runSetup = "powershell -Command `"Start-Process 'C:\Users\dev\Desktop\TheAudioAppSetup.exe' -ArgumentList '/S','/ALLUSERS' -Wait -Verb RunAs`""
    ssh $VM $runSetup
    if ($LASTEXITCODE -ne 0) { throw "Installer failed (exit $LASTEXITCODE)" }

    Write-Host "  App installed on VM" -ForegroundColor DarkGray
}

if (-not $Install) {
    Write-Host "=== Deploy complete (use -Install to install driver on VM) ===" -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# Step 5: Install driver on VM (signing, pnputil, devcon)
# ---------------------------------------------------------------------------
Write-Host "=== Installing driver on VM ===" -ForegroundColor Cyan

$installCmd = "powershell -ExecutionPolicy Bypass -Command `$null; Set-Location '$VMDriver'; .\install.ps1"
ssh $VM $installCmd
if ($LASTEXITCODE -ne 0) { throw "Driver install failed" }

Write-Host "=== Done ===" -ForegroundColor Green
