[Setup]
AppName=blade
AppVersion=1.0.1
DefaultDirName={autopf}\Blade
DefaultGroupName=Blade
OutputBaseFilename=blade_install
Compression=lzma
SolidCompression=yes
PrivilegesRequired=admin 
ArchitecturesInstallIn64BitMode=x64

[Files]
Source: "blade.exe"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
; This updates the SYSTEM path so it works for all users
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; \
    Check: NeedsAddPath('{app}')

[Code]
function NeedsAddPath(Param: string): boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', OrigPath)
  then begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Param + ';', ';' + OrigPath + ';') = 0;
end;