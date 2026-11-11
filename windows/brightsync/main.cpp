#include "receiver.hpp"
#include "updates.hpp"
#include <shellapi.h>
#include <dbt.h>
#include <wtsapi32.h>
#include <memory>
#include <ctime>

namespace {
using namespace bs;
constexpr UINT trayMessage = WM_APP + 1;
enum : UINT { toggle = 1, startup, pair, forget, rescan, quit, fallback, autoUpdate, updateNow, updateToken, firstDisplay = 100 };
UINT taskbarCreated;
constexpr GUID consoleDisplayState{0x6fe69556, 0x704a, 0x47a0, {0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}};

std::optional<Key> simulationKey() {
    wchar_t chars[65]{};
    if (GetEnvironmentVariableW(L"BRIGHTSYNC_TEST_KEY", chars, static_cast<DWORD>(std::size(chars))) != 64)
        return {};
    auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    Key result{};
    for (size_t i = 0; i < result.size(); ++i) {
        int high = hex(chars[i * 2]), low = hex(chars[i * 2 + 1]);
        if (high < 0 || low < 0) return {};
        result[i] = static_cast<uint8_t>(16 * high + low);
    }
    if (result == Key{}) return {};
    return result;
}

HICON appIcon(int width, int height) {
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101),
                                        IMAGE_ICON, width, height, LR_SHARED));
}

void setDialogIcon(HWND dialog) {
    SendMessageW(dialog, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(appIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON))));
}

struct FallbackDialog { int value; };

INT_PTR CALLBACK fallbackDialog(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    constexpr int fieldId = 105;
    if (message == WM_INITDIALOG) {
        auto* state = reinterpret_cast<FallbackDialog*>(l);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        setDialogIcon(dialog);
        SetDlgItemInt(dialog, fieldId, static_cast<UINT>(state->value), FALSE);
        SendDlgItemMessageW(dialog, fieldId, EM_SETLIMITTEXT, 3, 0);
        SetForegroundWindow(dialog);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    if (LOWORD(w) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (LOWORD(w) != IDOK) return FALSE;
    wchar_t input[4]{};
    const UINT length = GetDlgItemTextW(dialog, fieldId, input, ARRAYSIZE(input));
    const bool digits = length && std::all_of(input, input + length, [](wchar_t digit) {
        return digit >= L'0' && digit <= L'9';
    });
    BOOL parsed = FALSE;
    const UINT value = GetDlgItemInt(dialog, fieldId, &parsed, FALSE);
    if (!digits || !parsed || value > 100) {
        MessageBoxW(dialog, L"Enter a whole number from 0 to 100.", L"BrightSync", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, fieldId));
        return TRUE;
    }
    auto* state = reinterpret_cast<FallbackDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    state->value = static_cast<int>(value);
    EndDialog(dialog, IDOK);
    return TRUE;
}

struct PairDialog {
    const wchar_t* code;
    HFONT font = nullptr;
};

INT_PTR CALLBACK pairingDialog(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_INITDIALOG) {
        auto* content = reinterpret_cast<PairDialog*>(l);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(content));
        const int size = MulDiv(18, static_cast<int>(GetDpiForWindow(dialog)), 72);
        content->font = CreateFontW(-size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                    FIXED_PITCH, L"Consolas");
        HWND label = GetDlgItem(dialog, 103);
        if (content->font) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(content->font), TRUE);
        SetWindowTextW(label, content->code);
        setDialogIcon(dialog);
        SetForegroundWindow(dialog);
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL))) {
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    return FALSE;
}

struct App {
    const bool simulate;
    const std::wstring path;
    Control control;
    Updates updates;
    bool menuOpen = false;
    std::unique_ptr<Receiver> receiver;
    HPOWERNOTIFY displayPower = nullptr;
    bool trayAdded = false;

    explicit App(bool simulated) : simulate(simulated), path(simulated ? L"" : iniPath()) {
        control.key = simulate ? simulationKey() : std::nullopt;
        if (!control.key) control.key = loadSecret();
        control.enabled = readSetting(path, L"general", L"enabled", L"1") != L"0";
        control.fallbackPercent = static_cast<int>(std::lround(number(
            readSetting(path, L"brightness", L"fallback_percent", L""), defaultFallbackBrightness, 0, 100)));
        updates.automatic = readSetting(path, L"updates", L"automatic", L"0") == L"1";
        const auto lastCheck = std::wcstoull(readSetting(path, L"updates", L"last_check", L"0").c_str(), nullptr, 10);
        const auto now = static_cast<uint64_t>(std::time(nullptr));
        if (lastCheck <= now) updates.restoreLastCheck(now - lastCheck);
        control.monitor = simulate ? L"mock" : readSetting(path, L"general", L"monitor", L"");
    }

