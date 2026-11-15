; しずまる (formerly GmS De-Noise) Windows installer (Inno Setup 6).
; Installs the VST3 into the standard folder every Windows DAW scans
; (C:\Program Files\Common Files\VST3) and adds an uninstall entry under
; Settings > Apps. Built by the GitHub Actions workflow; the version and the
; bundle path come from the command line (/DAppVersion=... /DBundleDir=...).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef BundleDir
  #define BundleDir "..\..\build\GmSDeNoise_artefacts\Release\VST3\Shizumaru.vst3"
#endif

[Setup]
AppId={{FE39CA3E-8AC9-47E3-B93B-17BDECEC7CBA}
AppName=しずまる (Shizumaru)
AppVersion={#AppVersion}
AppPublisher=Genomersive Studio
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
DirExistsWarning=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=Shizumaru-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Shizumaru (VST3)
CloseApplications=no

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#BundleDir}\*"; DestDir: "{commoncf64}\VST3\Shizumaru.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "README_ja.txt"; DestDir: "{commoncf64}\VST3\Shizumaru.vst3"; DestName: "Shizumaru README_ja.txt"; Flags: ignoreversion
Source: "..\ThirdPartyNotices.txt"; DestDir: "{commoncf64}\VST3\Shizumaru.vst3"; DestName: "Shizumaru ThirdPartyNotices.txt"; Flags: ignoreversion

[InstallDelete]
; The same plug-in under its old name: two copies with one ID confuse hosts.
Type: filesandordirs; Name: "{commoncf64}\VST3\GmS De-Noise.vst3"

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Shizumaru.vst3"

[Messages]
japanese.FinishedLabel=しずまる をインストールしました（以前の GmS De-Noise は置き換えました）。DAW を開き直して、プラグインを再スキャンしてください。
english.FinishedLabel=Shizumaru is installed (it replaces GmS De-Noise). Restart your DAW and rescan plug-ins.
