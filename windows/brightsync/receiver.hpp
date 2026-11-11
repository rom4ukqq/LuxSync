#pragma once
#include "display.hpp"
#include "pairing.hpp"
#include <mutex>
#include <thread>
#include <cstdio>
#include <share.h>

namespace bs {
struct Control {
    std::optional<Key> key;
    unsigned generation = 0;
    unsigned rescan = 0;
    bool enabled = true;
    int fallbackPercent = defaultFallbackBrightness;
    std::wstring monitor;
};
struct Status {
    bool paired = false;
    bool connected = false;
    std::optional<double> macPercent;
    bool listening = false;
    std::vector<DisplayInfo> monitors;
    std::wstring active;
    std::optional<int> percent;
    bool retrying = false;
    bool usingFallback = false;
};

class Receiver {
    const bool simulate;
    const Tuning config;
    std::mutex mutex;
    Control control;
    Status status;
    PairingWindow pairing;
    bool stopping = false;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread thread;

    void run() {
        Hmac crypto;
        Session session;
        Brightness brightness(config);
        Displays displays;
        Status next;
        int simulatedPercent = 50;
        std::optional<unsigned> scanGeneration;
        std::optional<unsigned> keyGeneration;
        std::optional<double> reading;
        std::wstring previousMonitor;
        bool wasActive = false;
        uint64_t lastRead = 0;
        MonitorRetry retry;

        FILE* trace = nullptr;
        if (simulate) {
            wchar_t tracePath[32768]{};
            const DWORD n = GetEnvironmentVariableW(L"BRIGHTSYNC_TEST_TRACE", tracePath,
                                                    static_cast<DWORD>(std::size(tracePath)));
            if (n && n < std::size(tracePath)) trace = _wfsopen(tracePath, L"w", _SH_DENYNO);
        }
        WSADATA wsa{};
        const bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        SOCKET socket = started ? ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : INVALID_SOCKET;
        auto network = socket != INVALID_SOCKET ? WSACreateEvent() : WSA_INVALID_EVENT;
        if (socket != INVALID_SOCKET) {
            BOOL exclusive = TRUE;
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = htons(port);
            if (network == WSA_INVALID_EVENT ||
                setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                           sizeof(exclusive)) != 0 ||
                bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
                WSAEventSelect(socket, network, FD_READ) != 0) {
                closesocket(socket);
                socket = INVALID_SOCKET;
            }
        }
        next.listening = socket != INVALID_SOCKET;
        const uint64_t startedAt = GetTickCount64();

