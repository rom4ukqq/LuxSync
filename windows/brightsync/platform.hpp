#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <shlobj.h>
#include <string>
#include <iterator>
#include <cwchar>
#include <sstream>
#include "core.hpp"

namespace bs {
template <size_t N> std::optional<std::array<uint8_t, N>> randomBytes() {
    std::array<uint8_t, N> data{};
    if (BCryptGenRandom(nullptr, data.data(), static_cast<ULONG>(N), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0 ||
        data == std::array<uint8_t, N>{}) return {};
    return data;
}

class Hmac {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
public:
    Hmac() { BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG); }
    ~Hmac() { if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    Hmac(const Hmac&) = delete;
    Hmac& operator=(const Hmac&) = delete;
    bool sign(const Key& key, const uint8_t* data, Key& tag) const {
        if (!algorithm) return false;
        return BCryptHash(algorithm, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()),
                          const_cast<PUCHAR>(data), static_cast<ULONG>(signedSize), tag.data(),
                          static_cast<ULONG>(tag.size())) == 0;
    }
};

inline constexpr wchar_t registryPath[] = L"Software\\BrightSync";
inline constexpr wchar_t runPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

inline bool storeSecret(const Key& key) {
    DATA_BLOB input{static_cast<DWORD>(key.size()), const_cast<BYTE*>(key.data())}, protectedData{};
    if (!CryptProtectData(&input, L"BrightSync", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &protectedData))
        return false;
    const auto result = RegSetKeyValueW(HKEY_CURRENT_USER, registryPath, L"Pairing", REG_BINARY,
                                       protectedData.pbData, protectedData.cbData);
    LocalFree(protectedData.pbData);
    return result == ERROR_SUCCESS;
}

inline std::optional<Key> loadSecret() {
    std::array<BYTE, 4096> encrypted{};
    DWORD size = static_cast<DWORD>(encrypted.size());
    if (RegGetValueW(HKEY_CURRENT_USER, registryPath, L"Pairing", RRF_RT_REG_BINARY, nullptr,
                     encrypted.data(), &size) != ERROR_SUCCESS) return {};
    DATA_BLOB input{size, encrypted.data()}, clear{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &clear)) return {};
    std::optional<Key> key;
    if (clear.cbData == 32) {
        Key value{};
        std::copy_n(clear.pbData, 32, value.begin());
        if (value != Key{}) key = value;
        SecureZeroMemory(value.data(), value.size());
    }
    SecureZeroMemory(clear.pbData, clear.cbData);
    LocalFree(clear.pbData);
    return key;
}

inline bool forgetSecret() {
    auto r = RegDeleteKeyValueW(HKEY_CURRENT_USER, registryPath, L"Pairing");
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND;
}

inline bool autostart() {
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, runPath, L"BrightSync", RRF_RT_REG_SZ, nullptr, nullptr, &size) ==
        ERROR_SUCCESS;
}

inline bool setAutostart(bool enabled) {
    if (!enabled) {
        auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, runPath, L"BrightSync");
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
    }
    wchar_t path[32768]{};
    DWORD size = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!size || size >= std::size(path)) return false;
    std::wstring command = L"\"" + std::wstring(path, size) + L"\"";
    return RegSetKeyValueW(HKEY_CURRENT_USER, runPath, L"BrightSync", REG_SZ, command.c_str(),
                          static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

inline std::wstring iniPath() {
    PWSTR directory = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &directory))) return {};
    std::wstring path = std::wstring(directory) + L"\\BrightSync";
    CoTaskMemFree(directory);
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
    return path + L"\\settings.ini";
}

inline std::wstring readSetting(const std::wstring& path, const wchar_t* section,
                                const wchar_t* name, const wchar_t* fallback) {
    if (path.empty()) return fallback;
    wchar_t text[4096]{};
    auto n = GetPrivateProfileStringW(section, name, fallback, text, static_cast<DWORD>(std::size(text)), path.c_str());
    return n == std::size(text) - 1 ? fallback : text;
}

inline double number(const std::wstring& text, double fallback, double low, double high) {
    wchar_t* end = nullptr;
    auto value = std::wcstod(text.c_str(), &end);
    return end != text.c_str() && *end == L'\0' && std::isfinite(value) && value >= low && value <= high
        ? value : fallback;
}

inline Tuning readTuning(const std::wstring& path) {
    Tuning t;
    auto read = [&](const wchar_t* name, double def, double low, double high) {
        return number(readSetting(path, L"brightness", name, L""), def, low, high);
    };
    t.minimum = read(L"min", t.minimum, 0, 100);
    t.maximum = read(L"max", t.maximum, 0, 100);
    if (t.minimum > t.maximum) { t.minimum = 10; t.maximum = 100; }
    t.minimum = std::round(t.minimum);
    t.maximum = std::round(t.maximum);
    t.offset = read(L"offset", t.offset, -100, 100);
    t.alpha = read(L"smoothing", t.alpha, 0.01, 1);
    t.hysteresis = read(L"hysteresis", t.hysteresis, 1, 25);
    const auto oldStep = readSetting(path, L"brightness", L"max_step", L"");
    const auto oldInterval = readSetting(path, L"brightness", L"update_interval_ms", L"");
    double rampInterval = static_cast<double>(t.interval);
    if (!oldStep.empty() || !oldInterval.empty()) {
        const double step = number(oldStep, 6, 1, 25);
        const double interval = number(oldInterval, 2000, 500, 30000);
        rampInterval = std::clamp(interval / step, 250.0, 2000.0);
    }
    t.interval = static_cast<uint64_t>(read(L"ramp_interval_ms", rampInterval, 250, 2000));
    auto text = readSetting(path, L"brightness", L"curve", L"");
    Curve custom;
    bool valid = true;
    std::wstringstream items(text);
    std::wstring point;
    while (std::getline(items, point, L',')) {
        auto colon = point.find(L':');
        if (colon == std::wstring::npos || custom.size() == 32) { valid = false; break; }
        auto lux = number(point.substr(0, colon), -1, 0, 250000);
        auto percent = number(point.substr(colon + 1), -1, 0, 100);
        if (lux < 0 || percent < 0 || (!custom.empty() && lux <= custom.back().first)) { valid = false; break; }
        custom.emplace_back(lux, percent);
    }
    if (valid && custom.size() >= 2) t.curve = std::move(custom);
    return t;
}
}
