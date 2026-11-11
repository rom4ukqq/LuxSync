#include "../windows/brightsync/core.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace bs;
    Tuning config;
    auto level = [](uint32_t current) { return BrightnessRange{0, 100, current}; };
    Brightness output(config);
    assert(!output.next(level(50)));
    output.sample(40.25);
    assert(!output.next(level(40))); // same native monitor level
    assert(output.next(level(39)) == 40u);
    output.sample(40); // the panel percentage is the target
    auto ramp = level(25);
    for (uint64_t tick = 0; ramp.current < 40; tick += 300) {
        assert(output.delay(tick) == 0);
        const auto step = output.next(ramp);
        assert(step == ramp.current + 1);
        ramp.current = *step;
        output.attempted(tick, true);
        assert(output.delay(tick + 299) == 1);
    }
    assert(!output.next(ramp));
    output.reset();
    assert(!output.next(level(40)));
    assert(output.next(level(40), 39) == 39u); // fallback reaches even a nearby target
    output.sample(40);
    assert(output.next(level(39)) == 40u); // resume the panel target after fallback

    Tuning smoothed;
    smoothed.alpha = 0.25;
    smoothed.hysteresis = 3;
    Brightness filtered(smoothed);
    filtered.sample(20);
    filtered.sample(100); // optional EMA now produces 40%
    assert(!filtered.next(level(39)));
    assert(filtered.next(level(37)) == 38u);

    const BrightnessRange coarse{20, 40, 31};
    assert(output.next(coarse, 65) == 32u); // use one actual monitor level
    const BrightnessRange fine{0, 255, 150};
    assert(output.next(fine, 90) == 152u); // no more than one percentage point

    Tuning capped;
    capped.minimum = 15;
    capped.maximum = 70;
    capped.offset = 20;
    Brightness limited(capped);
    limited.sample(100);
    assert(!limited.next(level(70)));
    assert(limited.next(level(72)) == 71u);
    assert(!limited.next(level(70), 99)); // fallback respects the configured bounds
    assert(limited.next(level(16), 0) == 15u);
    capped.offset = -40;
    limited.reset();
    limited.sample(0);
    assert(!limited.next(level(15)));
    assert(limited.next(level(10)) == 11u);

    MonitorRetry recovery;
    assert(recovery.ready(0));
    recovery.scanned(0, false);
    assert(!recovery.ready(100));
    output.sample(100);
    assert(output.next(level(50)) == 51u);
    output.attempted(6000, false);
    recovery.failed(6000);
    assert(output.delay(6299) == 1);
    assert(!recovery.ready(8999) && recovery.ready(9000));
    recovery.scanned(9000, true);
    assert(!recovery.ready(18999) && recovery.ready(19000));
    recovery.changed();
    assert(recovery.ready(9500));
    recovery.scanned(9500, false);
    assert(output.next(level(44)) == 45u);

    Nonce client{1}, server{2}, nextClient{3}, nextServer{4};
    Packet hello{Kind::hello, client};
    Packet sample{Kind::sample, client, server, 3925};
    Session session;
    assert(!session.exchange(sample, 3, 0, {}));
    auto challenge = session.exchange(hello, 3, 1, server);
    assert(challenge && challenge->kind == Kind::challenge && challenge->receiver == server);
    assert(!session.exchange(sample, 8, 2, {}));
    assert(!session.exchange(sample, 3, 4001, {})); // expired
    assert(session.exchange(hello, 3, 5000, server));
    auto ack = session.exchange(sample, 3, 5001, {});
    assert(ack && ack->kind == Kind::ack);
    assert(!session.exchange(sample, 3, 5002, {})); // replay
    assert(session.connected(5001 + connectionTimeout - 1));
    assert(!session.connected(5001 + connectionTimeout));
    assert(session.exchange({Kind::hello, nextClient}, 9, 30000, nextServer));
    assert(session.exchange({Kind::sample, nextClient, nextServer, 0}, 9, 30001, {}));
    assert(session.connected(30001)); // reconnect from a new endpoint
    std::cout << "Brightness, DDC recovery and session tests passed\n";
}
