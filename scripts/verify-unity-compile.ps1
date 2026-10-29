param([Parameter(Mandatory=$true)][string]$EditorPath)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskData = Join-Path (Split-Path -Parent $EditorPath) 'Data'
$taskCheck = Join-Path $taskRoot 'build\unity-compile'
$taskCompiler = Join-Path $taskData 'DotNetSdkRoslyn\csc.dll'
$taskDotnet = Join-Path $taskData 'NetCoreRuntime\dotnet.exe'
if (-not (Test-Path -LiteralPath $taskCompiler) -or -not (Test-Path -LiteralPath $taskDotnet)) { throw 'This offline gate requires a Unity installation with DotNetSdkRoslyn and NetCoreRuntime' }
New-Item -ItemType Directory -Force -Path $taskCheck | Out-Null
$taskModules = @(Get-ChildItem (Join-Path $taskData 'Managed\UnityEngine\*.dll'))
$taskRefs = @(Get-ChildItem (Join-Path $taskData 'NetStandard\ref\2.1.0\*.dll')) + @($taskModules | Where-Object {$_.Name -like 'UnityEngine*.dll'})
$taskEditorRefs = @($taskModules | Where-Object {$_.Name -like 'UnityEditor*.dll'}) + @(Get-ChildItem (Join-Path $taskData 'Managed\UnityEditor*.dll'))
if ($taskEditorRefs.Name -contains 'UnityEditor.CoreModule.dll') { $taskEditorRefs = @($taskEditorRefs | Where-Object {$_.Name -ne 'UnityEditor.dll'}) }
$taskEditorRefs = @($taskEditorRefs | Group-Object Name | ForEach-Object {$_.Group[0]})
foreach ($taskAssembly in @('Runtime','Editor')) {
    $taskLines = @('-nologo','-target:library','-nostdlib+','-langversion:9','-nowarn:0649',('-out:"'+$taskCheck+'\Poseweaver.'+$taskAssembly+'.dll"'))
    $taskLines += @($taskRefs | ForEach-Object {'-r:"'+$_.FullName+'"'})
    if ($taskAssembly -eq 'Editor') { $taskLines += '-r:"'+$taskCheck+'\Poseweaver.Runtime.dll"'; $taskLines += @($taskEditorRefs | ForEach-Object {'-r:"'+$_.FullName+'"'}) }
    $taskLines += @(Get-ChildItem (Join-Path $taskRoot "integrations\unity\$taskAssembly") -Filter '*.cs' | ForEach-Object {'"'+$_.FullName+'"'})
    $taskResponse = Join-Path $taskCheck ($taskAssembly+'.rsp')
    $taskLines | Set-Content $taskResponse -Encoding UTF8
    $taskOutput = & $taskDotnet $taskCompiler "@$taskResponse" 2>&1
    $taskCode = $LASTEXITCODE
    $taskText = ($taskOutput -join [Environment]::NewLine) + [Environment]::NewLine + "Compiler exit code: $taskCode"
    [IO.File]::WriteAllText((Join-Path $taskCheck ($taskAssembly+'.log')),$taskText,[Text.UTF8Encoding]::new($false))
    if ($taskCode -ne 0) { throw "$taskAssembly compilation failed: $taskOutput" }
}
Write-Output "Unity Runtime and Editor assemblies compile: passed. Import, rendering and Player checks NOT RUN. Evidence: $taskCheck"
