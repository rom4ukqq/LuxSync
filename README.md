# LuxSync

Download BrightSync for Mac and Windows from [GitHub Releases](https://github.com/rom4ukqq/LuxSync/releases).

BrightSync uses the brightness percentage of an Apple Silicon MacBook’s built-in display to control the hardware brightness of a DDC/CI monitor on a nearby Windows 11 PC. It runs in the Mac menu bar and Windows tray. There are no accounts, background services, web UI, or external runtimes. The apps communicate directly on your LAN.

## Set up

1. Extract `BrightSync-Windows-x64.zip` to a permanent folder and run `BrightSync.exe`. Open its icon in the notification area (check the overflow arrow). Choose **Select Monitor** and select the display you want to control. The app waits for you to select one before making changes.
2. In the Windows tray menu, choose **Show Pairing Code**. The eight-character code expires after three minutes.
3. Extract `BrightSync-macOS-arm64.zip`, move `BrightSync.app` to Applications, and open it. Click the BrightSync sparkle in the menu bar, choose **Pair with Windows**, and enter the code. If macOS asks, allow [Local Network access](https://support.apple.com/guide/mac-help/control-access-to-your-local-network-on-mac-mchla4f49138/mac).
4. Check that both menus show **Connected** and display the Mac screen brightness percentage. Keep the MacBook lid open. Enable **Start at Login** and **Start with Windows** if you want either app to launch automatically. Both startup options default to off.

If the apps were already paired before this update, they can continue using the saved pairing. Starting a new pairing replaces it after the Mac enters the correct code. Update both computers to version 1.4.0 or later for percentage-based sync.

Both computers need IPv4 connectivity on the same LAN. If broadcast discovery does not work, run `ipconfig` on Windows and enter the PC's LAN IPv4 address under **Windows IP Address** on the Mac. Leave the field blank to return to automatic discovery. Discovery handles DHCP address changes; update a manually configured address if it changes. Guest Wi-Fi or client isolation may block peer traffic even with manual addressing.

If Windows Firewall blocks discovery or manual connection, allow the app on your **Private** network. Alternatively, use an elevated PowerShell in the installed application's directory to scope the inbound rule to this executable, UDP 47632, and the local subnet:

```powershell
New-NetFirewallRule -DisplayName 'BrightSync (private LAN)' -Direction Inbound `
  -Action Allow -Protocol UDP -LocalPort 47632 -Program (Resolve-Path .\BrightSync.exe).Path `
  -Profile Private -RemoteAddress LocalSubnet
```

For the MSI MAG 274QRFW, start with a direct DisplayPort or HDMI connection and verify that brightness can be changed in the monitor's own menu. If DDC/CI is available as a setting, enable it. If the monitor is missing or ignores writes, try SDR and a picture mode that allows brightness changes, then select **Rescan Monitors**. Some docks, KVMs, and display modes block DDC/CI. BrightSync skips displays that cannot be mapped unambiguously to a physical monitor, such as some mirrored or tiled setups. If Windows assigns a new monitor identity after you move the cable to a different port, select the monitor again.

## Calibration and storage

The Windows receiver uses the Mac display percentage as its target, with the configured offset and limits. It ramps about one percentage point at a time using the nearest available monitor levels. You can set an optional moving average and a threshold for small changes. Steps are spaced 300 ms apart by default. In the Windows tray, use **Fallback Brightness** to set the level used after 12 seconds without a reading. The default is 40%. The same ramp applies to this level, within the configured brightness limits. If the monitor is unavailable, brightness control resumes when it reconnects. Disabling **Auto Brightness** pauses both modes.

On Windows, quit the receiver before editing `%LOCALAPPDATA%\BrightSync\settings.ini`, then restart it. Create the file if it doesn't exist. Omitted or invalid settings use the defaults below. The ramp interval can be set from 250 to 2000 ms. Older `max_step` and `update_interval_ms` values are used to estimate the equivalent ramp speed unless you set `ramp_interval_ms`.

```ini
[general]
enabled=1

[brightness]
min=10
max=100
offset=0
smoothing=1
hysteresis=1
ramp_interval_ms=300
fallback_percent=40
```

The Mac reads its built-in display once a second by default. To change the interval, quit the app and use `defaults write com.brightsync.sender pollSeconds -float 1` with a value from 1 to 8. It reconnects after a wake or network change and retries with a fresh connection if Windows stops responding. The menu shows **Unavailable** when the built-in screen is asleep or its brightness cannot be read. Reading the level uses macOS’s private DisplayServices framework.

The code is used by BoringSSL’s SPAKE2 exchange to establish a separate 256-bit key. The key is stored in macOS Keychain and protected by Windows DPAPI for the current user. The [protocol](protocol/README.md) authenticates each reading and rejects replayed samples. The LAN packets carry authenticated brightness values in clear text.

## Automatic updates

If you already have version 1.3.0 with update access configured, use **Update Now** on both machines. For a fresh installation, download the current release ZIPs. On Windows, keep `BrightSyncUpdater.exe` next to `BrightSync.exe`. Install the app in a folder your account can write to. BrightSync runs the updater with your account permissions.

1. In GitHub, create a [fine-grained personal access token](https://github.com/settings/personal-access-tokens/new). Select only `rom4ukqq/LuxSync` and grant **Contents → Read-only**.
2. Open **GitHub Update Token** in BrightSync and paste the token on each computer. It is stored in macOS Keychain or Windows Credential Manager.
3. Enable **Automatic Updates**. BrightSync checks about every six hours while it runs. When a verified release is available, it installs the update and restarts the app. Use **Update Now** to check and install immediately, or leave automatic updates off and use the manual action.

The updater verifies the release manifest with the Ed25519 public key built into BrightSync. The manifest identifies the release version and contains the size and SHA-256 digest of each platform ZIP. The app and its updater are replaced together. Pairing and brightness settings live separately. If installation fails during replacement, the updater restores the previous files and tries to reopen the app. It records errors in `~/Library/Caches/BrightSync/update-error.log` on macOS or `%LOCALAPPDATA%\BrightSync\update-error.log` on Windows.

## Build and test

- **macOS 13 or newer, Apple Silicon:** Install the Xcode tools, CMake, Ninja, and Go, then run `scripts/build-macos.sh`.
- **Windows 11 x64:** Install the Visual Studio C++ build tools, a Windows SDK, Git, CMake, Ninja, and Go, then run `scripts/build-windows.ps1`. Set `BRIGHTSYNC_RUN_TESTS=1` to also run the C++ tests and UDP simulation (Python 3 required for tests).
- **Cross build on macOS:** Install MinGW-w64, CMake, Ninja, and Go (e.g. `brew install mingw-w64 cmake ninja go`) and run `scripts/build-windows-cross.sh`.
- ZIPs are written to `dist/`. The [build workflow](.github/workflows/build.yml) builds both ZIPs and runs simulations on GitHub-hosted macOS and Windows runners when the repo is pushed or the workflow is dispatched.
- For an end-to-end simulation on two machines, quit the normal apps and start `BrightSync.exe --simulate` on Windows and `BrightSync.app/Contents/MacOS/BrightSync --simulate 39` on the Mac. The Windows app uses a mock monitor starting at 50%. Existing pairing works in simulation. A new pairing on the Windows simulation lasts until it exits. Re-pair in normal mode if you change the pairing during simulation. The mock should approach 39%. Try another percentage by restarting the Mac simulation.
- For a local display check, run `BrightSync.app/Contents/MacOS/BrightSync --read-brightness` with the lid open. Change the Mac brightness in Control Center and compare the percentage. On real hardware, also check brightness in the MSI on-screen menu, monitor power cycling, Mac sleep/wake, and both login options.

The macOS ZIP is ad hoc signed by default and is not notarized. Gatekeeper may block downloaded copies, and macOS may not reliably retain Local Network permission across ad hoc builds. For distribution, set `MACOS_SIGN_IDENTITY` to your Developer ID Application identity, sign with the hardened runtime, and [notarize and staple the app](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution). See [Apple's notes on Local Network identity](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy). The Windows EXE is unsigned and may trigger SmartScreen.

See [RESEARCH.md](RESEARCH.md) for the upstream analysis and API choices. BrightSync is MIT licensed.

## Sign releases for the updater

Set `VERSION` and the platform version metadata, build both release ZIPs, then run `scripts/sign-release.sh <private-key-file> <zip-directory>`. Upload both ZIPs, `BrightSync-update.json`, and `BrightSync-update.sig` together in a release tagged `v<version>`. The signer checks the private key against the public key embedded in the updater. Keep the private key outside the repository and retain a secure backup so existing installations can verify future releases.
