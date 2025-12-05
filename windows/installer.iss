; Inno Setup script for packaging the Windows build of dsss-transfer-gui.
; Assumes you have prepared a portable folder "dist" that contains:
;   - dsss-transfer-gui.exe
;   - All required DLLs (GTK3 stack, SoapySDR, liquid-dsp, libhackrf, runtimes)
;   - SoapySDR plugins (share/SoapySDR/modules0.8/*.dll)
;   - GLib schemas (share/glib-2.0/schemas/*.xml and .gschema.compiled if used)
;
; Build the installer with Inno Setup (iscc):
;   iscc installer.iss
;

[Setup]
AppName=DSSS Transfer GUI
AppVersion=1.2.0
DefaultDirName={pf}\dsss-transfer
DefaultGroupName=DSSS Transfer
OutputBaseFilename=dsss-transfer-setup
Compression=lzma
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64

[Files]
Source: "dist\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\DSSS Transfer GUI"; Filename: "{app}\dsss-transfer-gui.exe"
Name: "{commondesktop}\DSSS Transfer GUI"; Filename: "{app}\dsss-transfer-gui.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Создать ярлык на рабочем столе"; GroupDescription: "Дополнительные ярлыки:"


