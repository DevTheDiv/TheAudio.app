# Build script for VirtualAudioDriver kernel-mode WDM driver
# Auto-detects VS and WDK installations; no hardcoded version numbers.
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Locate Visual Studio via vswhere
# ---------------------------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 with 'Desktop development with C++'."
}
$VsRoot = (& $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath 2>$null)
if (-not $VsRoot) {
    throw "No VS install with VC tools found. Install 'Desktop development with C++'."
}

$MsvcToolsDir = "$VsRoot\VC\Tools\MSVC"
$MsvcVer = (Get-ChildItem $MsvcToolsDir -Directory |
    Sort-Object Name -Descending |
    Select-Object -First 1).Name
if (-not $MsvcVer) { throw "No MSVC toolset found under $MsvcToolsDir" }

$Cl   = "$MsvcToolsDir\$MsvcVer\bin\HostX64\x64\cl.exe"
$Link = "$MsvcToolsDir\$MsvcVer\bin\HostX64\x64\link.exe"

# ---------------------------------------------------------------------------
# Locate WDK
# ---------------------------------------------------------------------------
$WdkRoot = (Get-ItemProperty `
    "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots" `
    -ErrorAction SilentlyContinue)."KitsRoot10"
if (-not $WdkRoot) {
    $WdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
}
$WdkRoot = $WdkRoot.TrimEnd('\')

$WdkVer = (Get-ChildItem "$WdkRoot\Include" -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
    Sort-Object Name -Descending |
    Select-Object -First 1).Name
if (-not $WdkVer) { throw "No WDK include version found under $WdkRoot\Include" }

Write-Host "VS   : $VsRoot (MSVC $MsvcVer)" -ForegroundColor DarkGray
Write-Host "WDK  : $WdkRoot ($WdkVer)"       -ForegroundColor DarkGray

$MsvcInc    = "$MsvcToolsDir\$MsvcVer\include"
$MsvcLib    = "$MsvcToolsDir\$MsvcVer\lib\x64"
$WdkKmInc   = "$WdkRoot\Include\$WdkVer\km"
$WdkShInc   = "$WdkRoot\Include\$WdkVer\shared"
$WdkUcrtInc = "$WdkRoot\Include\$WdkVer\ucrt"
$WdkKmLib   = "$WdkRoot\Lib\$WdkVer\km\x64"

$SrcDir = "$PSScriptRoot\src"
$OutDir = "$PSScriptRoot\out"
New-Item -ItemType Directory -Force $OutDir | Out-Null

$Sources = @(
    "$SrcDir\adapter.cpp",
    "$SrcDir\miniport.cpp",
    "$SrcDir\stream.cpp",
    "$SrcDir\topo.cpp",
    "$SrcDir\unkn.cpp",
    "$SrcDir\guids.cpp"
)

# ---------------------------------------------------------------------------
# Compile
# ---------------------------------------------------------------------------
$OptFlags = if ($Optimize) { @('/O2', '/DNDEBUG') } else { @('/Od', '/D_DEBUG') }

$ClFlags = @(
    '/nologo',
    '/W4', '/WX',
    '/kernel',
    '/GF', '/Gy',
    '/GR-',
    '/EHs-c-',
    '/Zp8',
    '/GS-',
    '/Zi',
    '/c',
    '/D_AMD64_=1', '/DAMD64=1', '/D_WIN64=1',
    '/DNTDDI_VERSION=0x0A00000C',
    '/D_WIN32_WINNT=0x0A00',
    '/DWINVER=0x0A00',
    '/DWINNT=1'
)
$ClFlags += $OptFlags
$ClFlags += @(
    "/I`"$WdkKmInc`"",
    "/I`"$WdkShInc`"",
    "/I`"$WdkUcrtInc`"",
    "/I`"$MsvcInc`"",
    "/Fd`"$OutDir\VirtualAudioDriver.pdb`"",
    "/Fo`"$OutDir\\`""
)

Write-Host "=== Compiling ===" -ForegroundColor Cyan
foreach ($src in $Sources) {
    Write-Host "  $([IO.Path]::GetFileName($src))"
    & $Cl @ClFlags $src
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $src" }
}

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
$Objs = $Sources | ForEach-Object {
    "$OutDir\" + [IO.Path]::GetFileNameWithoutExtension($_) + ".obj"
}

$LinkFlags = @(
    '/nologo',
    '/DRIVER',
    '/SUBSYSTEM:NATIVE',
    '/NODEFAULTLIB',
    '/ENTRY:DriverEntry',
    '/MERGE:_TEXT=.text',
    '/MERGE:_PAGE=PAGE',
    '/DEBUG',
    "/PDB:`"$OutDir\VirtualAudioDriver.pdb`"",
    "/OUT:`"$OutDir\VirtualAudioDriver.sys`"",
    "/LIBPATH:`"$WdkKmLib`"",
    "/LIBPATH:`"$MsvcLib`"",
    "portcls.lib", "ks.lib", "ntoskrnl.lib", "hal.lib", "BufferOverflowK.lib"
)

Write-Host "=== Linking ===" -ForegroundColor Cyan
& $Link @LinkFlags @Objs
if ($LASTEXITCODE -ne 0) { throw "Link failed" }

Write-Host "=== Done: $OutDir\VirtualAudioDriver.sys ===" -ForegroundColor Green
