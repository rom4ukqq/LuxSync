#include "../windows/brightsync/pairing.hpp"
#include <cassert>
#include <iostream>

using namespace bs;

int main() {
    PairingWindow server;
    const Pin code{'A','B','C','D','2','3','4','5'};
    const Pin wrong{'A','B','C','D','2','3','4','6'};
    constexpr uint64_t endpoint = 10;
    Key saved{};
    int saves = 0;
    auto store = [&](const Key& key) { ++saves; saved = key; return true; };
    auto exchange = [&](const Pin& pin, Nonce nonce, bool valid) {
        Key message{};
        auto client = std::unique_ptr<BS_PAKE, decltype(&bs_pake_free)>(
            bs_pake_start(1, pin.data(), message.data()), bs_pake_free);
        assert(client);
        const auto request = pairPacket(PairKind::start, nonce, message);
        const auto offer = server.receive(request, endpoint, 120, store);
        assert(offer && (*offer)[5] == 2);
        assert(server.receive(request, endpoint, 120, store) == offer);
        assert(!server.receive(request, endpoint + 1, 120, store));
        std::array<uint8_t, BS_PAKE_RESULT> result{};
        assert(bs_pake_finish(client.get(), offer->data() + 22, nonce.data(), result.data()));
        Key clientProof{}, serverProof{}, key{};
        std::copy_n(result.begin(), 32, key.begin());
        std::copy_n(result.begin() + 32, 32, clientProof.begin());
        std::copy_n(result.begin() + 64, 32, serverProof.begin());
        const auto confirm = pairPacket(PairKind::confirm, nonce, clientProof);
        assert(!server.receive(confirm, endpoint + 1, 121, store));
        auto tampered = confirm;
        tampered.back() ^= 1;
        assert(!server.receive(tampered, endpoint, 121, store));
        const auto ack = server.receive(confirm, endpoint, 122, store);
        if (!valid) {
            assert(!ack && saves == 0);
            return;
        }
        assert(ack && *ack == pairPacket(PairKind::accepted, nonce, serverProof));
        assert(saved == key && saves == 1);
        assert(server.receive(confirm, endpoint, 123, store) == ack && saves == 1);
        assert(!server.receive(confirm, endpoint, 16000, store));
        assert(!server.receive(request, endpoint, 16001, store));
    };

    assert(!server.receive(pairPacket(PairKind::start, {1}, {}), endpoint, 99, store));
    server.start(code, 100);
    exchange(wrong, {1}, false);
    exchange(code, {2}, true);

    server.start(code, 20000);
    for (uint8_t id = 1; id <= 6; ++id) {
        Key message{};
        auto client = std::unique_ptr<BS_PAKE, decltype(&bs_pake_free)>(
            bs_pake_start(1, code.data(), message.data()), bs_pake_free);
        assert(client);
        const auto offer = server.receive(pairPacket(PairKind::start, {id}, message), endpoint, 20100, store);
        assert(offer.has_value() == (id <= 5));
    }
    server.start(code, 21000);
    assert(!server.receive(pairPacket(PairKind::start, {7}, {}), endpoint, 201000, store));
    assert(saves == 1);
    std::cout << "PIN pairing, authentication, retry, rate limit and expiry tests passed\n";
}
