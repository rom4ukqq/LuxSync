#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace bs {
using Key = std::array<uint8_t, 32>;
using Nonce = std::array<uint8_t, 16>;
using Bytes = std::array<uint8_t, 76>;
constexpr size_t signedSize = 44;
constexpr uint16_t port = 47632;
constexpr uint64_t connectionTimeout = 12000;
constexpr int defaultFallbackBrightness = 40;
enum class Kind : uint8_t { hello = 1, challenge, sample, ack };

struct Packet {
    Kind kind;
    Nonce sender, receiver{};
    uint32_t hundredths = 0;
};

inline Bytes serialize(const Packet& p) {
    Bytes b{'B', 'S', 'Y', 'N', 2, static_cast<uint8_t>(p.kind)};
    std::copy(p.sender.begin(), p.sender.end(), b.begin() + 8);
    std::copy(p.receiver.begin(), p.receiver.end(), b.begin() + 24);
    for (int i = 0; i < 4; ++i) b[40 + i] = static_cast<uint8_t>(p.hundredths >> (24 - i * 8));
    return b;
}

// Sign returns false on crypto failure. Length is checked before parsing.
template <typename Sign>
std::optional<Packet> decode(const Bytes& b, size_t length, Sign sign) {
    if (length != b.size() || b[0] != 'B' || b[1] != 'S' || b[2] != 'Y' || b[3] != 'N' ||
        b[4] != 2 || b[5] < 1 || b[5] > 4 || b[6] || b[7]) return {};
    Key tag{};
    if (!sign(b.data(), tag)) return {};
    unsigned diff = 0;
    for (size_t i = 0; i < tag.size(); ++i) diff |= tag[i] ^ b[signedSize + i];
    if (diff != 0) return {};
    Packet p{static_cast<Kind>(b[5]), {}};
    std::copy_n(b.begin() + 8, 16, p.sender.begin());
    std::copy_n(b.begin() + 24, 16, p.receiver.begin());
    for (int i = 40; i < 44; ++i) p.hundredths = (p.hundredths << 8) | b[i];
    if (p.sender == Nonce{} || (p.receiver == Nonce{}) != (p.kind == Kind::hello) ||
        (p.kind == Kind::sample ? p.hundredths > 10000 : p.hundredths != 0)) return {};
    return p;
}

class Session {
    struct Pending { Nonce sender, receiver; uint64_t endpoint, expiry; };
    std::optional<Pending> pending;
    std::optional<uint64_t> lastSample;
public:
    std::optional<Packet> exchange(const Packet& p, uint64_t endpoint, uint64_t now,
                                   std::optional<Nonce> fresh) {
        if (p.kind == Kind::hello && fresh && *fresh != Nonce{}) {
            if (pending && now < pending->expiry && endpoint == pending->endpoint && p.sender == pending->sender)
                return Packet{Kind::challenge, p.sender, pending->receiver};
            pending = Pending{p.sender, *fresh, endpoint, now + 4000};
            return Packet{Kind::challenge, p.sender, *fresh};
        }
        if (p.kind == Kind::sample && pending && now < pending->expiry && endpoint == pending->endpoint &&
            p.sender == pending->sender && p.receiver == pending->receiver) {
            pending.reset();
            lastSample = now;
            return Packet{Kind::ack, p.sender, p.receiver};
        }
        return {};
    }
    bool connected(uint64_t now) const { return lastSample && now - *lastSample < connectionTimeout; }
};

struct Tuning {
    double minimum = 10, maximum = 100, offset = 0, alpha = 1, hysteresis = 1;
    uint64_t interval = 300;
};

// Reprobe after losing the handle, and keep looking for a missing selected
// display at a lower cadence. A topology event interrupts either delay.
class MonitorRetry {
    std::optional<uint64_t> due = 0;
public:
    bool ready(uint64_t now) const { return due && now >= *due; }
    void changed() { due = 0; }
    void failed(uint64_t now) { due = now + 3000; }
    void scanned(uint64_t now, bool missing) {
        if (missing) due = now + 10000;
        else due.reset();
    }
};

struct BrightnessRange {
    uint32_t minimum = 0, maximum = 100, current = 50;

    uint32_t span() const { return maximum - minimum; }
    double percent(uint32_t level) const { return 100.0 * (level - minimum) / span(); }
    uint32_t level(double percent) const {
        return minimum + static_cast<uint32_t>(std::llround(span() * (percent / 100.0)));
    }
};

class Brightness {
    const Tuning& cfg;
    double filtered = 0;
    bool hasSample = false;
    std::optional<uint64_t> lastAttempt;
    bool moving = false;
public:
    explicit Brightness(const Tuning& config) : cfg(config) {}
    void sample(double percent) {
        filtered = hasSample ? filtered + cfg.alpha * (percent - filtered) : percent;
        hasSample = true;
    }
    void reset() { hasSample = false; moving = false; }
    void newMonitor() { moving = false; }

    std::optional<uint32_t> next(const BrightnessRange& range, std::optional<int> fallback = {}) {
        if ((!hasSample && !fallback) || !range.span()) return {};
        const double desired = fallback ? *fallback : filtered + cfg.offset;
        const auto target = range.level(std::clamp(desired, cfg.minimum, cfg.maximum));
        const int64_t delta = static_cast<int64_t>(target) - range.current;
        const bool outside = range.current < range.level(cfg.minimum) || range.current > range.level(cfg.maximum);
        const double difference = 100.0 * std::abs(delta) / range.span();
        if (delta == 0 || (!fallback && !moving && !outside && difference < cfg.hysteresis)) {
            moving = false;
            return {};
        }
        // One percentage point where possible, otherwise one native monitor level.
        const int64_t step = std::max<uint32_t>(1, range.span() / 100);
        return static_cast<uint32_t>(range.current + std::clamp(delta, -step, step));
    }

    uint64_t delay(uint64_t now) const {
        if (!lastAttempt || now - *lastAttempt >= cfg.interval) return 0;
        return cfg.interval - (now - *lastAttempt);
    }

    void attempted(uint64_t now, bool ok) { lastAttempt = now; moving = ok; }
};
}
