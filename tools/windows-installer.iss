; Inno Setup script for Seabass (Windows testing builds).
;
; Prerequisite: build-win\ must already contain a fresh Release build plus
; deploy-windows.ps1's output (windeployqt + DLL closure walk). Run, in order:
;   cmake --build build-win --target seabass-cli
;   cmake --build build-win --target seabass
;   .\tools\deploy-windows.ps1
;   & "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" tools\windows-installer.iss
;
; Only the runtime files deploy-windows.ps1 actually produces are listed
; explicitly below (exes, DLLs, Qt plugin dirs) -- build-win\ also contains
; CMake/Ninja intermediates and ~20 unit-test .exe files that must NOT ship.
;
; qt.conf (deploy-windows.ps1 writes one) IS needed, contrary to what this
; comment used to say: without it, seabass.exe resolves its QML imports to
; this Qt install's own compile-time path (C:\msys64\ucrt64\qml) instead of
; the qml\ copy sitting right beside it, which happened to still work on
; every machine this was tested on before because that path is also a
; real, correct answer *there* -- MSYS2 is what built the exe in the first
; place. Installed on a machine that never had MSYS2 at that exact path,
; every "module ... is not installed" QML import fails and the app never
; gets past QQmlApplicationEngine::load(). Confirmed directly.

#define MyAppName "Seabass"
; CI passes the version and channel from the release tag
; (/DMyAppVersion=0.2.1 /DMyAppChannel=beta). The fallback is for a build
; by hand and is deliberately not a version anybody could mistake for a
; release: the one source of truth is project(seabass VERSION ...) in
; CMakeLists.txt, and tools/release.sh refuses a tag that disagrees.
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0-dev"
#endif
#ifndef MyAppChannel
  #define MyAppChannel "dev"
#endif
#define MyAppPublisher "Sebastian Kugler"
#define BuildDir "..\build-win"

[Setup]
AppId={{5B1E9F2B-6C3E-4B2E-9C6D-6B3E7B9F0E01}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; Per-user install by default (no UAC elevation, no admin needed) so the
; installer can run unattended in CI/nightly testing; {autopf}/{autodesktop}
; above already adapt to a per-user location under this mode. Pass
; /ALLUSERS on the command line to opt into a per-machine install instead.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
OutputDir=..\installer-out
OutputBaseFilename=Seabass-Setup-{#MyAppVersion}-{#MyAppChannel}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile=..\src\gui\win\app_icon.ico
UninstallDisplayIcon={app}\seabass.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
; Executables
Source: "{#BuildDir}\seabass.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\seabass-cli.exe"; DestDir: "{app}"; Flags: ignoreversion

; Tells the deployed seabass.exe to resolve its QML/plugin imports
; relative to itself rather than to this machine's own Qt install -- see
; this file's header comment.
Source: "{#BuildDir}\qt.conf"; DestDir: "{app}"; Flags: ignoreversion

; Runtime DLLs (mingw runtime, Qt, ffmpeg codec graph, sqlcipher, etc. --
; the full closure deploy-windows.ps1 already resolved)
Source: "{#BuildDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion

; Qt plugin directories windeployqt populated
Source: "{#BuildDir}\iconengines\*"; DestDir: "{app}\iconengines"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\multimedia\*"; DestDir: "{app}\multimedia"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\qml\*"; DestDir: "{app}\qml"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\qmltooling\*"; DestDir: "{app}\qmltooling"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\generic\*"; DestDir: "{app}\generic"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\seabass.exe"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\seabass.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\seabass.exe"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
