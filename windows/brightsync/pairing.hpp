#pragma once
#include "core.hpp"
#include "../../protocol/pake.h"
#include <memory>

namespace bs {
using Pin = std::array<uint8_t, 8>;
using PairBytes = std::array<uint8_t, 54>;
inline constexpr char pinAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
enum class PairKind : uint8_t { start = 1, offer, confirm, accepted };

inline PairBytes pairPacket(PairKind kind, const Nonce& nonce, const Key& payload) {
    PairBytes bytes{'B', 'S', 'P', 'R', 1, static_cast<uint8_t>(kind)};
    std::copy(nonce.begin(), nonce.end(), bytes.begin() + 6);
    std::copy(payload.begin(), payload.end(), bytes.begin() + 22);
    return bytes;
}

class PairingWindow {
    struct Attempt {
        uint64_t endpoint;
        Nonce nonce;
        Key request, offer;
        std::array<uint8_t, BS_PAKE_RESULT> result;
    };
    Pin pin{};
    uint64_t expires = 0;
    std::vector<Attempt> attempts;
    struct Receipt {
        uint64_t endpoint, expires;
        PairBytes request, response;
    };
    std::optional<Receipt> receipt;

public:
    PairingWindow() { attempts.reserve(5); }
    ~PairingWindow() { cancel(); }

    void start(const Pin& code, uint64_t now) {
        cancel();
        pin = code;
        expires = now + 180000;
    }

    void cancel() {
        bs_pake_clear(pin.data(), pin.size());
        expires = 0;
        for (auto& attempt : attempts) bs_pake_clear(attempt.result.data(), attempt.result.size());
        attempts.clear();
        receipt.reset();
    }

    void expire(uint64_t now) {
        if (expires && now >= expires) cancel();
        if (receipt && now >= receipt->expires) receipt.reset();
    }

    template <typename Save>
    std::optional<PairBytes> receive(const PairBytes& bytes, uint64_t endpoint, uint64_t now, Save save) {
        expire(now);
        if (receipt && endpoint == receipt->endpoint && bytes == receipt->request) return receipt->response;
        if (!expires || bytes[0] != 'B' || bytes[1] != 'S' || bytes[2] != 'P' ||
            bytes[3] != 'R' || bytes[4] != 1) return {};
        Nonce nonce{};
        Key payload{};
        std::copy_n(bytes.begin() + 6, nonce.size(), nonce.begin());
        std::copy_n(bytes.begin() + 22, payload.size(), payload.begin());
        if (nonce == Nonce{}) return {};

        auto attempt = std::find_if(attempts.begin(), attempts.end(), [&](const auto& a) { return a.nonce == nonce; });
        if (bytes[5] == static_cast<uint8_t>(PairKind::start)) {
            if (attempt != attempts.end()) {
                if (attempt->endpoint != endpoint || attempt->request != payload || attempt->offer == Key{}) return {};
                return pairPacket(PairKind::offer, nonce, attempt->offer);
            }
            // Each distinct start consumes one online PIN attempt, even with a malformed group element.
            if (attempts.size() >= 5) return {};
            auto& next = attempts.emplace_back(Attempt{endpoint, nonce, payload, {}, {}});
            auto context = std::unique_ptr<BS_PAKE, decltype(&bs_pake_free)>(
                bs_pake_start(0, pin.data(), next.offer.data()), bs_pake_free);
            if (!context || !bs_pake_finish(context.get(), payload.data(), nonce.data(), next.result.data())) {
                next.offer.fill(0);
                return {};
            }
            return pairPacket(PairKind::offer, nonce, next.offer);
        }
        if (bytes[5] != static_cast<uint8_t>(PairKind::confirm) || attempt == attempts.end() ||
            attempt->endpoint != endpoint || attempt->offer == Key{} ||
            !bs_pake_equal(payload.data(), attempt->result.data() + 32)) return {};
        Key key{}, proof{};
        std::copy_n(attempt->result.begin(), key.size(), key.begin());
        std::copy_n(attempt->result.begin() + 64, proof.size(), proof.begin());
        const bool saved = save(key);
        bs_pake_clear(key.data(), key.size());
        if (!saved) return {};
        const auto reply = pairPacket(PairKind::accepted, nonce, proof);
        cancel();
        receipt = Receipt{endpoint, now + 15000, bytes, reply};
        return reply;
    }
};
}
