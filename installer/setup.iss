; Inno Setup script - builds a single-file installer for Antigravity Voice Engine.
; Compiled by package.ps1 (paths below are relative to this file).

#define AppName "Antigravity Voice Engine"
#define AppVersion "2.5.0"
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
; Checked by default: without VB-CABLE the app's output can't reach
; Discord/games. Skipped automatically if the driver is already present.
Name: "vbcable"; Description: "Download and install the VB-CABLE virtual audio driver (needed for Discord/games)"; GroupDescription: "Audio driver:"; Check: not VBCableInstalled

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

[UninstallRun]
; Best-effort ping so the dashboard knows this machine removed the app. Reads
; the anonymous device id the app wrote, fires one short HTTP request, never
; blocks or fails the uninstall (errors swallowed).
Filename: "powershell.exe"; Parameters: "-NoProfile -WindowStyle Hidden -Command ""try {{ $d = (Get-Content -Raw '{app}\device.id').Trim(); Invoke-WebRequest -UseBasicParsing -TimeoutSec 5 -Uri ('https://antigravity-license.onrender.com/api/uninstall?device=' + $d) | Out-Null }} catch {{}}"""; Flags: runhidden; RunOnceId: "uninstallping"

[UninstallDelete]
; imgui.ini and device.id are created at runtime next to the exe
Type: files; Name: "{app}\imgui.ini"
Type: files; Name: "{app}\device.id"

[Code]
// VB-Audio doesn't permit redistributing VB-CABLE inside other installers,
// so it's downloaded from their official server at install time, extracted,
// and its own setup is launched (needs one UAC click + their Install button).
var
  DownloadPage: TDownloadWizardPage;

function VBCableInstalled: Boolean;
begin
  // The driver registers this service name when installed
  Result := RegKeyExists(HKLM, 'SYSTEM\CurrentControlSet\Services\VBAudioVACWDM');
end;

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  Result := True;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage('Downloading VB-CABLE',
    'Getting the virtual audio driver from vb-audio.com...', @OnDownloadProgress);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpReady) and WizardIsTaskSelected('vbcable') then begin
    DownloadPage.Clear;
    DownloadPage.Add('https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip', 'vbcable.zip', '');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
      except
        // Download failed (offline / URL moved): not fatal, the app installs
        // fine; the post-install step falls back to opening their website.
        Log('VB-CABLE download failed: ' + GetExceptionMessage);
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  ZipPath, ExtractDir, DriverSetup: String;
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('vbcable') then begin
    ZipPath := ExpandConstant('{tmp}\vbcable.zip');
    ExtractDir := ExpandConstant('{tmp}\vbcable');

    if FileExists(ZipPath) then begin
      Exec('powershell.exe',
           '-NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath ''' +
           ZipPath + ''' -DestinationPath ''' + ExtractDir + ''' -Force"',
           '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    end;

    // Pick the setup that matches the OS: the driver pack ships both
    // VBCABLE_Setup_x64.exe (64-bit Windows) and VBCABLE_Setup.exe (32-bit).
    // Running the wrong one fails, so choose by architecture.
    if IsWin64 then
      DriverSetup := ExtractDir + '\VBCABLE_Setup_x64.exe'
    else
      DriverSetup := ExtractDir + '\VBCABLE_Setup.exe';

    // Fall back to whichever exists if the expected one is missing (pack layout
    // has changed before).
    if not FileExists(DriverSetup) then begin
      if FileExists(ExtractDir + '\VBCABLE_Setup_x64.exe') then
        DriverSetup := ExtractDir + '\VBCABLE_Setup_x64.exe'
      else if FileExists(ExtractDir + '\VBCABLE_Setup.exe') then
        DriverSetup := ExtractDir + '\VBCABLE_Setup.exe';
    end;

    if FileExists(DriverSetup) then begin
      MsgBox('VB-CABLE''s own installer will open now (Windows will ask for admin permission).' + #13#10 +
             'Click "Install Driver" in it, then reboot when it says so.', mbInformation, MB_OK);
      // Driver installation requires elevation; 'runas' shows the UAC prompt
      if not ShellExec('runas', DriverSetup, '', ExtractDir, SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) then
        ShellExec('open', 'https://vb-audio.com/Cable/', '', '', SW_SHOWNORMAL, ewNoWait, ResultCode);
    end else begin
      // Zip missing or extraction failed: open the official page instead
      ShellExec('open', 'https://vb-audio.com/Cable/', '', '', SW_SHOWNORMAL, ewNoWait, ResultCode);
    end;
  end;
end;
