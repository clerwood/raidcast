; RaidCast installer. Ships host and viewer together: ~15 MB either way, and
; either end may want to swap roles later.
;
; Built by .github/workflows/release.yml, which passes MyAppVersion and StageDir.

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0-dev"
#endif
#ifndef StageDir
  #define StageDir "..\build\windows\staging"
#endif

#define MyAppName "RaidCast"
#define MyAppPublisher "RaidCast"
#define MyAppURL "https://github.com/clerwood/raidcast"

[Setup]
AppId={{8F3A6B21-4C7E-4C1E-9F2B-6D5A1E0C7B44}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=Output
OutputBaseFilename=RaidCast-{#MyAppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Per-user install keeps us out of UAC for the common case.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
LicenseFile=..\LICENSE

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\RaidCast Host";   Filename: "{app}\raidcast-host.exe"
Name: "{group}\RaidCast Viewer"; Filename: "{app}\raidcast-viewer.exe"
Name: "{autodesktop}\RaidCast Viewer"; Filename: "{app}\raidcast-viewer.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\raidcast-viewer.exe"; Description: "Launch RaidCast Viewer"; Flags: nowait postinstall skipifsilent
