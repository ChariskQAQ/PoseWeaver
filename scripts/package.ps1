# Builds a Release Poseweaver and packages it for Windows:
#   dist\Poseweaver-<version>-win64\        portable folder
#   dist\Poseweaver-<version>-win64.zip     the same folder zipped
#   dist\Poseweaver-<version>-setup.exe     installer (only when Inno Setup 6 is installed)
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
if ($cmake -notmatch 'project\(Poseweaver VERSION ([0-9.]+)') { throw "version not found in CMakeLists.txt" }
$version = $Matches[1]
$name = "Poseweaver-$version-win64"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
$zip = Join-Path $dist "$name.zip"

$safeDist = [IO.Path]::GetFullPath($dist) + [IO.Path]::DirectorySeparatorChar
if (-not [IO.Path]::GetFullPath($stage).StartsWith($safeDist,[StringComparison]::OrdinalIgnoreCase)) { throw 'Stage outside dist' }
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage 'samples') | Out-Null
Copy-Item (Join-Path $root 'build\Release\Poseweaver.exe') $stage
Copy-Item (Join-Path $root 'build\Release\rigcore_bench.exe') $stage
Copy-Item (Join-Path $root 'build\Release\riggpu_bench.exe') $stage
Copy-Item (Join-Path $root 'build\Release\rigeditor_bench.exe') $stage
Copy-Item (Join-Path $root 'samples\*') (Join-Path $stage 'samples')
Copy-Item (Join-Path $root 'integrations') $stage -Recurse
foreach ($plugin in @('integrations\unity\Runtime\Plugins\x86_64','integrations\godot\addons\poseweaver_runtime\bin')) {
    $target = Join-Path $stage $plugin
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    Copy-Item (Join-Path $root 'build\Release\poseweaver_runtime.dll') $target -Force
}
Get-ChildItem (Join-Path $stage 'integrations') -Directory -Filter '.godot' -Recurse -Force | ForEach-Object {
    $cachePath = [IO.Path]::GetFullPath($_.FullName)
    if (-not $cachePath.StartsWith([IO.Path]::GetFullPath($stage) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Cache path outside package' }
    Remove-Item -LiteralPath $cachePath -Recurse -Force
}
Copy-Item (Join-Path $root 'README.md') $stage
Copy-Item (Join-Path $root 'installer\THIRD_PARTY_NOTICES.txt') $stage
Copy-Item (Join-Path $root 'installer\GLAD_NOTICE.txt') $stage
Copy-Item (Join-Path $root 'installer\GODOT_NOTICE.txt') $stage
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'docs') | Out-Null
Copy-Item (Join-Path $root 'docs\images') (Join-Path $stage 'docs') -Recurse
Copy-Item (Join-Path $root 'docs\COMPONENT_CURVES.md'),(Join-Path $root 'docs\LINKED_MESHES.md'),(Join-Path $root 'docs\RUNTIME_INTEGRATION.md') (Join-Path $stage 'docs')
Copy-Item (Join-Path $root 'docs\DESIGN.md'), (Join-Path $root 'docs\ROADMAP.md'), `
    (Join-Path $root 'docs\PERFORMANCE.md'), (Join-Path $root 'docs\performance-baseline.csv'), `
    (Join-Path $root 'docs\gpu-performance-baseline.csv'), (Join-Path $root 'docs\gpu-performance-metadata.json'), `
    (Join-Path $root 'docs\ENGINE_INTEGRATION.md'), (Join-Path $root 'docs\LIGHTING_PIXELS.md'), `
    (Join-Path $root 'docs\effects-performance-baseline.csv'), (Join-Path $root 'docs\RELEASE_ACCEPTANCE.md'), `
    (Join-Path $root 'docs\GETTING_STARTED.md') (Join-Path $stage 'docs')
Copy-Item (Join-Path $root 'docs\editor-performance-before.csv'), (Join-Path $root 'docs\editor-performance-after.csv'), `
    (Join-Path $root 'docs\editor-performance-robot.csv'), (Join-Path $root 'docs\editor-performance-metadata.json') (Join-Path $stage 'docs')
Copy-Item (Join-Path $root 'docs\editor-performance-local-undo.csv'),(Join-Path $root 'docs\editor-performance-local-undo-metadata.txt') (Join-Path $stage 'docs')
Copy-Item (Join-Path $root 'docs\runtime-integration-acceptance.json'),(Join-Path $root 'docs\runtime-godot-render.png') (Join-Path $stage 'docs')

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
        (Join-Path $root 'installer\Poseweaver.iss')
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed" }
    Write-Host "Installer: $dist\Poseweaver-$version-setup.exe"
} else {
    Write-Host "Inno Setup 6 not found: skipped the installer (install it to also get a setup.exe)."
}
