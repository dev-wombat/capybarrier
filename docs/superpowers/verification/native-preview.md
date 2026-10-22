# CapyBarrier native preview

Experimental build of the current fork, not the completed CapyBarrier roadmap.
The GUI still displays the upstream Barrier name/version. Use matching preview
packages on both machines; upstream Barrier protocol 1 clients cannot connect.
Some releases contain only the macOS package; download only the platform assets
actually attached to that release. Cross-release device interoperability and
two-physical-machine acceptance are not established by a successful CI run.

## Downloads and first run

- Windows 11 x64: extract **all** of `CapyBarrier-Windows-x64.zip`, then run
  the foreground daemon before `barrier.exe` (see below). Keep its DLLs and
  plugins beside it. No build tools are needed.
- Apple Silicon (M1/M2/M3), macOS 14 or newer: extract `CapyBarrier-macOS-arm64.zip`, move
  `CapyBarrier.app` to `/Applications`, then open it. It is ad-hoc signed, not
  Developer ID signed or notarized. If macOS blocks it, verify the source and
  checksum before using Privacy & Security's per-app Open Anyway control.
  Do not disable Gatekeeper globally. Allow Accessibility and Input Monitoring
  for the application when required, then restart it.
  Check `MACOS_REQUIREMENTS.txt` for the exact minimum required by the bundled
  libraries; it is also recorded in the app's Info.plist.
- Stop other Barrier/Input Leap/Synergy instances before testing. Existing
  Barrier settings/profile directories may be shared; back them up first.
- The machine with the keyboard/mouse is the server; the other is the client.
  Start with manual server IP entry on the same trusted LAN. Bonjour discovery
  on Windows needs a running Bonjour service; the included client DLL alone
  does not install that service. Manual IP avoids this dependency.
- Keep TLS enabled and compare/approve the peer certificate fingerprint.
  Allow the application on the private-network firewall only, not the internet.

### Windows ZIP startup (no service installation)

The Windows GUI needs the included `barrierd.exe` for IPC, even when no Windows
service is installed. Stop any existing Barrier service/instances first; do not
replace another installation's service. Extract to a folder you can write to.
Open PowerShell in that folder and run:

```powershell
$capyDaemon = Start-Process -FilePath .\barrierd.exe -ArgumentList '-f' -PassThru
Start-Process -FilePath .\barrier.exe
```

Keep the daemon running while sharing input. After clicking Stop in the GUI and
closing it, stop only the daemon started above with `Stop-Process -Id $capyDaemon.Id`
in that same PowerShell session. This preview does not install a service, start
at login, or promise input on the Windows login/UAC secure desktop.

## What to test

1. Connect and move the pointer between screens; type and scroll on the peer.
2. Copy/paste plain text in both directions.
3. Disconnect/reconnect the network and check input resumes without restarting.

Image clipboard improvements, native file/folder drag-and-drop, the new calm
status-first GUI, and fully automatic file-transfer replay are **unfinished**.
Do not use file transfer for important data or interpret a successful build as
proof of these features. Two-physical-machine acceptance remains pending.

## Build and verification

`.github/workflows/native-preview.yml` builds on standard Windows/macOS runners
in this public repository. It runs the existing complete CTest suite, packages
the GUI/client/server and runtime dependencies, checks CLI startup and binary
architecture and Windows foreground-daemon IPC startup, and uploads ZIPs plus
per-platform SHA-256 lists to a draft.
The draft becomes a pre-release only after every selected platform succeeds.
The `agent/macos-preview` branch selects Mac only, running all Mac test suites
plus repeated wakeup/TCP regressions; it does not rebuild or certify Windows.
The `agent/release-preview` branch still requires both platforms to succeed.
The ZIPs contain `SOURCE_COMMIT.txt`, this guide and license notices; the exact
source and dependency recipes remain available at the release's Git tag.

macOS uses Homebrew Qt 5/OpenSSL 3; Windows uses Qt 5.15.2 MSVC and a pinned
vcpkg revision for OpenSSL 3/Bonjour. Windows Qt is a legacy runtime; this is a
trusted-LAN trial, not a security-reviewed production distribution.
