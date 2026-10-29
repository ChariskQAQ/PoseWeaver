param([string]$OldPackage, [string]$NewPackage, [string]$IsccPath)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskIscc = $IsccPath
if (-not $taskIscc) { $taskIscc = (Get-Command ISCC.exe -ErrorAction Stop).Source }
if (-not (Test-Path -LiteralPath $taskIscc)) { throw 'Pass -IsccPath to the Inno Setup compiler' }
if (-not $OldPackage -or -not $NewPackage) { throw 'Pass -OldPackage and -NewPackage portable directories' }
$taskRun = Join-Path $taskRoot ('build\installer-acceptance-' + [guid]::NewGuid().ToString('N'))
$taskInstall = Join-Path $taskRun 'installed'
$taskProfile = Join-Path $taskRun 'profile'
New-Item -ItemType Directory -Force -Path $taskRun, (Join-Path $taskProfile 'RigEditor') | Out-Null
$taskIdentity = 'Poseweaver.Acceptance.' + [guid]::NewGuid().ToString('N')
$taskSource = Get-Content (Join-Path $taskRoot 'installer\Poseweaver.iss') -Raw
$taskSource = $taskSource.Replace('AppId={{6C1D9F3A-58E2-4E0B-9D3B-2F7A4C1E8B52}',"AppId=$taskIdentity")
$taskSource = [regex]::Replace($taskSource,'(?ms)\[InstallDelete\].*?(?=\[Icons\])','')
$taskSource = [regex]::Replace($taskSource,'(?ms)\[Icons\].*?(?=\[Run\])','')
$taskScript = Join-Path $taskRun 'acceptance.iss'
$taskSource | Set-Content -LiteralPath $taskScript -Encoding UTF8
$taskSettings = Join-Path $taskProfile 'RigEditor\settings.json'
'{"acceptanceSentinel":true}' | Set-Content $taskSettings -Encoding UTF8
$taskSettingsHash = (Get-FileHash $taskSettings).Hash
$taskUninstalled = $false
function Invoke-InstallerChild([string]$exe,[string]$arguments) {
    $taskStart = New-Object Diagnostics.ProcessStartInfo
    $taskStart.FileName = $exe; $taskStart.Arguments = $arguments
    $taskStart.UseShellExecute = $false; $taskStart.CreateNoWindow = $true; $taskStart.WindowStyle = 'Hidden'
    $taskStart.EnvironmentVariables['APPDATA'] = $taskProfile
    $taskStart.EnvironmentVariables['LOCALAPPDATA'] = $taskProfile
    $taskProcess = [Diagnostics.Process]::Start($taskStart)
    if (-not $taskProcess.WaitForExit(60000)) { $taskProcess.Kill(); throw 'Installer timed out' }
    if ($taskProcess.ExitCode -ne 0) { throw "Installer exited $($taskProcess.ExitCode)" }
}
try {
    foreach ($taskPackage in @($OldPackage,$NewPackage)) {
        $taskPackage = [IO.Path]::GetFullPath($taskPackage)
        $taskVersion = (Get-Item (Join-Path $taskPackage 'Poseweaver.exe')).VersionInfo.ProductVersion
        & $taskIscc /Q "/DAppVersion=$taskVersion" "/DSourceDir=$taskPackage" "/DOutputDir=$taskRun" "/DIconFile=$taskRoot\assets\app.ico" $taskScript
        if ($LASTEXITCODE -ne 0) { throw 'Acceptance installer compilation failed' }
        Invoke-InstallerChild (Join-Path $taskRun "Poseweaver-$taskVersion-setup.exe") ('/CURRENTUSER /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOICONS /DIR="{0}" /LOG="{1}"' -f $taskInstall,(Join-Path $taskRun "install-$taskVersion.log"))
        if ((Get-FileHash (Join-Path $taskInstall 'Poseweaver.exe')).Hash -ne (Get-FileHash (Join-Path $taskPackage 'Poseweaver.exe')).Hash) { throw 'Installed binary mismatch' }
        if ((Get-FileHash $taskSettings).Hash -ne $taskSettingsHash) { throw 'Installer changed user settings' }
        if (-not (Test-Path (Join-Path $taskInstall 'samples\lighting-pixels.rig.json'))) { throw 'Installed sample missing' }
        'user-owned project' | Set-Content (Join-Path $taskInstall 'user-kept.rig.json') -Encoding UTF8
        Write-Output "Installed/upgraded ${taskVersion}: passed"
    }
    Invoke-InstallerChild (Join-Path $taskInstall 'unins000.exe') ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="{0}"' -f (Join-Path $taskRun 'uninstall.log'))
    $taskUninstalled = $true
    if ((Test-Path (Join-Path $taskInstall 'Poseweaver.exe')) -or -not (Test-Path (Join-Path $taskInstall 'user-kept.rig.json'))) { throw 'Uninstall removed user data or retained application' }
    if ((Get-FileHash $taskSettings).Hash -ne $taskSettingsHash) { throw 'Uninstall changed user settings' }
    Write-Output "Installer lifecycle passed. Evidence: $taskRun"
} finally {
    if (-not $taskUninstalled -and (Test-Path (Join-Path $taskInstall 'unins000.exe'))) { Invoke-InstallerChild (Join-Path $taskInstall 'unins000.exe') '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' }
}
