; Windows installer for the native edition (Inno Setup 6). Build it after cpp\build-win.bat:
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=0.3.0 installer\windows.iss
; Output: dist\Undae-Sonicae-<version>-windows-setup.exe. Installs to Program Files (or, if chosen, just for this user)
; with a Start menu entry; the plug-in hosts sit next to the app, where it looks for them; libmp3lame.dll is included
; when it is in the build folder (MP3 export). Projects show under "Open with" for .json files without taking .json over.

#ifndef AppVersion
  #define AppVersion "0.3.0"
#endif
#define BuildDir "..\cpp\build\UndaeSonicae_artefacts\Release"

[Setup]
AppId={{6E2B7C1A-4F3D-4B8E-9A51-2C7D8E9F0A13}
AppName=Undae Sonicae
AppVersion={#AppVersion}
AppVerName=Undae Sonicae {#AppVersion}
AppPublisher=Undae Sonicae
DefaultDirName={autopf}\Undae Sonicae
DefaultGroupName=Undae Sonicae
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\Undae Sonicae.exe
UninstallDisplayName=Undae Sonicae
OutputDir=..\dist
OutputBaseFilename=Undae-Sonicae-{#AppVersion}-windows-setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern
ChangesAssociations=yes

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "{#BuildDir}\Undae Sonicae.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\libmp3lame.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; Microsoft's C++ runtime, app-local (as Microsoft allows), so the app starts on a Windows without the VC++ redistributable
Source: "{#BuildDir}\msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#BuildDir}\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#BuildDir}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\native\bin\undae-vsthost.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\native\bin\undae-vsthost32.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\app\img\wordmark.png"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\cpp\examples\first-light.undae.json"; DestDir: "{app}\examples"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Undae Sonicae"; Filename: "{app}\Undae Sonicae.exe"
Name: "{autodesktop}\Undae Sonicae"; Filename: "{app}\Undae Sonicae.exe"; Tasks: desktopicon

[Registry]
; "Open with" for project files (*.undae.json): a ProgID listed under .json's OpenWithProgids, not the default handler
Root: HKA; Subkey: "Software\Classes\UndaeSonicae.Project"; ValueType: string; ValueName: ""; ValueData: "Undae Sonicae project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\UndaeSonicae.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Undae Sonicae.exe,0"
Root: HKA; Subkey: "Software\Classes\UndaeSonicae.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Undae Sonicae.exe"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.json\OpenWithProgids"; ValueType: string; ValueName: "UndaeSonicae.Project"; ValueData: ""; Flags: uninsdeletevalue

[Run]
Filename: "{app}\Undae Sonicae.exe"; Parameters: """{app}\examples\first-light.undae.json"""; Description: "Start Undae Sonicae with the demo song"; Flags: nowait postinstall skipifsilent
