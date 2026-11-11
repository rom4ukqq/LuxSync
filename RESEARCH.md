# BrightSync research

All four repositories were cloned and their source inspected before implementation. BrightSync uses its own implementation and incorporates no source from these projects.

| Reference | Source findings | License at inspected revision |
| --- | --- | --- |
| [OpenDisplay](https://github.com/aquitaine/OpenDisplay/tree/f63703b) | Experimental Lifecycle Provider's `AmbientLight.swift` resolves IOKit HID event symbols dynamically. It matches vendor usage page `0xff00`, usage 4, and reads the ALS event's level field as lux. No helper app is necessary. The source applies no additional calibration. The main app uses an AppKit status item and `SMAppService`. | GPL v3 text in LICENSE; no code incorporated. |
| [lonar](https://github.com/Eimen2018/lonar/tree/5ee6bd9) | `BuiltinBrightnessMonitor.swift` locates the internal display and reads `DisplayServicesGetBrightness` via `PrivateAPIs.swift`. Notifications and watchdog polling observe the internal panel level, which can be influenced by ambient light but cannot be converted reliably to lux. `AppSettings.swift` uses `SMAppService`. | MIT; bundled AppleSiliconDDC has a separate notice. |
| [Lumos DDC](https://github.com/sfortis/lumos_ddc/tree/3b0c973) | `monitor.c` probes actual brightness through Dxva2 and owns physical handles. `lumos.c` uses `Shell_NotifyIconW` and schedules rescans on display changes, session unlock and power resume. Native Win32 is a good fit for a small single executable. | README explicitly reserves all rights; no source reused. |
| [Monitorian](https://github.com/emoacht/Monitorian/tree/4905e84) | `MonitorConfiguration.cs` supports high-level brightness and low-level VCP luminance (0x10), handles transient failures, and reads hardware-specific min/max. Its comments note that high-level writes may report success even if the monitor ignored them. `DdcMonitorItem.cs` owns physical handles. | MIT. |

BrightSync reads the ALS service directly and reports its device-provided lux without inferring anything from panel brightness. The isolated SPI adapter resolves symbols dynamically and reports unavailable on failure. Apple can change or restrict this interface without notice. Sensor orientation, occlusion, and device calibration affect readings; the brightness curve is user configurable independently.

On Windows we use C++17 with Win32, Winsock, CNG, DPAPI and [Dxva2](https://learn.microsoft.com/en-us/windows/win32/monitor/using-the-low-level-monitor-configuration-functions). This enables native MSVC builds and cross builds with MinGW from this macOS host, with no third-party runtime. BrightSync prefers VCP 0x10, falls back to the high-level monitor API, confirms hardware writes by reading back, and releases/reacquires handles after failures or topology changes.
