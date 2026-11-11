# LuxSync

Download BrightSync for Mac and Windows from [GitHub Releases](https://github.com/rom4ukqq/LuxSync/releases).

BrightSync uses an Apple Silicon MacBook's ambient light sensor to adjust the hardware brightness of a DDC/CI monitor on a nearby Windows 11 PC. It runs in the Mac menu bar and Windows tray. There are no accounts, background services, web UI, or external runtimes. The apps communicate directly on your LAN.

## Set up

1. Extract `BrightSync-Windows-x64.zip` to a permanent folder and run `BrightSync.exe`. Open its icon in the notification area (check the overflow arrow). Choose **Select Monitor** and select the display you want to control. The app waits for you to select one before making changes.
2. In the Windows tray menu, choose **Show Pairing Code**. The eight-character code expires after three minutes.
3. Extract `BrightSync-macOS-arm64.zip`, move `BrightSync.app` to Applications, and open it. Click the BrightSync sparkle in the menu bar, choose **Pair with Windows**, and enter the code. If macOS asks, allow [Local Network access](https://support.apple.com/guide/mac-help/control-access-to-your-local-network-on-mac-mchla4f49138/mac).
4. Check that both menus show **Connected** and display a lux reading. Keep the MacBook lid open. Enable **Start at Login** and **Start with Windows** if you want either app to launch automatically. Both startup options default to off.

If the apps were already paired before this update, they can continue using the saved pairing. Starting a new pairing replaces it after the Mac enters the correct code. Use the same updated release on both computers to pair by code.

Both computers need IPv4 connectivity on the same LAN. If broadcast discovery does not work, run `ipconfig` on Windows and enter the PC's LAN IPv4 address under **Windows IP Address** on the Mac. Leave the field blank to return to automatic discovery. Discovery handles DHCP address changes; update a manually configured address if it changes. Guest Wi-Fi or client isolation may block peer traffic even with manual addressing.

If Windows Firewall blocks discovery or manual connection, allow the app on your **Private** network. Alternatively, use an elevated PowerShell in the installed application's directory to scope the inbound rule to this executable, UDP 47632, and the local subnet:

```powershell
New-NetFirewallRule -DisplayName 'BrightSync (private LAN)' -Direction Inbound `
  -Action Allow -Protocol UDP -LocalPort 47632 -Program (Resolve-Path .\BrightSync.exe).Path `
  -Profile Private -RemoteAddress LocalSubnet
```

For the MSI MAG 274QRFW, start with a direct DisplayPort or HDMI connection and verify that brightness can be changed in the monitor's own menu. If DDC/CI is available as a setting, enable it. If the monitor is missing or ignores writes, try SDR and a picture mode that allows brightness changes, then select **Rescan Monitors**. Some docks, KVMs, and display modes block DDC/CI. BrightSync skips displays that cannot be mapped unambiguously to a physical monitor, such as some mirrored or tiled setups. If Windows assigns a new monitor identity after you move the cable to a different port, select the monitor again.

## Calibration and storage

The Windows receiver interpolates between the curve points, smooths lux with an exponential moving average, and applies the brightness offset and limits. It ignores small changes and ramps toward the target about one percentage point at a time, using the nearest available monitor levels. Steps are spaced 300 ms apart by default. In the Windows tray, use **Fallback Brightness** to set the level used after 12 seconds without a reading. The default is 40%. The same ramp applies to this level, within the configured brightness limits. If the monitor is unavailable, brightness control resumes when it reconnects. Disabling **Auto Brightness** pauses both modes.

On Windows, quit the receiver before editing `%LOCALAPPDATA%\BrightSync\settings.ini`, then restart it. Create the file if it doesn't exist. Omitted or invalid calibration values use the defaults below. Curve lux values must be strictly increasing. The ramp interval can be set from 250 to 2000 ms. Older `max_step` and `update_interval_ms` values are used to estimate the equivalent ramp speed unless you set `ramp_interval_ms`.

```ini
[general]
enabled=1

[brightness]
min=10
max=100
offset=0
smoothing=0.25
hysteresis=3
ramp_interval_ms=300
fallback_percent=40
curve=0:10,10:15,50:25,150:40,300:55,600:75,1000:100
```

The Mac uses per-user preferences (`com.brightsync.sender`). To change the default three-second polling interval, quit the app, run `defaults write com.brightsync.sender pollSeconds -float 3` with a value from 1 to 8, then reopen it. The app reports **Unavailable** if the sensor or the undocumented IOKit interface cannot be read.

The code is used by BoringSSL’s SPAKE2 exchange to establish a separate 256-bit key. The key is stored in macOS Keychain and protected by Windows DPAPI for the current user. The [protocol](protocol/README.md) authenticates each reading and rejects replayed samples. Ambient readings are not encrypted on the LAN.

## Build and test

- **macOS 13 or newer, Apple Silicon:** Install the Xcode tools, CMake, and Ninja, then run `scripts/build-macos.sh`.
- **Windows 11 x64:** Install the Visual Studio C++ build tools, a Windows SDK, Git, CMake, and Ninja, then run `scripts/build-windows.ps1`. Set `BRIGHTSYNC_RUN_TESTS=1` to also run the C++ tests and UDP simulation (Python 3 required for tests).
- **Cross build on macOS:** Install MinGW-w64, CMake, and Ninja (e.g. `brew install mingw-w64 cmake ninja`) and run `scripts/build-windows-cross.sh`.
- ZIPs are written to `dist/`. The [build workflow](.github/workflows/build.yml) builds both ZIPs and runs simulations on GitHub-hosted macOS and Windows runners when the repo is pushed or the workflow is dispatched.
- For an end-to-end simulation on two machines, quit the normal apps and start `BrightSync.exe --simulate` on Windows and `BrightSync.app/Contents/MacOS/BrightSync --simulate 145` on the Mac. The Windows app uses a mock monitor starting at 50%. Existing pairing works in simulation. A new pairing on the Windows simulation lasts until it exits. Re-pair in normal mode if you change the pairing during simulation. With the default curve the mock should approach 39%. Try another lux value by restarting the Mac simulation.
- For a local sensor check, run `BrightSync.app/Contents/MacOS/BrightSync --read-sensor` with the lid open. Change the light near the screen and compare readings. On real hardware, also check brightness in the MSI on-screen menu, monitor power cycling, Mac sleep/wake, and both login options.

The macOS ZIP is ad hoc signed by default and is not notarized. Gatekeeper may block downloaded copies, and macOS may not reliably retain Local Network permission across ad hoc builds. For distribution, set `MACOS_SIGN_IDENTITY` to your Developer ID Application identity, sign with the hardened runtime, and [notarize and staple the app](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution). See [Apple's notes on Local Network identity](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy). The Windows EXE is unsigned and may trigger SmartScreen.

See [RESEARCH.md](RESEARCH.md) for the upstream analysis and API choices. BrightSync is MIT licensed.
