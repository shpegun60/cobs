# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

The canonical architecture migration plan, locked decisions, phase checklist,
and acceptance criteria are in `doc/COBS_REFACTOR_PLAN.md`. Consult it before
changing COBS names, ownership, storage, delegates, state fields, or file
boundaries.

The stable COBS documentation is split by boundary:

- `doc/ARCHITECTURE.md` — canonical component/API/ownership entry point;
- `doc/PROTOCOL.md` — normative wire and decoder behavior;
- `doc/STORAGE.md` — exact custom-storage contract;
- `doc/COBS_ENGINE.md` — detailed rationale, transition traces, and proofs.

A Qt Widgets application (qmake, C++20) intended as a desktop host/testbed for a reusable UART + COBS communication stack. The Qt GUI itself is currently a bare scaffold (`main.cpp`, `mainwindow.*`), but `COBS.pro` includes `cobs/cobs.pri` and therefore compiles the real non-template COBS core. The separate console consumer under `cobs/tests/qmake_consumer/` instantiates and executes the full public API over both built-in storage strategies. The STM32 implementation remains in `uart/Uart.h` (not part of the Qt build — it needs an STM32 HAL).

Local dependencies live in `libs/` (cloned from the author's GitHub, on `INCLUDEPATH`):
- `libs/spsc` — wait-free SPSC containers; `spsc::cache_aligned_chunk_fifo` is the RX buffer pool of the UART engine (DMA writes straight into claimed chunk slots). UART builds need both `libs/spsc` and `libs/spsc/src` on the include path: headers live below `src` and resolve the library-owned root `basic_types.h`.
- `libs/delegate` — `tiny::delegate`, the no-heap `std::function` replacement used for all callbacks.

## Build

Use the `/build` skill (`.claude/skills/build/SKILL.md`) to build from the command line; full instructions including exact toolchain paths are in `doc/BUILD.md`. In short (Git Bash):

```bash
export PATH="/c/Qt/6.10.1/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"
mkdir -p build/cli && cd build/cli
qmake ../../COBS.pro   # CONFIG+=debug for a debug build
mingw32-make -j
```

Output: `build/cli/release/COBS.exe` (release is the default). Qt Creator uses its own directory, `build/Desktop_Qt_6_10_1_MinGW_64_bit_Debug/` — never build into it from the CLI.

The reusable COBS fragment and its real application-shaped consumer are
verified separately:

```bash
export PATH="/c/Qt/6.10.1/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"
sh cobs/tests/qmake_consumer/run.sh
```

The consumer includes only `Cobs.h`, links `Decoder.cpp` and `Encoder.cpp`
through `cobs.pri`, and runs the same bind/send/receive flow over `Heap` and
`Pool`. A downstream qmake project uses `include(path/to/cobs/cobs.pri)`; set
`COBS_DELEGATE_DIR` before the include only when `tiny_delegate` is not at the
repository default.

GUI source/header/form files must be added to `SOURCES`/`HEADERS`/`FORMS` in
`COBS.pro`. COBS library files belong in `cobs/cobs.pri`. Re-run qmake after
changing either source list.

### STM32 portability matrix

`uart/Uart.h` is verified by compile-only builds for real STM32 targets (F1 = legacy SR/DR IP, G4 = new ISR/RDR IP + classic DMA, H7RS = Cortex-M7 + D-cache + GPDMA), using the arm-none-eabi-gcc 14.3 shipped with STM32CubeIDE 2.0.0 and HAL drivers in `libs/` / the local Cube repository:

```bash
sh uart/tests/port/build.sh
```

Objects land in `uart/tests/port/out/`; inspect codegen with the same toolchain's `arm-none-eabi-objdump -d -C`. IDE clangd errors like "main.h not found" inside `uart/Uart.h` are expected — that header only compiles against an STM32 HAL via this matrix or the host fake HAL below.

### Host test suite (executable)

`uart/tests/host/` runs the driver against a fake HAL on the desktop — unlike the port matrix it EXECUTES the interleavings:

```bash
PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH" sh uart/tests/host/run.sh
```

The fake HAL models the real behaviours verified in the ST sources (IDLE/TC end reception before the callback; every RX error is blocking in DMA mode; an abort may raise the completion callback of the transfer it interrupts; aborts can return `HAL_TIMEOUT`), plus a PRIMASK where an interrupt raised while masked becomes **pending** and runs on restore, and a DMA ownership model that asserts DMA-owned memory is never handed to the consumer.

Tests are grouped by guarantee (Initialization, RxOwnership, RxDiscontinuity, TxOwnership, TeardownArbitration, FaultInjection, Watchdog), not by HAL function, so they survive refactoring inside the driver.

**Run both suites after any change to `uart/`.**

### COBS host tests

The COBS layer owns no HAL, so its suites are ordinary host programs — no fake anything:

```bash
PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH" sh cobs/tests/run.sh
```

Before building runtime suites, the runner compiles all five public headers
independently and verifies six intentional compile-fail translation units with
boundary-specific diagnostic markers. Those negative cases lock the Storage
concept, coordinator-only message/packet operations, serializer constraints,
and the physical absence of old/split API names.

It builds and runs nine independent binaries, so a failure names the layer without needing a stack trace (plus two of them a second time under `-DNDEBUG`, because the pool's double-free rejection is a guarantee and a guarantee that only holds in debug builds is not one):

- `test_decoder` — pure COBS framing only, with no length prefix anywhere; mostly property tests (every length × pattern × span-boundary combination, ~20k checks), plus the segmented-output battery: the same wire decoded under many segmentation plans must give identical bytes.
- `test_block_pool` — `cobs::detail::BlockPool`, the raw memory primitive both policies are built on.
- `test_storage` — the `cobs::Storage` contract (`doc/STORAGE.md`), one shared test body run against both `cobs::Heap` and `cobs::Pool`. It uses nothing beyond the checked contract, so strategy-specific knowledge cannot leak into the engine.
- `test_receiver` — the internal RX vertical end to end: length-field codec, exact-allocation proof through recording storage, queue/teardown behavior, and every way a declared length can disagree with a frame.
- `test_packet` — the public `cobs::Packet` lifetime boundary, created only through `Endpoint::consume()/pop_packet()` with no detail include or friend escape. Copy/move/assignment, retained-packet back-pressure, and the 70,000-copy refcount regression are asserted behaviorally through pool occupancy.
- `test_encoder` — canonical in-place encoding, checked against two independent oracles (the reference encoder byte-for-byte, and a round trip through `cobs::codec::Decoder`) at the minimum legal headroom.
- `test_message` — `cobs::Message`: TX block ownership, move semantics, the public container semantics (`size`/`capacity`, the ~1.5x growth sequence, the strong no-change guarantee on a failed growth), and the `append_native`/`append_bytes` serializers. Encoding, retry, and payload contents are observed through a real `cobs::Endpoint::send()` coordinator and a capture transport; compile-time checks keep encoding state, storage identity, and block surrender out of the application API.
- `test_endpoint` — the assembled `cobs::Endpoint` over a fake transport bound through delegates, the whole body run against **both** storage strategies: RX end to end, all send outcomes, retry of the identical frame after a failed start, TX reclaim only after the transport lets go, atomic transport-pair binding, and the combined value-snapshot `cobs::Stats` boundary. A separate binding-mode group locks in owning move-only lambdas, bound members, and explicitly borrowed callables.

The script builds with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` and adds `-fsanitize=address,undefined` when the toolchain provides the runtime. MinGW does not, so for a sanitized run use WSL (the exact command is in the script header). `COBS_POOL_CHECKS` (on by default in EVERY build, `NDEBUG` included) compiles in the pool's double-free and foreign-pointer detection; a rejected free is counted and ignored rather than corrupting the free list. Set it to 0 explicitly to opt out.

## Architecture

`doc/old/UART_COBS_ARCHITECTURE.md` is the archived original design sketch. It remains
useful as historical rationale, but its sample API and parts of its UART model
are not current. The implemented boundaries are:

- **Three layers**: byte transport (UART/TCP/…) → COBS (framing, packet lifetime, storage) → application (`cobs::Message` / `cobs::Packet`). "UART handles bytes. COBS handles packets."
- The transport is bound as one owning `tiny::delegate` pair for busy state and `send(span)`; it has **no TX queue**, no knowledge of framing, CRC, or packet sizes. TX-busy policy (retry/drop/queue) belongs to layers above.
- `cobs::Endpoint` is templated on a checked `cobs::Storage` implementation (`cobs::Heap` by default, `cobs::Pool` for deterministic storage); changing storage must not change the application-facing API or wire format.
- **RX and TX ownership are deliberately asymmetric**: RX packets use the intrusive shared `cobs::Packet` handle (refcount inside `cobs::RxBlock`, payload immutable once decoded); TX frames use exclusive ownership through one `cobs::TxBlock` descriptor (`cobs::Message` owns until `send()` succeeds, then the endpoint holds it until DMA completes).
- RX callbacks deliver arbitrary byte chunks (a frame may span chunks, or one chunk may hold several frames); the span is valid only during the callback. On errors COBS drops bytes until the next `0x00` delimiter to resynchronize.
- CRC is a COBS/protocol policy, never a UART feature.

For current COBS work, start with `doc/ARCHITECTURE.md` and follow its links
to `PROTOCOL.md` or `STORAGE.md`. `COBS_ENGINE.md` retains the reviewed decoder
state machine, arithmetic rationale, in-place overlap proof, and test plan.
For current UART behavior, read `uart/Uart.h` and its executable host and
portability tests rather than treating the old sketch as an API contract.

## Reference material

`doc/old/` holds legacy STM32 UART driver code (HAL-based `UartEngine`, RS-485 wrapper, DMA variants) with a README in Ukrainian. It is prior art for the new design, not part of the build — don't extend it; the new architecture intentionally replaces its approach.
