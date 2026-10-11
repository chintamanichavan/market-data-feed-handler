# Market Data Feed Handler

Consumer-side market data feed handler in C++23 for MDP 3.0–shaped venues: SBE over UDP multicast,
redundant A/B feeds, separate incremental / snapshot / instrument-definition channels, TCP replay.
Built to serve CME and a second venue with the same protocol from one core.

Companion to [`order-matching-engine`](../order-matching-engine). Deliberately a separate project: an MBP book is
addressed by *level index* and has no order IDs, participants, or self-match logic, so pushing feed
data through `rtrade::LimitOrderBook` would be a category error. An optional adapter lands in Phase 9.

## Status

Code and tests exist for phases 0 through 9 of the roadmap below: framing, codecs for two synthetic
venues, pcap and UDP transports, A/B line arbitration, instrument definitions, the MBP book, the
recovery state machine and the TCP replay client. 187 tests pass.

It has only ever run against synthetic venues and generated packets. The roadmap marks a phase
**done** only when its gate is met, and the gates for the book and recovery phases need a real
exchange capture, which this project does not have yet. No benchmarks, no latency numbers, no
promises; this README is the truth.

## Design commitments

- **pcap-first, network-last.** `Transport` is a concept; the file-backed implementation comes before
  the UDP one. Framing, decode, arbitration, book building, and the whole recovery FSM are developed
  and tested deterministically against captures. Live multicast (Phase 3b) becomes a transport swap
  rather than a debugging surface — and AF_XDP drops in later the same way.
- **Generated codecs, hand-written framing.** Packet header, message framing, and the SBE message
  header are hand-written: they are tiny, stable, hot, and needed before we know which venue's
  generated namespace applies. Message *bodies* come from
  [aeron-io/simple-binary-encoding](https://github.com/aeron-io/simple-binary-encoding) against the
  venue's schema XML. The deciding argument is the `groupSize` composite — CME's is
  `blockLength:u16 + numInGroup:u8`, and a nominally identical venue may use `u16`. Hand-written code
  misparses that silently. Generated headers are checked in, so ordinary builds need no JVM.
- **`BookState` on every update.** A consumer must never have to guess whether the book it just
  received is authoritative. `Recovering` / `Degraded` / `Live` ride on the wire out.
- **The mantissa is the canonical price.** `mantissa / min_price_increment` is not always exact —
  spreads and implieds land on sub-tick increments. Tick conversion is fallible and lives at the
  normalization boundary, never inline.
- **Zero venue conditionals in `src/`.** Differences live in generated code (build time),
  `VenuePolicy` (compile time), or `ChannelConfig` (runtime). An `if (venue == Cme)` in the core is a
  design bug.

## Layout

```
include/rfeed/   public headers — no generated SBE types ever appear here
src/             implementation
venues/<venue>/  schema.xml, generated codecs, policy.hpp
tests/           golden vectors, malformed-input suites, replay fixtures
tools/           rfeed_replay, rfeed_gen (synthetic packets), rfeed_gapinject
```

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizers — the malformed-input suite only means something under these:

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRFEED_SANITIZE=address
cmake --build build-asan && ctest --test-dir build-asan
```

## Roadmap

| Phase | Deliverable |
|---|---|
| 0 | Scaffold, `types.hpp`, `byte_reader.hpp` — **done** |
| 1 | Packet header, message framing, SBE header, golden vectors — **done** |
| 2 | SBE codegen from schema XML; differential test vs a second decoder |
| 3a | pcap transport + synthetic packet generator |
| 3b | UDP multicast transport, `recvmmsg`, `SO_RXQ_OVFL`, hardware timestamps |
| 4 | A/B line arbitration, gap injection tooling |
| 5 | Instrument definitions, fallible tick conversion |
| 6 | MBP book + normalization — **gate: book-vs-snapshot clean over a full day** |
| 7 | Recovery FSM — **gate: recovered book == loss-free book, every gap shape** |
| 8 | TCP replay client with enforced rate limits |
| 9 | Venue genericization, second venue, optional `rtrade` adapter |
| 10 | Benchmarks, fuzzing, allocation-count soak |
| 11 | *(deferred)* MBO books, AF_XDP |

## Verification contract

Three tests decide whether this works. Everything else is supporting evidence.

1. Book-vs-snapshot cross-validation over a full trading day, zero mismatches.
2. Final book after injected-gap recovery equals the final book from the loss-free run, for every
   drop pattern.
3. Byte-identical normalized event stream across replays of the same capture.

## Open questions for the venue

Not assumptions to be discovered in production. Get these in writing during onboarding:

- Is the packet header really `u32` seq + `u64` sending time? Does `MsgSize` include the SBE header?
- Machine-readable SBE schema XML, or PDF-only? (PDF-only adds a week and a large correctness risk.)
- Does a heartbeat consume a sequence number? Getting this backwards produces a permanent phantom
  gap or a permanent phantom duplicate.
- What exactly does `ChannelReset` clear — books, sequence, definitions, the `SecurityID` namespace?
- Is `SecurityID` stable across periods, and unique across channels?
- Price exponent, `MinPriceIncrementAmount`, sub-tick products.
- Which `MDUpdateAction` values exist — is there `DeleteThru` / `DeleteFrom` / `Overlay`?
- TCP replay limits: concurrent requests, packets per request, requests per day, cooldown.
- **A sample capture.** Coding blind through Phase 7 is the single largest schedule risk.

## License

No licence has been chosen yet, so all rights are reserved. The code is public so it can be read.
