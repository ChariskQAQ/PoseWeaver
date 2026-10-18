# Builds a Release RigEditor and packages it for Windows:
#   dist\RigEditor-<version>-win64\        portable folder
#   dist\RigEditor-<version>-win64.zip     the same folder zipped
#   dist\RigEditor-<version>-setup.exe     installer (only when Inno Setup 6 is installed)
# Usage: scripts\package.bat [-SkipBuild]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $SkipBuild) {
    & cmd /c "scripts\build.bat Release"
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(RigEditor VERSION ([0-9.]+)') { throw "version not found in CMakeLists.txt" }
$version = $Matches[1]
$name = "RigEditor-$version-win64"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
$zip = Join-Path $dist "$name.zip"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage 'samples') | Out-Null
Copy-Item (Join-Path $root 'build\Release\RigEditor.exe') $stage
Copy-Item (Join-Path $root 'samples\*') (Join-Path $stage 'samples')
Copy-Item (Join-Path $root 'README.md') $stage
Copy-Item (Join-Path $root 'installer\THIRD_PARTY_NOTICES.txt') $stage

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "Portable: $zip"

# Installer through Inno Setup's command-line compiler, when available.
$iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
foreach ($p in @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
                 "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
    if (-not $iscc -and (Test-Path $p)) { $iscc = $p }
}
if ($iscc) {
    & $iscc /Q "/DAppVersion=$version" "/DSourceDir=$stage" "/DOutputDir=$dist" "/DIconFile=$root\assets\app.ico" `
        (Join-Path $root 'installer\RigEditor.iss')
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed" }
    Write-Host "Installer: $dist\RigEditor-$version-setup.exe"
} else {
    Write-Host "Inno Setup 6 not found: skipped the installer (install it to also get a setup.exe)."
}
