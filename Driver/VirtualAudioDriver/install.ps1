#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

# Auto-detect WDK installation via registry
$kitsRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' `
              -Name KitsRoot10 -ErrorAction SilentlyContinue).KitsRoot10
if (-not $kitsRoot) { throw "Windows Driver Kit not found. Install WDK 10 from https://aka.ms/wdk" }
$WdkVer  = Get-ChildItem "$kitsRoot\bin" |
           Where-Object { $_.Name -match '^\d' } |
           Sort-Object Name -Descending |
           Select-Object -First 1 -ExpandProperty Name
$WdkBin  = "$kitsRoot\bin\$WdkVer"
$SignTool = "$WdkBin\x64\signtool.exe"
$Inf2Cat  = "$WdkBin\x86\inf2cat.exe"
Write-Host "WDK: $WdkVer"

$Root   = $PSScriptRoot
$PkgDir = "$Root\pkg"
$InfSrc = "$Root\VirtualAudioDriver.inf"
$SysSrc = "$Root\out\VirtualAudioDriver.sys"
$HwId   = "Root\VirtualAudioDriver"
$CertCN = "CN=VirtualAudioDriver Test"

# Step 1: Test signing
$bcd = (& bcdedit /enum "{current}") -join "`n"
if ($bcd -notmatch 'testsigning\s+Yes') {
    Write-Warning "Test signing is OFF -- enabling it now. Reboot required before driver will load."
    bcdedit /set testsigning on | Out-Null
}

# Step 2: Test certificate
$cert = Get-ChildItem Cert:\LocalMachine\My |
        Where-Object { $_.Subject -eq $CertCN -and $_.HasPrivateKey } |
        Select-Object -First 1

if (-not $cert) {
    Write-Host "Creating test certificate..."
    $cert = New-SelfSignedCertificate `
        -Type              CodeSigningCert `
        -Subject           $CertCN `
        -CertStoreLocation Cert:\LocalMachine\My `
        -KeyExportPolicy   Exportable
}

foreach ($storeName in @('Root', 'TrustedPublisher')) {
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new($storeName, 'LocalMachine')
    $store.Open('ReadWrite')
    $store.Add($cert)
    $store.Close()
}
Write-Host "Cert thumbprint: $($cert.Thumbprint)"

# Step 3: Assemble package directory
New-Item -ItemType Directory -Force $PkgDir | Out-Null
Copy-Item $SysSrc "$PkgDir\VirtualAudioDriver.sys" -Force
Copy-Item $InfSrc "$PkgDir\VirtualAudioDriver.inf" -Force

# Step 4: Generate catalog
Write-Host "Running inf2cat..."
& $Inf2Cat /driver:"$PkgDir" /os:10_X64
if ($LASTEXITCODE -ne 0) { throw "inf2cat failed" }

# Step 5: Sign .sys and .cat
Write-Host "Signing..."
foreach ($f in @("$PkgDir\VirtualAudioDriver.sys", "$PkgDir\VirtualAudioDriver.cat")) {
    & $SignTool sign /sm /sha1 $cert.Thumbprint /fd sha256 /v $f
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $f" }
}

# Step 6: Remove old staged package then install fresh
$lines = pnputil /enum-drivers
$existing = @()
for ($i = 1; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'virtualaudiodriver') {
        for ($j = $i - 1; $j -ge [Math]::Max(0, $i-5); $j--) {
            if ($lines[$j] -match 'Published Name') {
                $existing += ($lines[$j] -split ':\s+', 2)[1].Trim()
                break
            }
        }
    }
}
foreach ($oem in $existing) {
    Write-Host "Removing old package $oem..."
    pnputil /delete-driver $oem /uninstall /force 2>&1 | Out-Null
}

Write-Host "Adding driver to driver store..."
pnputil /add-driver "$PkgDir\VirtualAudioDriver.inf" /install
# Exit 0 = installed, 259/0x103 = "no devices updated" (still success for staging)
if ($LASTEXITCODE -notin @(0, 259, 3010)) { throw "pnputil /add-driver failed (exit $LASTEXITCODE)" }

# Step 7: Create device node via devcon
$WdkTools  = "$kitsRoot\Tools\$WdkVer"
$devconPath = "$WdkTools\x64\devcon.exe"
if (-not (Test-Path $devconPath)) {
    $found = Get-Command devcon.exe -ErrorAction SilentlyContinue
    if ($found) { $devconPath = $found.Source } else { $devconPath = $null }
}

if ($devconPath) {
    # Remove existing device node so StartDevice re-runs with the new .sys
    Write-Host "Removing old device node..."
    & $devconPath remove $HwId 2>&1 | Out-Null
    Write-Host "Installing new device node..."
    & $devconPath install "$PkgDir\VirtualAudioDriver.inf" $HwId
} else {
    Write-Host ""
    Write-Host "devcon.exe not found -- driver staged but device node not created."
    Write-Host "Run this manually:"
    Write-Host "  devcon install `"$PkgDir\VirtualAudioDriver.inf`" $HwId"
}

# Step 8: Status
Write-Host ""
Write-Host "Staged driver packages:"
pnputil /enum-drivers | Select-String -Context 0,4 'VirtualAudio'
Write-Host ""
Write-Host "MEDIA class devices:"
pnputil /enum-devices /class MEDIA | Select-String -Context 0,3 'Virtual'
