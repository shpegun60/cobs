# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

A Qt Widgets application (qmake, C++20) intended as a desktop host/testbed for a reusable UART + COBS communication stack. The Qt GUI itself is currently a bare scaffold (`main.cpp`, `mainwindow.*`); the substance of the project is the transport-stack design in `doc/UART_COBS_ARCHITECTURE.md` and its STM32 implementation in `uart/Uart.h` (not part of the Qt build — it needs an STM32 HAL).

Local dependencies live in `libs/` (cloned from the author's GitHub, on `INCLUDEPATH`):
- `libs/spsc` — wait-free SPSC containers; `spsc::cache_aligned_chunk_fifo` is the RX buffer pool of the UART engine (DMA writes straight into claimed chunk slots).
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

New source/header/form files must be added to `SOURCES`/`HEADERS`/`FORMS` in `COBS.pro`, then qmake must be re-run.

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

It builds and runs seven independent binaries, so a failure names the layer without needing a stack trace:

- `test_decoder` — framing only; mostly property tests (every length × pattern × span-boundary combination, ~20k checks).
- `test_block_pool` — `cobs_detail::StaticBlockPool`, the raw memory primitive both policies are built on.
- `test_allocators` — the allocator policy contract (`COBS_ENGINE.md` §9), one shared test body run against **both** `CobsHeapAllocator` and `CobsFixedAllocator`. It uses nothing but the contract, so if it ever needed to know which policy it was talking to, the abstraction would have leaked.
- `test_cobs_rx` — the assembled RX vertical end to end, plus `PacketRef` semantics. Those live here rather than beside the pool because `PacketRef::adopt()` is private to `CobsRx`, its only legitimate source; refcounts are therefore asserted behaviourally, through pool occupancy.
- `test_encoder` — canonical in-place encoding, checked against two independent oracles (the reference encoder byte-for-byte, and a round trip through `CobsDecoder`) at the minimum legal headroom.
- `test_cobs_msg` — `CobsMsg`: TX block ownership, move semantics, payload geometry and encoding. No transport.
- `test_cobs` — the assembled `Cobs` engine over a fake transport bound through delegates, the whole body run against **both** policies: RX end to end, the `push()` outcomes (`Sent`/`Busy`/`Error`/`NotBound`/`Invalid`), retry of the identical frame after a failed start, `proceed()` reclaiming the block only once the transport lets go, and rebinding refused while a transfer is in flight.

The script builds with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` and adds `-fsanitize=address,undefined` when the toolchain provides the runtime. MinGW does not, so for a sanitized run use WSL (the exact command is in the script header). `COBS_POOL_CHECKS` (on by default in debug builds) compiles in the pool's double-free and foreign-pointer detection; a rejected free is counted and ignored rather than corrupting the free list.

## Architecture

`doc/UART_COBS_ARCHITECTURE.md` is the original design document — read it before implementing or modifying any transport code. Its core rules:

- **Three layers**: byte transport (UART/TCP/…) → COBS (framing, packet lifetime, allocation) → application (`CobsMsg` / `PacketRef`). "UART handles bytes. COBS handles packets."
- The transport implements only `tx_busy()` and `send(span)` (interface `IByteTx`); it has **no TX queue**, no knowledge of framing, CRC, or packet sizes. TX-busy policy (retry/drop/queue) belongs to layers above.
- COBS is templated on an allocator (`HeapAllocator` default, fixed-pool for embedded); changing the allocator must not change the application-facing API.
- **RX and TX ownership are deliberately asymmetric**: RX packets use a custom intrusive shared handle (`PacketRef`, refcount inside `RxPacket`, packets immutable once decoded); TX frames use exclusive ownership (`CobsMsg` owns until `push()` succeeds, then COBS holds the frame until DMA completes).
- RX callbacks deliver arbitrary byte chunks (a frame may span chunks, or one chunk may hold several frames); the span is valid only during the callback. On errors COBS drops bytes until the next `0x00` delimiter to resynchronize.
- CRC is a COBS/protocol policy, never a UART feature.

Section 33 of the doc ("Key invariants") lists the invariants any implementation must preserve.

**`doc/COBS_ENGINE.md` supersedes it for the COBS layer.** The architecture document is a design sketch and its UART sections are now partly historical (the implemented driver is a template with an internal chunk pool, not the external-storage engine it describes); `COBS_ENGINE.md` is the reviewed contract the COBS implementation must satisfy — decision table, decoder state machine, size arithmetic, the in-place TX overlap invariant with its proof, and the test plan. Where the two disagree, `COBS_ENGINE.md` wins. Notably it splits a non-template `CobsDecoder` out of `Cobs<Allocator>`, binds the transport with delegates rather than a template parameter, and defines memory as a single allocator policy (§9).

## Reference material

`doc/old/` holds legacy STM32 UART driver code (HAL-based `UartEngine`, RS-485 wrapper, DMA variants) with a README in Ukrainian. It is prior art for the new design, not part of the build — don't extend it; the new architecture intentionally replaces its approach.
