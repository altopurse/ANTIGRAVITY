; Inno Setup script - builds a single-file installer for Antigravity Voice Engine.
; Compiled by package.ps1 (paths below are relative to this file).

#define AppName "Antigravity Voice Engine"
#define AppVersion "1.3.0"
#define AppExe "voice-changer.exe"

[Setup]
AppId={{7A5C2E9B-4B1F-4D0A-9C3E-58A1F2D6B7C4}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Antigravity
; Per-user install, no admin prompt; matches where the app stores its license.
PrivilegesRequired=lowest
DefaultDirName={localappdata}\AntigravityVoiceEngine
DisableProgramGroupPage=yes
DisableDirPage=yes
OutputDir=..\dist
OutputBaseFilename=AntigravityVoiceEngine-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=app.ico
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "..\build\bin\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.txt"; DestDir: "{app}"; Flags: ignoreversion
; Sample sounds: don't clobber the user's own imported sounds on upgrade
Source: "..\dist\sounds\*"; DestDir: "{app}\sounds"; Flags: onlyifdoesntexist

[Icons]
Name: "{userprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{userdesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName} now"; Flags: nowait postinstall skipifsilent
; VB-Audio does not permit redistributing VB-CABLE, so offer their official
; download page instead of bundling the driver.
Filename: "https://vb-audio.com/Cable/"; Description: "Get VB-CABLE (virtual mic driver - needed for Discord/games)"; Flags: shellexec nowait postinstall skipifsilent unchecked

[UninstallDelete]
; imgui.ini is created at runtime next to the exe
Type: files; Name: "{app}\imgui.ini"
