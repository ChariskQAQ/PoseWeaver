; Inno Setup 6 script for Poseweaver. Normally run by scripts\package.ps1, which passes
; /DAppVersion, /DSourceDir (the packaged folder), /DOutputDir and /DIconFile.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\Poseweaver-" + AppVersion + "-win64"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif
#ifndef IconFile
  #define IconFile "..\assets\app.ico"
#endif

[Setup]
; Keep the application ID so existing installations can upgrade.
AppId={{6C1D9F3A-58E2-4E0B-9D3B-2F7A4C1E8B52}
AppName=Poseweaver
AppVersion={#AppVersion}
AppVerName=Poseweaver {#AppVersion}
AppPublisher=YuanLu
DefaultDirName={autopf}\Poseweaver
DefaultGroupName=Poseweaver
DisableProgramGroupPage=yes
; Lets the user choose between installing for everyone (admin) or just for themselves.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=Poseweaver-{#AppVersion}-setup
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\Poseweaver.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
#if FileExists(AddBackslash(CompilerPath) + "Languages\ChineseSimplified.isl")
Name: "chs"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
#endif
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
Type: files; Name: "{app}\RigEditor.exe"
Type: files; Name: "{autoprograms}\RigEditor.lnk"
Type: files; Name: "{autodesktop}\RigEditor.lnk"

[Icons]
Name: "{autoprograms}\Poseweaver"; Filename: "{app}\Poseweaver.exe"
Name: "{autodesktop}\Poseweaver"; Filename: "{app}\Poseweaver.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Poseweaver.exe"; Description: "{cm:LaunchProgram,Poseweaver}"; Flags: nowait postinstall skipifsilent
