; Inno Setup 6 script for RigEditor. Normally run by scripts\package.ps1, which passes
; /DAppVersion, /DSourceDir (the packaged folder), /DOutputDir and /DIconFile.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\RigEditor-" + AppVersion + "-win64"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif
#ifndef IconFile
  #define IconFile "..\assets\app.ico"
#endif

[Setup]
AppId={{6C1D9F3A-58E2-4E0B-9D3B-2F7A4C1E8B52}
AppName=RigEditor
AppVersion={#AppVersion}
AppVerName=RigEditor {#AppVersion}
AppPublisher=YuanLu
DefaultDirName={autopf}\RigEditor
DefaultGroupName=RigEditor
DisableProgramGroupPage=yes
; Lets the user choose between installing for everyone (admin) or just for themselves.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=RigEditor-{#AppVersion}-setup
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\RigEditor.exe
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

[Icons]
Name: "{autoprograms}\RigEditor"; Filename: "{app}\RigEditor.exe"
Name: "{autodesktop}\RigEditor"; Filename: "{app}\RigEditor.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\RigEditor.exe"; Description: "{cm:LaunchProgram,RigEditor}"; Flags: nowait postinstall skipifsilent
