#pragma once
#include "platform.hpp"
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <memory>
#include <string>
#include <vector>

namespace bs {
struct DisplayInfo { std::wstring id, name; };
struct Physical {
    DisplayInfo info;
    HANDLE handle = nullptr;
    BrightnessRange brightness;
    bool vcp = false;
    int percent() const { return static_cast<int>(std::lround(brightness.percent(brightness.current))); }
    Physical() = default;
    Physical(const Physical&) = delete;
    ~Physical() { if (handle) DestroyPhysicalMonitor(handle); }

    bool read(DWORD& raw) {
        DWORD minimum = 0, maximum = 0;
        const bool ok = vcp ? GetVCPFeatureAndVCPFeatureReply(handle, 0x10, nullptr, &raw, &maximum) != FALSE :
            GetMonitorBrightness(handle, &minimum, &raw, &maximum) != FALSE;
        if (!ok || maximum <= minimum || raw < minimum || raw > maximum) return false;
        brightness = {minimum, maximum, raw};
        return true;
    }

    bool write(uint32_t target) {
        if (!handle) { brightness.current = target; return true; }
        const double requested = brightness.percent(target);
        auto apply = [&](DWORD raw) {
            if (raw == brightness.current) return true;
            const bool ok = vcp ? SetVCPFeature(handle, 0x10, raw) != FALSE :
                SetMonitorBrightness(handle, raw) != FALSE;
            DWORD actual = 0;
            if (!ok) return false;
            if (read(actual) && actual == raw) return true;
            Sleep(80);
            return read(actual) && actual == raw;
        };
        if (apply(target)) return true;
        if (!vcp) return false;
        vcp = false;
        DWORD actual = 0;
        return read(actual) && apply(brightness.level(requested));
    }
};

class Displays {
    static BOOL CALLBACK enumerate(HMONITOR logical, HDC, LPRECT, LPARAM data) {
        static_cast<Displays*>(reinterpret_cast<void*>(data))->discover(logical);
        return TRUE;
    }
    void discover(HMONITOR logical) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(logical, reinterpret_cast<MONITORINFO*>(&mi))) return;
        // Do not guess a mapping for cloned or tiled displays. Identity is the
        // monitor device interface path, not its friendly name or screen order.
        std::vector<DisplayInfo> interfaces;
        for (DWORD i = 0; i < 32; ++i) {
            DISPLAY_DEVICEW device{};
            device.cb = sizeof(device);
            if (!EnumDisplayDevicesW(mi.szDevice, i, &device, EDD_GET_DEVICE_INTERFACE_NAME)) break;
            if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) && device.DeviceID[0])
                interfaces.push_back({device.DeviceID, device.DeviceString});
        }
        if (interfaces.size() != 1) return;
        DWORD count = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(logical, &count) || count != 1) return;
        PHYSICAL_MONITOR native{};
        auto p = std::make_unique<Physical>();
        if (!GetPhysicalMonitorsFromHMONITOR(logical, 1, &native)) return;
        p->handle = native.hPhysicalMonitor;
        p->info = std::move(interfaces.front());
        if (native.szPhysicalMonitorDescription[0]) {
            const std::wstring description(native.szPhysicalMonitorDescription);
            if (p->info.name.empty() || p->info.name == L"Generic PnP Monitor" ||
                description != L"Generic PnP Monitor") p->info.name = description;
        }
        if (p->info.name.empty()) p->info.name = L"DDC/CI display";
        DWORD raw = 0;
        p->vcp = true;
        if (!p->read(raw)) {
            p->vcp = false;
            if (!p->read(raw)) return;
        }
        all.push_back(std::move(p));
    }
public:
    std::vector<std::unique_ptr<Physical>> all;
    void scan(bool simulate, int simulatedPercent) {
        all.clear();
        if (simulate) {
            auto p = std::make_unique<Physical>();
            p->info = {L"mock", L"Simulated monitor"};
            p->brightness.current = static_cast<uint32_t>(simulatedPercent);
            all.push_back(std::move(p));
        } else EnumDisplayMonitors(nullptr, nullptr, enumerate, reinterpret_cast<LPARAM>(this));
    }
    Physical* find(const std::wstring& id) const {
        if (id.empty()) return nullptr;
        for (const auto& p : all) if (_wcsicmp(id.c_str(), p->info.id.c_str()) == 0) return p.get();
        return nullptr;
    }
};
}
