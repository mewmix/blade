[Setup]
AppName=blade
AppVersion=1.0.4
AppPublisher=Alex Klein
AppPublisherURL=https://github.com/mewmix
AppSupportURL=https://github.com/mewmix/blade/issues
AppUpdatesURL=https://github.com/mewmix/blade/releases
AppCopyright=© 2025 Alex Klein
DefaultDirName={autopf}\Blade
DefaultGroupName=Blade
OutputBaseFilename=Blade Installer
Compression=lzma
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64

; Use your custom icon for the installer, uninstaller, and Start Menu group
SetupIconFile=blade_icon.ico
UninstallDisplayIcon={app}\blade.exe

[Files]
Source: "blade.exe";           DestDir: "{app}"; Flags: ignoreversion
Source: "BladeExplorer.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "blade_icon.ico";      DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Add installation directory to system PATH for all users
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; \
    Check: NeedsAddPath('{app}')

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; \
  GroupDescription: "Additional icons:"; Flags: unchecked

[Icons]
; Start Menu shortcuts
Name: "{group}\Blade"; \
  Filename: "{app}\blade.exe"; \
  WorkingDir: "{app}"; \
  IconFilename: "{app}\blade_icon.ico"

Name: "{group}\Blade Explorer"; \
  Filename: "{app}\BladeExplorer.exe"; \
  WorkingDir: "{app}"; \
  IconFilename: "{app}\blade_icon.ico"

; Optional Desktop icon
Name: "{autodesktop}\Blade"; \
  Filename: "{app}\blade.exe"; \
  WorkingDir: "{app}"; \
  IconFilename: "{app}\blade_icon.ico"; \
  Tasks: desktopicon

[Code]
function NeedsAddPath(Param: string): boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', OrigPath)
  then
  begin
    Result := True;
    exit;
  end;

  Result := Pos(';' + Param + ';', ';' + OrigPath + ';') = 0;
end;
