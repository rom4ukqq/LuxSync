# BrightSync UDP v2

Receiver listens on IPv4 UDP 47632. Sender uses an ephemeral port, limited broadcast by default, or a manually configured unicast IPv4 address. All integers are unsigned big endian.

| Offset | Length | Field |
| --- | --- | --- |
| 0 | 4 | ASCII `BSYN` |
| 4 | 1 | Version (2) |
| 5 | 1 | Kind: hello=1, challenge=2, sample=3, ack=4 |
| 6 | 2 | Reserved, zero |
| 8 | 16 | Sender nonce, random for each exchange |
| 24 | 16 | Receiver nonce (zero on hello) |
| 40 | 4 | Built-in display brightness percentage × 100, rounded to nearest integer, 0–10,000 (zero except sample) |
| 44 | 32 | HMAC-SHA256 over the preceding 44 bytes |

A random 256-bit secret identifies the paired installation; a new secret revokes the old pairing. A trusted hello gets a fresh challenge unicast to its source. A sample must come from the same endpoint with both nonces matching the outstanding challenge within four seconds. The receiver consumes the challenge before accepting the reading and returns an authenticated ack. The sender only accepts a challenge for its current poll and an ack from the endpoint and challenge it answered. Each poll retries the exchange. The sender creates a new UDP socket after sleep or a network change, or when the receiver stops acknowledging polls. No clocks need to be synchronized and stale samples cannot be replayed after restart. The packets authenticate brightness samples sent in clear text. Use a different pairing secret for each Mac/PC pair.

## Pairing exchange

Pairing uses the same UDP port. Each pairing packet is 54 bytes with ASCII `BSPR` at offset 0, version 1 at offset 4, type at offset 5, a 16-byte Mac-generated nonce at offset 6 and a 32-byte payload at offset 22. The sequence is start (1, Mac SPAKE2 message), offer (2, Windows SPAKE2 message), confirm (3, Mac proof), and accepted (4, Windows proof). The PAKE is the SPAKE2 implementation in BoringSSL, with identities for the two BrightSync roles. HKDF-SHA256 uses the nonce as salt and expands the PAKE output into three 32-byte values for the sync key, Mac proof and Windows proof. No PIN or sync key is sent over the network.

Windows opens pairing only when the user requests a random eight-character code (40 bits). It expires after three minutes and allows at most five distinct attempts. Each attempt is tied to its source endpoint. The Mac and Windows confirm possession of the PAKE output before using the new key. Windows keeps its acceptance reply for 15 seconds so a lost reply can be retried. The pairing protocol and stored keys carry over from sync v1. Both apps need sync v2 to exchange brightness percentages.
