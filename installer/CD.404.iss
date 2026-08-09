#define MyAppName "CD.404"
#define MyAppVersion "0.2.0-public-beta.1"
#define MyAppExeName "CD.404.exe"

[Setup]
AppId={{E25F75DA-3AA6-4F9E-854D-2FBA77139C54}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=CD.404 contributors
DefaultDirName={localappdata}\Programs\CD.404
DefaultGroupName=CD.404
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\out\package
OutputBaseFilename=CD.404-{#MyAppVersion}-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}
InfoBeforeFile=..\docs\PRIVACY.md
VersionInfoVersion=0.2.0.0
VersionInfoDescription=CD.404 per-user installer

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\out\build\ninja-msvc-x64-release\apps\cd404\CD.404.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\PRIVACY.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\CD.404"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\CD.404"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,CD.404}"; Flags: nowait postinstall skipifsilent
