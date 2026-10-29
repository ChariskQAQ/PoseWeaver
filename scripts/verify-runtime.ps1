param([Parameter(Mandatory=$true)][string]$GodotPath, [string]$WindowsTemplate)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskWork = Join-Path $taskRoot ('build\runtime-acceptance-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path (Join-Path $taskWork 'addons'),(Join-Path $taskWork 'tests'),(Join-Path $taskWork 'example') | Out-Null
Copy-Item (Join-Path $taskRoot 'integrations\godot\addons\poseweaver_runtime') (Join-Path $taskWork 'addons') -Recurse
New-Item -ItemType Directory -Force -Path (Join-Path $taskWork 'addons\poseweaver_runtime\bin') | Out-Null
Copy-Item (Join-Path $taskRoot 'build\Poseweaver-Verified\poseweaver_runtime.dll') (Join-Path $taskWork 'addons\poseweaver_runtime\bin')
Copy-Item (Join-Path $taskRoot 'integrations\godot\tests\skeleton*.gd') (Join-Path $taskWork 'tests')
@'
config_version=5
[application]
config/name="Poseweaver Runtime Acceptance"
run/main_scene="res://main.tscn"
[editor_plugins]
enabled=PackedStringArray("res://addons/poseweaver_runtime/plugin.cfg")
[rendering]
renderer/rendering_method="gl_compatibility"
'@ | Set-Content (Join-Path $taskWork 'project.godot') -Encoding UTF8
$taskScene = @'
[gd_scene load_steps=3 format=3]
[ext_resource type="Script" path="res://tests/skeleton_player_test.gd" id="1"]
[ext_resource type="PackedScene" path="res://example/robot.pwskeleton" id="2"]
[node name="Acceptance" type="Node"]
script=ExtResource("1")
robot_scene=ExtResource("2")
'@
[IO.File]::WriteAllText((Join-Path $taskWork 'main.tscn'),$taskScene,[Text.UTF8Encoding]::new($false))
function Invoke-RuntimeGate([string]$program,[string]$arguments,[string]$label) {
    $taskStart = New-Object Diagnostics.ProcessStartInfo
    $taskStart.FileName = $program; $taskStart.Arguments = $arguments; $taskStart.WorkingDirectory = $taskRoot
    $taskStart.UseShellExecute = $false; $taskStart.CreateNoWindow = $true; $taskStart.WindowStyle = 'Hidden'
    $taskStart.RedirectStandardOutput = $true; $taskStart.RedirectStandardError = $true
    $taskStart.EnvironmentVariables['APPDATA'] = Join-Path $taskWork 'profile'
    $taskStart.EnvironmentVariables['LOCALAPPDATA'] = Join-Path $taskWork 'profile'
    $taskProcess = [Diagnostics.Process]::Start($taskStart)
    $taskOut = $taskProcess.StandardOutput.ReadToEndAsync(); $taskErr = $taskProcess.StandardError.ReadToEndAsync()
    $taskTimeout = -not $taskProcess.WaitForExit(30000)
    if ($taskTimeout) { $taskProcess.Kill(); $taskProcess.WaitForExit() }
    $taskText = $taskOut.Result + $taskErr.Result
    $taskText | Set-Content (Join-Path $taskWork "$label.log") -Encoding UTF8
    if ($taskTimeout) { throw "$label timed out; see $taskWork" }
    if ($taskProcess.ExitCode -ne 0 -or $taskText -match 'SCRIPT ERROR|ERROR:|FAIL:') { throw "$label failed; see $taskWork" }
    return $taskText
}
Invoke-RuntimeGate (Join-Path $taskRoot 'build\Poseweaver-Verified\Poseweaver.exe') ('"{0}" --run skeleton "{1}" --run skeleton "{2}"' -f (Join-Path $taskRoot 'samples\robot-starter.rig.json'),(Join-Path $taskWork 'example\robot.pwskeleton'),(Join-Path $taskWork 'root-robot.pwskeleton')) 'export' | Out-Null
# Godot #111048: allow deferred extension documentation work before import-mode shutdown.
Invoke-RuntimeGate $GodotPath ('--headless --frame-delay 800 --editor --path "{0}" --import' -f $taskWork) 'import' | Out-Null
$taskResult = Invoke-RuntimeGate $GodotPath ('--headless --path "{0}" --script res://tests/skeleton_cli_test.gd' -f $taskWork) 'playback'
if ($taskResult -notmatch 'Poseweaver Godot skeleton: 0 failure\(s\)') { throw 'Missing playback success marker' }
if ($WindowsTemplate) {
    if (-not (Test-Path -LiteralPath $WindowsTemplate -PathType Leaf)) { throw 'Windows export template does not exist' }
    $taskTemplate = [IO.Path]::GetFullPath($WindowsTemplate).Replace('\','/')
    $taskPreset = @"
[preset.0]
name="Windows"
platform="Windows Desktop"
export_filter="all_resources"
include_filter=""
exclude_filter=""
export_path="player/RuntimeAcceptance.exe"
[preset.0.options]
custom_template/release="$taskTemplate"
binary_format/architecture="x86_64"
codesign/enable=false
application/modify_resources=false
"@
    [IO.File]::WriteAllText((Join-Path $taskWork 'export_presets.cfg'),$taskPreset,[Text.UTF8Encoding]::new($false))
    New-Item -ItemType Directory -Force -Path (Join-Path $taskWork 'player') | Out-Null
    Invoke-RuntimeGate $GodotPath ('--headless --frame-delay 800 --path "{0}" --export-release Windows' -f $taskWork) 'build-player' | Out-Null
    $taskResult = Invoke-RuntimeGate (Join-Path $taskWork 'player\RuntimeAcceptance.exe') '--headless' 'exported-player'
    if ($taskResult -notmatch 'Poseweaver Godot exported player: passed') { throw 'Missing exported player success marker' }
    $taskResult = Invoke-RuntimeGate (Join-Path $taskWork 'player\RuntimeAcceptance.exe') ('--rendering-method gl_compatibility -- --render-output "{0}"' -f (Join-Path $taskWork 'rendered-player.png')) 'rendered-player'
    if ($taskResult -notmatch 'Poseweaver Godot rendered player: 0 failure\(s\)') { throw 'Missing rendered player success marker' }
}
Write-Output "Godot runtime passed; evidence: $taskWork"