        for (;;) {
            Control settings;
            {
                std::lock_guard<std::mutex> guard(mutex);
                if (stopping) break;
                pairing.expire(GetTickCount64());
                settings = control;
            }
            if (keyGeneration != settings.generation) {
                keyGeneration = settings.generation;
                session = Session{};
                reading.reset();
                brightness.reset();
            }
            if (scanGeneration != settings.rescan) {
                scanGeneration = settings.rescan;
                retry.changed();
            }
            if (previousMonitor != settings.monitor) {
                previousMonitor = settings.monitor;
                if (!settings.monitor.empty() && !displays.find(settings.monitor)) retry.changed();
                brightness.newMonitor();
                lastRead = 0;
            }

            if (socket != INVALID_SOCKET) {
                // Leave the event signalled if we reach the batch limit and
                // there are datagrams left; recvfrom rearms FD_READ.
                WSANETWORKEVENTS events{};
                WSAEnumNetworkEvents(socket, network, &events);
                for (int i = 0; i < 32; ++i) {
                    Bytes bytes{};
                    sockaddr_in from{};
                    int size = sizeof(from);
                    const auto n = recvfrom(socket, reinterpret_cast<char*>(bytes.data()),
                                            static_cast<int>(bytes.size()), 0,
                                            reinterpret_cast<sockaddr*>(&from), &size);
                    if (n == SOCKET_ERROR) {
                        const auto error = WSAGetLastError();
                        // A previous UDP peer can disappear during sleep. Winsock
                        // reports its ICMP error separately from any queued samples.
                        if (error == WSAEMSGSIZE || error == WSAECONNRESET) continue;
                        break;
                    }
                    if (size != sizeof(from) || from.sin_family != AF_INET) continue;
                    const auto now = GetTickCount64();
                    const uint64_t endpoint = (static_cast<uint64_t>(from.sin_addr.s_addr) << 16) | from.sin_port;
                    if (n == static_cast<int>(sizeof(PairBytes))) {
                        PairBytes request{};
                        std::copy_n(bytes.begin(), request.size(), request.begin());
                        std::optional<PairBytes> reply;
                        {
                            std::lock_guard<std::mutex> guard(mutex);
                            reply = pairing.receive(request, endpoint, now, [&](const Key& key) {
                                if (!simulate && !storeSecret(key)) return false;
                                control.key = key;
                                ++control.generation;
                                SetEvent(wake);
                                return true;
                            });
                        }
                        if (reply) sendto(socket, reinterpret_cast<const char*>(reply->data()),
                                          static_cast<int>(reply->size()), 0,
                                          reinterpret_cast<sockaddr*>(&from), size);
                        continue;
                    }
                    if (!settings.key) continue;
                    auto sign = [&](const uint8_t* b, Key& tag) { return crypto.sign(*settings.key, b, tag); };
                    const auto packet = decode(bytes, static_cast<size_t>(n), sign);
                    if (!packet) continue;
                    const bool hadConnection = session.connected(now);
                    auto reply = session.exchange(*packet, endpoint, now,
                        packet->kind == Kind::hello ? randomBytes<16>() : std::nullopt);
                    if (!reply) continue;
                    if (reply->kind == Kind::ack) {
                        if (!hadConnection) brightness.reset();
                        reading = packet->hundredths / 100.0;
                        if (settings.enabled) brightness.sample(*reading);
                        if (trace) { std::fprintf(trace, "sample %u\n", static_cast<unsigned>(packet->hundredths)); std::fflush(trace); }
                    }
                    auto output = serialize(*reply);
                    Key tag{};
                    if (sign(output.data(), tag)) {
                        std::copy(tag.begin(), tag.end(), output.begin() + signedSize);
                        sendto(socket, reinterpret_cast<const char*>(output.data()), static_cast<int>(output.size()),
                               0, reinterpret_cast<sockaddr*>(&from), size);
                    }
                }
            }

            auto now = GetTickCount64();
            if (retry.ready(now)) {
                displays.scan(simulate, simulatedPercent);
                brightness.newMonitor();
                lastRead = GetTickCount64();
                // A sleeping/unplugged selected monitor is reprobed at a low cadence.
                retry.scanned(lastRead, settings.monitor.empty() ? displays.all.empty() :
                              !displays.find(settings.monitor));
            }

            now = GetTickCount64();
            next.connected = session.connected(now);
            next.macPercent = next.connected ? reading : std::nullopt;
            const bool live = next.connected && settings.enabled;
            if (!live && wasActive) brightness.reset();
            wasActive = live;
            const bool fallbackDue = !next.connected && settings.enabled && now - startedAt >= connectionTimeout;
            const auto fallback = fallbackDue ? std::optional<int>(settings.fallbackPercent) : std::nullopt;
            DWORD wait = 2000;
            next.monitors.clear();
            for (const auto& p : displays.all) next.monitors.push_back(p->info);
            auto display = displays.find(settings.monitor);

            if (display) {
                DWORD raw = 0;
                bool healthy = true;
                if (!simulate && now - lastRead >= 15000) {
                    healthy = display->read(raw);
                    lastRead = GetTickCount64();
                }
                if (healthy && (live || fallback)) {
                    if (const auto target = brightness.next(display->brightness, fallback)) {
                        const auto stepAt = GetTickCount64();
                        const auto delay = brightness.delay(stepAt);
                        if (delay) {
                            wait = static_cast<DWORD>(delay);
                        } else {
                            {
                                std::lock_guard<std::mutex> guard(mutex);
                                if (stopping || !control.enabled || control.generation != settings.generation ||
                                    control.monitor != settings.monitor || control.rescan != settings.rescan ||
                                    control.fallbackPercent != settings.fallbackPercent ||
                                    session.connected(GetTickCount64()) != live) continue;
                            }
                            healthy = display->write(*target);
                            brightness.attempted(stepAt, healthy);
                            if (simulate) simulatedPercent = display->percent();
                            if (trace && healthy) {
                                std::fprintf(trace, "brightness %d\n", display->percent());
                                std::fflush(trace);
                            }
                            if (healthy && brightness.next(display->brightness, fallback))
                                wait = static_cast<DWORD>(brightness.delay(GetTickCount64()));
                        }
                    }
                }
                if (!healthy) {
                    displays.all.clear();
                    next.monitors.clear();
                    display = nullptr;
                    retry.failed(GetTickCount64());
                    brightness.newMonitor();
                }
            }
            next.active = display ? display->info.name : L"";
            next.percent = display ? std::optional<int>(display->percent()) : std::nullopt;
            next.retrying = !settings.monitor.empty() && !display;
            next.usingFallback = fallbackDue && display;
            {
                std::lock_guard<std::mutex> guard(mutex);
                status = next;
            }
            HANDLE handles[]{wake, network};
            WaitForMultipleObjects(socket != INVALID_SOCKET ? 2 : 1, handles, FALSE, wait);
        }

        if (trace) std::fclose(trace);
        if (socket != INVALID_SOCKET) closesocket(socket);
        if (network != WSA_INVALID_EVENT) WSACloseEvent(network);
        if (started) WSACleanup();
    }

public:
    Receiver(bool simulated, Tuning tuning, Control initial)
        : simulate(simulated), config(std::move(tuning)), control(std::move(initial)) {
        if (wake) thread = std::thread([this] { run(); });
    }
    ~Receiver() {
        if (!wake) return;
        {
            std::lock_guard<std::mutex> guard(mutex);
            stopping = true;
        }
        SetEvent(wake);
        thread.join();
        CloseHandle(wake);
    }
    void update(const Control& next) {
        std::lock_guard<std::mutex> guard(mutex);
        control.enabled = next.enabled;
        control.fallbackPercent = next.fallbackPercent;
        control.monitor = next.monitor;
        control.rescan = next.rescan;
        if (wake) SetEvent(wake);
    }
    std::optional<Pin> beginPairing() {
        auto random = randomBytes<8>();
        if (!random) return {};
        Pin code{};
        for (size_t i = 0; i < code.size(); ++i) code[i] = pinAlphabet[(*random)[i] & 31];
        std::lock_guard<std::mutex> guard(mutex);
        pairing.start(code, GetTickCount64());
        return code;
    }
    bool forgetPairing() {
        std::lock_guard<std::mutex> guard(mutex);
        if (!simulate && !forgetSecret()) return false;
        pairing.cancel();
        control.key.reset();
        ++control.generation;
        if (wake) SetEvent(wake);
        return true;
    }
    Status snapshot() {
        std::lock_guard<std::mutex> guard(mutex);
        Status view = status;
        view.paired = control.key.has_value();
        return view;
    }
};
}
