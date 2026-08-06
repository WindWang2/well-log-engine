; Optional Inno Setup script for a classic Windows installer (T15 / #303).
; Requires: Inno Setup 6+ (ISCC.exe) and a prior PyInstaller onedir build.
;
; Build:
;   1. powershell -File well_log_workstation\packaging\build.ps1
;   2. iscc well_log_workstation\packaging\windows\wellplot-desktop.iss
;
; Output: dist\WellPlotDesktop-Setup.exe

#define MyAppName "WellPlot Desktop"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "paleo-workbench"
#define MyAppExeName "WellPlotDesktop.exe"
; Repo-relative paths assume ISCC is run from monorepo root.
#define MyAppSource "..\..\..\dist\WellPlotDesktop"

[Setup]
AppId={{A8E7C1F2-9B4D-4E6A-8C3F-1D2E5F6A7B8C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\WellPlotDesktop
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\..\..\dist
OutputBaseFilename=WellPlotDesktop-Setup
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "{#MyAppSource}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Remove any leftover meta next to the app dir
Type: filesandordirs; Name: "{app}\.wellplot-install-meta.txt"
