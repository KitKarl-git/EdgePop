; EdgePOP installer - Inno Setup.
;
; PER-USER, NO UAC - this is required, not a preference: EdgePop keeps
; EdgePop.ini next to the exe and never elevates ("No UAC, ever", assumed
; defaults), so a Program Files install would silently break saving
; settings (unelevated writes there fail). {localappdata}\Programs is
; user-writable, needs no elevation, and uninstalls cleanly.
;
; Rebuild after a code change: cmake --build build --config Release,
; then ISCC installer\edgepop.iss -> dist\EdgePop-Setup-<ver>.exe.
; Unsigned (no code-signing cert yet): downloaders see SmartScreen's
; "unknown publisher" prompt.

#define MyAppName "EdgePop"
#define MyAppVersion "1.2.1"
#define MyAppPublisher "KitKarl"
#define MyAppURL "https://kitkarl.com"
#define MyAppExeName "EdgePop.exe"

[Setup]
AppId={{04bcc8c3-503a-4bb0-85ca-3788e4e413ea}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName=EdgePOP by KitKarl {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=EdgePop-Setup-1.2.1
SetupIconFile=..\src\app.ico
UninstallDisplayName=EdgePOP by KitKarl
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "..\build\Release\EdgePop.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\EdgePop"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\EdgePop"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Same entry the app itself maintains two-way on every launch:
; HKCU Run, name "EdgePop", quoted exe path.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "EdgePop"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch EdgePOP now"; Flags: nowait postinstall skipifsilent

[Code]
procedure StopRunningEdgePop();
var
  ResultCode: Integer;
begin
  // Graceful first (WM_CLOSE gives the app its exit path - debounced INI
  // saves are flushed at exit,); force only if it survived.
  Exec(ExpandConstant('{cmd}'), '/C taskkill /IM EdgePop.exe >nul 2>&1',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(600);
  Exec(ExpandConstant('{cmd}'), '/C taskkill /F /IM EdgePop.exe >nul 2>&1',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  StopRunningEdgePop();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    StopRunningEdgePop();
end;