    void error(HWND hwnd, const wchar_t* text) {
        MessageBoxW(hwnd, text, L"BrightSync", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
    void publish() { if (receiver) receiver->update(control); }
    void notify(HWND hwnd, DWORD action) {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = hwnd;
        icon.uID = 1;
        if (action == NIM_ADD) {
            icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            icon.uCallbackMessage = trayMessage;
            icon.hIcon = appIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
            if (!icon.hIcon) icon.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
            lstrcpynW(icon.szTip, simulate ? L"BrightSync - Simulation" : L"BrightSync - Windows Receiver",
                      ARRAYSIZE(icon.szTip));
            trayAdded = Shell_NotifyIconW(action, &icon) != FALSE;
        } else {
            Shell_NotifyIconW(action, &icon);
            trayAdded = false;
        }
    }

    void showPairing(HWND hwnd) {
        const auto pin = receiver->beginPairing();
        if (!pin) { error(hwnd, L"Could not generate a pairing code."); return; }
        std::wstring code(pin->begin(), pin->end());
        code.insert(4, 1, L'-');
        PairDialog content{code.c_str()};
        const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(102), hwnd,
                                            pairingDialog, reinterpret_cast<LPARAM>(&content));
        if (content.font) DeleteObject(content.font);
        if (result == -1) MessageBoxW(hwnd, code.c_str(), L"BrightSync Pairing", MB_OK | MB_SETFOREGROUND);
    }

    void editFallback(HWND hwnd) {
        FallbackDialog setting{control.fallbackPercent};
        const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(104), hwnd,
                                            fallbackDialog, reinterpret_cast<LPARAM>(&setting));
        if (result == -1) { error(hwnd, L"Could not open fallback brightness settings."); return; }
        if (result != IDOK) return;
        const auto value = std::to_wstring(setting.value);
        if (!simulate && !save(L"fallback_percent", value.c_str(), L"brightness")) {
            error(hwnd, L"Could not save fallback brightness.");
            return;
        }
        control.fallbackPercent = setting.value;
        publish();
    }

