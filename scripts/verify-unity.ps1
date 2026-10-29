param([string]$EditorPath, [switch]$PrepareOnly, [switch]$BuildPlayer)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskProject = Join-Path $taskRoot 'build\unity-acceptance'
$taskAssets = Join-Path $taskProject 'Assets'
New-Item -ItemType Directory -Force -Path $taskAssets, (Join-Path $taskProject 'Packages'), (Join-Path $taskProject 'ProjectSettings') | Out-Null
foreach ($taskFolder in @('Runtime','Editor')) {
    $taskTarget = Join-Path $taskAssets "Poseweaver\$taskFolder"
    New-Item -ItemType Directory -Force -Path $taskTarget | Out-Null
    Copy-Item (Join-Path $taskRoot "integrations\unity\$taskFolder\*") $taskTarget -Recurse -Force
}
Copy-Item (Join-Path $taskRoot 'integrations\unity\Samples~\Demo') $taskAssets -Recurse -Force
$taskBinary = Join-Path $taskRoot 'build\Poseweaver-Verified\poseweaver_runtime.dll'
if (-not (Test-Path -LiteralPath $taskBinary)) { throw 'Build poseweaver_runtime before preparing Unity acceptance' }
$taskPlugin = Join-Path $taskAssets 'Poseweaver\Runtime\Plugins\x86_64'
New-Item -ItemType Directory -Force -Path $taskPlugin,(Join-Path $taskAssets 'RuntimeDemo') | Out-Null
Copy-Item -LiteralPath $taskBinary -Destination $taskPlugin -Force
$taskExport = New-Object Diagnostics.ProcessStartInfo
$taskExport.FileName = Join-Path $taskRoot 'build\Poseweaver-Verified\Poseweaver.exe'
$taskExport.Arguments = '"{0}" --run skeleton "{1}"' -f (Join-Path $taskRoot 'samples\robot-starter.rig.json'),(Join-Path $taskAssets 'RuntimeDemo\robot.pwskeleton')
$taskExport.WorkingDirectory = $taskRoot; $taskExport.UseShellExecute = $false; $taskExport.CreateNoWindow = $true; $taskExport.WindowStyle = 'Hidden'
$taskExport.EnvironmentVariables['APPDATA'] = Join-Path $taskProject 'export-profile'
$taskProcess = [Diagnostics.Process]::Start($taskExport)
if (-not $taskProcess.WaitForExit(30000)) { $taskProcess.Kill(); throw 'Runtime fixture export timed out' }
if ($taskProcess.ExitCode -ne 0) { throw 'Runtime fixture export failed' }
[IO.File]::WriteAllText((Join-Path $taskProject 'Packages\manifest.json'),'{"dependencies":{"com.unity.modules.imageconversion":"1.0.0","com.unity.modules.jsonserialize":"1.0.0"}}',[Text.UTF8Encoding]::new($false))
if ($PrepareOnly) { Write-Output "Prepared Unity acceptance project: $taskProject; Editor and player checks NOT RUN"; exit 0 }
if (-not $EditorPath -or -not (Test-Path -LiteralPath $EditorPath -PathType Leaf)) { throw 'Pass -EditorPath to an installed Unity Editor, or -PrepareOnly' }
function Invoke-UnityGate([string]$method,[string]$log,[string]$marker,[string]$extra='') {
    $taskStart = New-Object Diagnostics.ProcessStartInfo
    $taskStart.FileName = $EditorPath; $taskStart.UseShellExecute = $false; $taskStart.CreateNoWindow = $true; $taskStart.WindowStyle = 'Hidden'
    $taskStart.Arguments = '-batchmode -nographics -quit -projectPath "{0}" -executeMethod {1} -poseweaverSample Assets/Demo/arm.pwframes -logFile "{2}" {3}' -f $taskProject,$method,$log,$extra
    $taskProcess = [Diagnostics.Process]::Start($taskStart)
    # Unity import/build can take minutes; the child is independent and this loop stays interruptible.
    $taskDeadline = [DateTime]::UtcNow.AddMinutes(15)
    while (-not $taskProcess.WaitForExit(1000)) { if ([DateTime]::UtcNow -gt $taskDeadline) { $taskProcess.Kill(); throw 'Unity gate timed out' } }
    $taskText = Get-Content -LiteralPath $log -Raw
    if ($taskProcess.ExitCode -eq 198 -or $taskText -match 'No valid Unity Editor license found') { throw "Unity Editor license unavailable. Activate in Unity Hub and rerun; see $log" }
    if ($taskProcess.ExitCode -ne 0 -or $taskText -notmatch [regex]::Escape($marker) -or $taskText -match 'error CS\d+|Scripts have compiler errors|Batchmode quit due to failure') { throw "Unity gate failed; see $log" }
    Write-Output $marker
}
Invoke-UnityGate 'Poseweaver.Editor.FramesSmokeTest.Run' (Join-Path $taskProject 'import-playback.log') 'Poseweaver Unity frames: passed'
Invoke-UnityGate 'Poseweaver.Editor.SkeletonSmokeTest.Run' (Join-Path $taskProject 'skeleton-playback.log') 'Poseweaver Unity skeleton: passed'
if ($BuildPlayer) {
    Invoke-UnityGate 'Poseweaver.Editor.FramesSmokeTest.BuildWindowsPlayer' (Join-Path $taskProject 'windows-build.log') 'Poseweaver Unity Windows build: passed' ('-poseweaverBuild "{0}"' -f (Join-Path $taskProject 'Player\PoseweaverAcceptance.exe'))
}
