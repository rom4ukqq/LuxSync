#pragma once
#include "platform.hpp"
#include <wincred.h>

namespace bs {

inline constexpr wchar_t updateCredential[] = L"BrightSync/LuxSync GitHub update token";

inline bool validUpdateToken(const std::string& token) {
    return token.size() >= 20 && token.size() <= 256 &&
        std::all_of(token.begin(), token.end(), [](unsigned char c) {
            return c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        });
}

inline std::string loadUpdateToken() {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(updateCredential, CRED_TYPE_GENERIC, 0, &credential)) return {};
    std::string token;
    if (credential->CredentialBlob && credential->CredentialBlobSize <= 256) {
        token.assign(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    }
    CredFree(credential);
    return validUpdateToken(token) ? token : std::string{};
}

inline bool storeUpdateToken(std::string& token) {
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(updateCredential);
    credential.CredentialBlobSize = static_cast<DWORD>(token.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(token.data());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) != FALSE;
}

inline bool removeUpdateToken() {
    return CredDeleteW(updateCredential, CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
}

struct UpdateTokenDialog { std::string token; };
inline constexpr UINT removeToken = 108;

inline INT_PTR CALLBACK updateTokenDialog(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    constexpr int input = 107;
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(dialog, DWLP_USER, static_cast<LONG_PTR>(l));
        SendDlgItemMessageW(dialog, input, EM_SETLIMITTEXT, 256, 0);
        SetForegroundWindow(dialog);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    const auto action = LOWORD(w);
    if (action == IDCANCEL || action == removeToken) {
        EndDialog(dialog, action);
        return TRUE;
    }
    if (action != IDOK) return FALSE;

    wchar_t buffer[257]{};
    const auto length = GetDlgItemTextW(dialog, input, buffer, ARRAYSIZE(buffer));
    std::string token;
    for (UINT i = 0; i < length; ++i)
        token.push_back(buffer[i] <= 127 ? static_cast<char>(buffer[i]) : '\0');
    SecureZeroMemory(buffer, sizeof(buffer));
    if (!validUpdateToken(token)) {
        MessageBoxW(dialog, L"Paste the fine-grained token from GitHub.", L"BrightSync", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, input));
        return TRUE;
    }
    auto* state = reinterpret_cast<UpdateTokenDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    state->token = std::move(token);
    EndDialog(dialog, IDOK);
    return TRUE;
}

class Updates {
    struct OwnedHandle {
        HANDLE handle = nullptr;
        ~OwnedHandle() { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    };
    HANDLE process = nullptr;
    bool manual = false;
    uint64_t next = GetTickCount64() + 30000;

public:
    bool automatic = false;

    ~Updates() { cancel(); }
    void cancel() {
        if (!process) return;
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 1);
        CloseHandle(process);
        process = nullptr;
    }
    bool busy() const { return process != nullptr; }
    bool due() const { return automatic && !process && GetTickCount64() >= next; }
    void checkSoon() { next = GetTickCount64(); }
    void restoreLastCheck(uint64_t elapsedSeconds) {
        constexpr uint64_t interval = 6 * 60 * 60;
        if (elapsedSeconds < interval)
            next = std::max(next, GetTickCount64() + (interval - elapsedSeconds) * 1000);
    }

    bool start(std::string token, bool userInitiated) {
        if (process) return false;
        next = GetTickCount64() + 6 * 60 * 60 * 1000;
        if (!validUpdateToken(token)) return false;
        wchar_t path[32768]{};
        DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (!length || length >= std::size(path)) return false;
        const std::wstring target(path, length);
        const auto separator = target.find_last_of(L"\\/");
        if (separator == std::wstring::npos) return false;
        const std::wstring helper = target.substr(0, separator + 1) + L"BrightSyncUpdater.exe";
        std::wstring command = L"\"" + helper + L"\" prepare \"" + target + L"\" " +
            std::to_wstring(GetCurrentProcessId());

        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        OwnedHandle readPipe, writePipe, nullDevice;
        if (!CreatePipe(&readPipe.handle, &writePipe.handle, &security, 0) ||
            !SetHandleInformation(writePipe.handle, HANDLE_FLAG_INHERIT, 0)) return false;
        nullDevice.handle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &security, OPEN_EXISTING, 0, nullptr);
        if (nullDevice.handle == INVALID_HANDLE_VALUE) return false;

        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        if (!attributeSize) return false;
        std::vector<unsigned char> buffer(attributeSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)) return false;
        HANDLE handles[]{readPipe.handle, nullDevice.handle};
        const bool restricted = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handles, sizeof(handles), nullptr, nullptr) != FALSE;

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = readPipe.handle;
        startup.StartupInfo.hStdOutput = nullDevice.handle;
        startup.StartupInfo.hStdError = nullDevice.handle;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION child{};
        const bool started = restricted && CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child);
        DeleteProcThreadAttributeList(attributes);
        if (!started) return false;
        CloseHandle(child.hThread);
        process = child.hProcess;
        manual = userInitiated;
        DWORD written = 0;
        WriteFile(writePipe.handle, token.data(), static_cast<DWORD>(token.size()), &written, nullptr);
        SecureZeroMemory(token.data(), token.size());
        return true;
    }

    struct Result { DWORD code; bool manual; };
    std::optional<Result> poll() {
        if (!process || WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return {};
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        process = nullptr;
        return Result{code, manual};
    }
};
}