    bool configureUpdates(HWND hwnd) {
        UpdateTokenDialog setting;
        const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(106), hwnd,
                                           updateTokenDialog, reinterpret_cast<LPARAM>(&setting));
        if (result == -1) error(hwnd, L"Could not open the update token dialog.");
        if (result == removeToken) {
            if (!removeUpdateToken()) error(hwnd, L"Could not remove the token.");
            else {
                updates.automatic = false;
                armUpdateTimer(hwnd);
                if (!save(L"automatic", L"0", L"updates")) error(hwnd, L"Could not save update settings.");
            }
        }
        if (result != IDOK) return false;
        const bool saved = storeUpdateToken(setting.token);
        SecureZeroMemory(setting.token.data(), setting.token.size());
        if (!saved) { error(hwnd, L"Could not save the token in Windows Credential Manager."); return false; }
        updates.checkSoon();
        return true;
    }

    std::string updateAccess(HWND hwnd) {
        auto token = loadUpdateToken();
        if (token.empty() && configureUpdates(hwnd)) token = loadUpdateToken();
        return token;
    }

    void toggleUpdates(HWND hwnd) {
        const bool enable = !updates.automatic;
        if (enable) {
            auto token = updateAccess(hwnd);
            if (token.empty()) return;
            SecureZeroMemory(token.data(), token.size());
        }
        if (!save(L"automatic", enable ? L"1" : L"0", L"updates")) {
            error(hwnd, L"Could not save update settings.");
            return;
        }
        updates.automatic = enable;
        if (enable) updates.checkSoon();
        armUpdateTimer(hwnd);
    }

    void installUpdate(HWND hwnd) {
        auto token = updateAccess(hwnd);
        if (token.empty()) return;
        const auto choice = MessageBoxW(hwnd, L"Check for updates and restart BrightSync if an update is available?",
            L"Update BrightSync", MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
        if (choice == IDOK && !startUpdate(hwnd, std::move(token), true))
            error(hwnd, L"Could not start the updater. Extract both executables from the Windows ZIP.");
        if (!token.empty()) SecureZeroMemory(token.data(), token.size());
    }

    void armUpdateTimer(HWND hwnd) {
        if (updates.busy()) SetTimer(hwnd, 2, 1000, nullptr);
        else if (updates.automatic) SetTimer(hwnd, 2, 15000, nullptr);
        else KillTimer(hwnd, 2);
    }

    bool startUpdate(HWND hwnd, std::string token, bool manual) {
        if (!updates.start(std::move(token), manual)) return false;
        const auto now = std::to_wstring(std::time(nullptr));
        save(L"last_check", now.c_str(), L"updates");
        armUpdateTimer(hwnd);
        return true;
    }

    void updateTick(HWND hwnd) {
        if (const auto result = updates.poll()) {
            armUpdateTimer(hwnd);
            if (result->code == 10) {
                if (menuOpen) EndMenu();
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return;
            }
            if (result->manual) {
                if (result->code == 0)
                    MessageBoxW(hwnd, L"BrightSync is up to date.", L"BrightSync", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
                else if (result->code == 3)
                    error(hwnd, L"GitHub denied access. Check the token's expiration and repository permissions.");
                else
                    error(hwnd, L"Update failed. See %LOCALAPPDATA%\\BrightSync\\update-error.log for details.");
            }
        }
        if (updates.due() && !menuOpen) startUpdate(hwnd, loadUpdateToken(), false);
    }

    void menu(HWND hwnd) {
        const auto state = receiver->snapshot();
        HMENU root = CreatePopupMenu();
        if (!root) return;
        auto label = [&](const std::wstring& text) { AppendMenuW(root, MF_STRING | MF_GRAYED, 0, text.c_str()); };
        auto separator = [&] { AppendMenuW(root, MF_SEPARATOR, 0, nullptr); };
        label(simulate ? L"BrightSync (simulation)" : L"BrightSync - Windows Receiver");
        separator();
        std::wstring connection = !state.listening ? L"UDP port 47632 unavailable" :
            !state.paired ? L"Not paired" : state.connected ? L"Connected" : L"Waiting for readings";
        label(L"MacBook: " + connection);
        wchar_t lux[64]{};
        if (state.lux) std::swprintf(lux, std::size(lux), L"Ambient: %.1f lux", *state.lux);
        label(state.lux ? lux : L"Ambient: \x2014");
        label(L"Monitor: " + (state.active.empty() ? (state.retrying ? std::wstring(L"Reconnecting") :
            std::wstring(L"Select a monitor")) : state.active));
        label(state.percent ? L"Brightness: " + std::to_wstring(*state.percent) +
            (state.usingFallback ? L"% (fallback)" : L"%") : L"Brightness: \x2014");
        separator();
        AppendMenuW(root, MF_STRING | (control.enabled ? MF_CHECKED : MF_UNCHECKED), toggle, L"Auto Brightness");
        const auto fallbackLabel = L"Fallback Brightness (" + std::to_wstring(control.fallbackPercent) + L"%)\x2026";
        AppendMenuW(root, MF_STRING, fallback, fallbackLabel.c_str());
        if (!simulate) AppendMenuW(root, MF_STRING | (autostart() ? MF_CHECKED : MF_UNCHECKED), startup,
                                   L"Start with Windows");
        HMENU monitors = CreatePopupMenu();
        if (monitors) {
            if (state.monitors.empty()) AppendMenuW(monitors, MF_STRING | MF_GRAYED, 0, L"No DDC/CI monitors");
            for (size_t i = 0; i < state.monitors.size(); ++i) {
                const auto& monitor = state.monitors[i];
                std::wstring name = monitor.name;
                const auto count = std::count_if(state.monitors.begin(), state.monitors.end(),
                                                 [&](const auto& p) { return p.name == monitor.name; });
                if (count > 1) name += L" (" + std::to_wstring(i + 1) + L")";
                for (size_t j = 0; (j = name.find(L'&', j)) != std::wstring::npos; j += 2)
                    name.insert(j, 1, L'&');
                const bool selected = _wcsicmp(monitor.id.c_str(), control.monitor.c_str()) == 0;
                AppendMenuW(monitors, MF_STRING | (selected ? MF_CHECKED : MF_UNCHECKED),
                            firstDisplay + i, name.c_str());
            }
            AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(monitors), L"Select Monitor");
        }
        AppendMenuW(root, MF_STRING, rescan, L"Rescan Monitors");
        separator();
        AppendMenuW(root, MF_STRING | (updates.busy() ? MF_GRAYED : MF_ENABLED), pair, L"Show Pairing Code\x2026");
        AppendMenuW(root, MF_STRING | (state.paired ? MF_ENABLED : MF_GRAYED), forget, L"Forget Pairing\x2026");
        separator();
        if (!simulate) {
            const auto available = updates.busy() ? MF_GRAYED : MF_ENABLED;
            AppendMenuW(root, MF_STRING | available | (updates.automatic ? MF_CHECKED : MF_UNCHECKED),
                        autoUpdate, L"Automatic Updates");
            AppendMenuW(root, MF_STRING | available, updateNow, L"Update Now\x2026");
            AppendMenuW(root, MF_STRING | available, updateToken, L"GitHub Update Token\x2026");
            separator();
        }
        AppendMenuW(root, MF_STRING, quit, L"Quit");

        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(hwnd);
        const UINT command = TrackPopupMenu(root, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                           cursor.x, cursor.y, 0, hwnd, nullptr);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(root);
        switch (command) {
        case toggle: {
            bool enabled = !control.enabled;
            if (!simulate && !save(L"enabled", enabled ? L"1" : L"0")) { error(hwnd, L"Could not save settings."); break; }
            control.enabled = enabled;
            publish();
            break;
        }
        case startup:
            if (!setAutostart(!autostart())) error(hwnd, L"Could not update Windows startup.");
            break;
        case pair: showPairing(hwnd); break;
        case forget:
            if (MessageBoxW(hwnd, L"Remove the key for this Mac?", L"BrightSync",
                    MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND) != IDOK) break;
            if (!receiver->forgetPairing()) error(hwnd, L"Could not remove the pairing.");
            break;
        case rescan: requestScan(); break;
        case fallback: editFallback(hwnd); break;
        case autoUpdate: toggleUpdates(hwnd); break;
        case updateNow: installUpdate(hwnd); break;
        case updateToken: configureUpdates(hwnd); break;
        case quit: DestroyWindow(hwnd); break;
        default:
            if (command >= firstDisplay && command - firstDisplay < state.monitors.size()) {
                const auto& id = state.monitors[command - firstDisplay].id;
                if (!simulate && !save(L"monitor", id.c_str())) { error(hwnd, L"Could not save the monitor selection."); break; }
                control.monitor = id;
                publish();
            }
        }
    }
    bool save(const wchar_t* key, const wchar_t* value, const wchar_t* section = L"general") const {
        return !path.empty() && WritePrivateProfileStringW(section, key, value, path.c_str());
    }
    void requestScan() { ++control.rescan; publish(); }
};

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (app) {
        if (message == taskbarCreated && taskbarCreated) { app->notify(hwnd, NIM_ADD); return 0; }
        switch (message) {
        case WM_CREATE:
            app->receiver = std::make_unique<Receiver>(app->simulate, readTuning(app->path), app->control);
            app->notify(hwnd, NIM_ADD);
            if (!app->trayAdded && !app->simulate)
                SetTimer(hwnd, 1, 2000, nullptr);
            if (!app->simulate) {
                app->armUpdateTimer(hwnd);
                app->displayPower = RegisterPowerSettingNotification(hwnd, &consoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
                WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
            }
            return 0;
        case WM_TIMER:
            if (w == 1) {
                app->notify(hwnd, NIM_ADD);
                if (app->trayAdded) KillTimer(hwnd, 1);
            } else if (w == 2) app->updateTick(hwnd);
            return 0;
        case trayMessage:
            if (!app->menuOpen && (l == WM_RBUTTONUP || l == WM_LBUTTONUP || l == WM_CONTEXTMENU)) {
                app->menuOpen = true;
                app->menu(hwnd);
                app->menuOpen = false;
            }
            return 0;
        case WM_DEVICECHANGE:
            if (w == DBT_DEVNODES_CHANGED) app->requestScan();
            return TRUE;
        case WM_DISPLAYCHANGE: app->requestScan(); return 0;
        case WM_WTSSESSION_CHANGE:
            if (w == WTS_SESSION_UNLOCK || w == WTS_CONSOLE_CONNECT) app->requestScan();
            return 0;
        case WM_POWERBROADCAST:
            if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) app->requestScan();
            if (w == PBT_POWERSETTINGCHANGE && l) {
                const auto* info = reinterpret_cast<const POWERBROADCAST_SETTING*>(l);
                if (IsEqualGUID(info->PowerSetting, consoleDisplayState) &&
                    info->DataLength >= sizeof(DWORD) && info->Data[0] != 0) app->requestScan();
            }
            return TRUE;
        case WM_DESTROY:
            app->updates.cancel();
            if (app->displayPower) UnregisterPowerSettingNotification(app->displayPower);
            if (!app->simulate) WTSUnRegisterSessionNotification(hwnd);
            app->notify(hwnd, NIM_DELETE);
            app->receiver.reset();
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, message, w, l);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    const bool simulated = args && count == 2 && std::wcscmp(args[1], L"--simulate") == 0;
    LocalFree(args);
    HANDLE singleton = CreateMutexW(nullptr, FALSE, simulated ? L"Local\\BrightSyncMock" : L"Local\\BrightSync");
    if (!singleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(singleton); return 0; }

    App app(simulated);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW window{};
    window.lpfnWndProc = windowProc;
    window.hInstance = instance;
    window.lpszClassName = L"BrightSyncTray";
    if (!RegisterClassW(&window) ||
        !CreateWindowExW(WS_EX_TOOLWINDOW, window.lpszClassName, L"BrightSync", WS_POPUP,
                         0, 0, 0, 0, nullptr, nullptr, instance, &app)) {
        CloseHandle(singleton);
        return 1;
    }
    MSG msg{};
    int result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(singleton);
    return result == -1 ? 1 : static_cast<int>(msg.wParam);
}
