<#
.SYNOPSIS
  Build and publish a GitHub release for TheAudio.app.

.EXAMPLE
  .\release.ps1
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root      = $PSScriptRoot
$UiPkg     = Join-Path $Root 'ui\package.json'
$BuildDir  = Join-Path $Root 'build'

function Step  ($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan  }
function Ok    ($msg) { Write-Host "    $msg"   -ForegroundColor Green }
function Abort ($msg) { Write-Host "`n[FAIL] $msg" -ForegroundColor Red; exit 1 }
function Ask   ($msg) {
  Write-Host "$msg " -ForegroundColor Yellow -NoNewline
  return (Read-Host).Trim()
}

# ── 1. Version ────────────────────────────────────────────────────────────────

$currentVersion = (Get-Content $UiPkg -Raw | ConvertFrom-Json).version
Step "Version"
Write-Host "    Current: $currentVersion"
$version = Ask "    New version (leave blank to keep):"
if ([string]::IsNullOrWhiteSpace($version)) { $version = $currentVersion }
if ($version -notmatch '^\d+\.\d+\.\d+$') { Abort "Version must be in x.y.z format." }

# ── 2. Artifacts ──────────────────────────────────────────────────────────────

Step "Artifacts"
Write-Host "    [1] Installer only  (Setup .exe)"
Write-Host "    [2] Portable only   (zipped win-unpacked)"
Write-Host "    [3] Both"
$artifactChoice = Ask "    Choice [1/2/3]:"
if ($artifactChoice -notin @('1','2','3')) { Abort "Invalid choice." }
$doInstaller = $artifactChoice -in @('1','3')
$doPortable  = $artifactChoice -in @('2','3')

# ── 3. Release notes ─────────────────────────────────────────────────────────

Step "Release notes"
Write-Host "    Enter an overview for this release (press Enter twice when done):"
$lines = @()
while ($true) {
  $line = Read-Host "   "
  if ($line -eq '' -and $lines.Count -gt 0 -and $lines[-1] -eq '') { break }
  $lines += $line
}
$notes = ($lines | Where-Object { $_ -ne '' -or $lines.IndexOf($_) -lt $lines.Count - 1 }) -join "`n"
$notes = $notes.TrimEnd()
if ([string]::IsNullOrWhiteSpace($notes)) { Abort "Release notes cannot be empty." }

# ── 4. Confirm ────────────────────────────────────────────────────────────────

Step "Summary"
Write-Host "    Version   : v$version"
Write-Host "    Installer : $doInstaller"
Write-Host "    Portable  : $doPortable"
Write-Host "    Notes     :"
$notes -split "`n" | ForEach-Object { Write-Host "      $_" }
$confirm = Ask "`n    Proceed? [y/N]:"
if ($confirm -notmatch '^[Yy]$') { Write-Host "Aborted." -ForegroundColor Yellow; exit 0 }

# ── 5. Bump version in package.json ──────────────────────────────────────────

Step "Updating package.json to v$version"
$pkg = Get-Content $UiPkg -Raw
$pkg = $pkg -replace '"version"\s*:\s*"[^"]+"', "`"version`": `"$version`""
[System.IO.File]::WriteAllText($UiPkg, $pkg, (New-Object System.Text.UTF8Encoding $false))
Ok "Done"

# ── 6. Build ──────────────────────────────────────────────────────────────────

Step "Building"
& "$Root\build.ps1"
if ($LASTEXITCODE -ne 0) { Abort "build.ps1 failed." }

# ── 7. Locate installer ───────────────────────────────────────────────────────

$installerPath = Get-ChildItem -Path $BuildDir -Filter 'TheAudio.app Setup *.exe' -ErrorAction SilentlyContinue |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1 -ExpandProperty FullName

if ($doInstaller -and -not $installerPath) { Abort "Installer .exe not found in $BuildDir." }

# ── 8. Build portable zip ─────────────────────────────────────────────────────

$portablePath = $null
if ($doPortable) {
  Step "Creating portable zip"
  $unpackedDir  = Join-Path $BuildDir 'win-unpacked'
  if (-not (Test-Path $unpackedDir)) { Abort "win-unpacked not found in $BuildDir." }
  $portablePath = Join-Path $BuildDir "TheAudio.app-v$version-portable.zip"
  if (Test-Path $portablePath) { Remove-Item $portablePath -Force }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::CreateFromDirectory($unpackedDir, $portablePath)
  Ok "Created: $portablePath"
}

# ── 9. GitHub release ─────────────────────────────────────────────────────────

Step "Creating GitHub release v$version"

$tag = "v$version"
$assets = @()
if ($doInstaller) { $assets += $installerPath }
if ($doPortable)  { $assets += $portablePath  }

$notesFile = Join-Path $env:TEMP 'theaudio_release_notes.txt'
[System.IO.File]::WriteAllText($notesFile, $notes, (New-Object System.Text.UTF8Encoding $false))

$ghArgs = @('release', 'create', $tag, '--title', "v$version", '--notes-file', $notesFile) + $assets
& gh @ghArgs
if ($LASTEXITCODE -ne 0) { Abort "gh release create failed." }

Remove-Item $notesFile -Force -ErrorAction SilentlyContinue

# ── 10. Done ──────────────────────────────────────────────────────────────────

Step "Release complete"
Write-Host ""
Write-Host "  Tag     : $tag" -ForegroundColor Cyan
if ($doInstaller) { Write-Host "  Setup   : $installerPath"  -ForegroundColor Green }
if ($doPortable)  { Write-Host "  Portable: $portablePath"   -ForegroundColor Green }
Write-Host ""
