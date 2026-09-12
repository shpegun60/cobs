# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Subsystem status (2026-09-06)

After the final hardening review, the whole Modbus subsystem is CLOSED and is
not to be audited, hardened or reworked without a concrete reproducible
failure or a new requirement:

```
Modbus core          CLOSED
CRC                  CLOSED
RTU framing          CLOSED
STM32 UART           CLOSED (contract sealed at setWakeHandler + rx_progress)
STM32 UartAdapter    CLOSED
FreeRTOS glue        CLOSED
Qt SerialAdapter     CLOSED
Qt RtuClient         CLOSED
QtSerialBus interop  HARDWARE VERIFIED (src/adapters/qt/tests/hardware/h7s/README.md)
```

The 7 September live repeat is recorded in `doc/HARDWARE_REGRESSION_2026-09-07.md`:
the full COBS/RTU fault matrix and framed high-baud trials passed. Two Qt-server
timeouts are retained as failures alongside passing repeats. The follow-up
`doc/QT_USB_TIMEOUT_DIAGNOSIS.md` reproduces the step-0 failure mechanism by
stalling host fragment processing, and verifies the Qt reference server's
USB-aware 50-ms RX deadline. That experiment changed no production headers or
MCU images. The subsequent `doc/QT_CLIENT_RECOVERY.md` fixes reproducible
desktop-only event ordering, write deadlines, cancellation/retry accounting
and nested error cleanup in SerialAdapter/RtuClient, with 202 host checks.
It does not reopen or change the protocol cores, CRC, UART or MCU adapter.
The runner keeps a trace by default and returns the verifier's failure after
restoring the board. Do not reinterpret the old untraced failure as proved,
claim unbounded Qt/VCP reliability, or count host-only tests as board coverage.

Two documented properties are not bugs: a length-table framer (ours framed,
Qt's server alike) cannot answer an unknown function code with exception 01
because it cannot frame it, only the burst endpoint can; and Qt's client
reports a Diagnostics (0x08) response as `InvalidResponseError` at its API
although the echo on the wire is correct.

The explicit 2026-09-11 cross-stack audit reproduced RTU count overflow,
Layout offset overflow, stale-progress/empty-input adapter deadline defects
and an RX watchdog blind spot. The scoped fixes and their current host/ARM
evidence are in `doc/PARANOID_AUDIT_2026-09-11.md`. They do not change valid
wire formats or UART ISR/cache paths. Fresh board validation is recorded in
`doc/HARDWARE_REGRESSION_2026-09-12.md`: the complete 33-image COBS/RTU matrix,
132 targeted live trials in -Os/-O2/-O3, framed RTU through 10M and eight Qt
interop runs passed their acceptance checks. The failed programming attempt
and burst-candidate boundary losses are retained, not relabelled as passes.
Original firmware was restored and read back. Evidence identifies the
uncommitted audit inputs; older records must not be attributed to this revision.
`Layout::store_count()` now returns a checked bool, and
the stream suite releases its retained Packet with reset(), not placement-new
over a const local. Keep both sanitized and optimized framing fuzz tests.

## Project overview

The current design decisions and their acceptance evidence are in
`doc/SHARED_POLICIES_PLAN.md` and `doc/SHARED_POLICIES_VALIDATION.md`.
`doc/COBS_REFACTOR_PLAN.md` and `doc/COBS_ENGINE.md` are the completed
2026-09-01 refactor and its historical rationale; the pure-codec overlap proof
in `COBS_ENGINE.md` still applies, its API examples do not. Consult the current
documents before changing names, ownership, storage, delegates, state fields,
or file boundaries.

The stable documentation is split by boundary:

- `doc/INTEGRATION.md` — the usage guide: every supported composition (RTU
  through `UartAdapter`, COBS on the driver directly, FreeRTOS on top, RTU
  without the adapter, any other byte transport); its snippets are the
  translation units in `doc/examples/`, compiled and run by
  `sh doc/examples/build.sh` against the real headers and the host fakes;
- `doc/ARCHITECTURE.md` — canonical component/API/ownership entry point for COBS;
- `doc/PROTOCOL.md` — normative COBS wire format (v2: length prefix + CRC trailer) and decoder behavior;
- `doc/STORAGE.md` — the shared raw-byte storage contract used by both protocols;
- `src/modbus/ARCHITECTURE.md` and `src/modbus/README.md` — the Modbus RTU endpoint;
- `src/crc/README.md` — the protocol-independent CRC policy library;
- `doc/PROTOCOL_COMPARISON.md` and `doc/COBS_PERFORMANCE.md` — measured H7S evidence;
- `src/adapters/qt/tests/hardware/h7s/README.md` — the RTU stack against Qt's
  QtSerialBus, both ways round, on the H7S.

Repository layout: `src/` holds the stack itself (`wire/`, `crc/`, `cobs/`,
`modbus/`, `uart/`) and `src/adapters/`, the glue that knows both a transport
and an endpoint while neither knows it (`rtu/UartAdapter.h`,
`freertos/FreeRtosWake.h`, `qt/SerialAdapter.h` with the QModbus-shaped
`qt/RtuClient.h` on top); `uart/` is only the driver, its
tests and the probe header, and `modbus/` names no transport; `libs/` at the
root holds the third-party dependencies
(`spsc`, `delegate`, the vendored STM32 HAL/CMSIS packages) and is never part
of `src/`, because a dependency is not the stack's source; `app/` is the Qt
host application (`main.cpp`, `mainwindow.*`); `doc/` documents the whole
repository; `build/` and `stm32_cube_test/` are local and gitignored. Every
build and test script derives its paths from an explicit `ROOT` (the
directory holding `COBS.pro`, found by walking up from the script's own
directory), `SRC = ROOT/src` and `LIBS = ROOT/libs`, never from a `../..` or
`parents[N]` chain that encodes the script's depth. Include paths are
`src`-relative (`#include "modbus/rtu/Rtu.h"` with `-I src`), so moving a
script or a document never changes an include.

Four libraries share one repository: `wire/` (scalar codec, stateless readers,
the `wire::Heap` / `wire::Pool<Rx, Tx>` storage specifications and the
`Geometry`/`Storage` concepts), `crc/` (`crc::Policy`, CRC8/16/32/64 Bitwise
and Table engines, `NoCrc`), `cobs/` and `modbus/`. Both protocol endpoints are
spelled `Endpoint<Memory, Format>`: the same `wire::Pool<8, 2>` goes into
either, and the protocol's `Format` names the CRC policy (`cobs::Format<Crc,
RxMax, TxMax>`, `modbus::rtu::Format<Crc, MaxAdu>`). The RTU endpoint has an
optional third parameter, `Framer = framing::None`: a
`framing::Standard<Direction>` policy (or a user type derived from it) adds
`consume()` for arbitrary stream chunks and a builder-owned length prefix
for private functions; with the default nothing changes (`src/modbus/ARCHITECTURE.md` §8).
`src/adapters/rtu/UartAdapter.h` is the integration object between `src/uart/Uart.h` and
either RTU endpoint (RX/gap/transport binding, `proceed(now_ms)`
orchestration, and the stale-frame rule for framed endpoints, which needs the
driver's chunk geometry and the baud); it does not include the driver, so it
compiles against the host fake HAL.
The RTU hardware harness builds either endpoint (`MODBUS_HW_FRAMER`), and
`src/modbus/rtu/tests/hardware/h7s/run_framing.py` / `verify_framing.py` produce
and recheck the framed-versus-burst record.

A Qt Widgets application (qmake, C++20) intended as a desktop host/testbed for a reusable UART + COBS communication stack. The Qt GUI itself is currently a bare scaffold (`main.cpp`, `mainwindow.*`), but `COBS.pro` includes `src/cobs/cobs.pri` (which includes `src/wire/wire.pri`) and therefore compiles the real non-template COBS core. The separate console consumers under `src/cobs/tests/qmake_consumer/` and `src/modbus/rtu/tests/qmake_consumer/` instantiate and execute the full public APIs over both built-in storage specifications. The STM32 implementation remains in `src/uart/Uart.h` (not part of the Qt build — it needs an STM32 HAL).

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
sh src/cobs/tests/qmake_consumer/run.sh
```

The consumer includes only `Cobs.h`, links `Decoder.cpp` and `Encoder.cpp`
through `cobs.pri`, and runs the same bind/send/receive flow over `wire::Heap`
and `wire::Pool`. A downstream qmake project uses
`include(path/to/cobs/cobs.pri)` or `include(path/to/modbus/rtu/rtu.pri)`;
both pull in `src/wire/wire.pri` and `src/crc/crc.pri`. Set `COBS_DELEGATE_DIR` /
`MODBUS_DELEGATE_DIR` before the include only when `tiny_delegate` is not at
the repository default.

GUI source/header/form files must be added to `SOURCES`/`HEADERS`/`FORMS` in
`COBS.pro`. Library files belong in `src/cobs/cobs.pri`, `src/modbus/rtu/rtu.pri`,
`src/wire/wire.pri` or `src/crc/crc.pri`. Re-run qmake after changing any source list.

### STM32 portability matrix

`src/uart/Uart.h` is verified by compile-only builds for real STM32 targets (F1 = legacy SR/DR IP, G4 = new ISR/RDR IP + classic DMA, H7RS = Cortex-M7 + D-cache + GPDMA), using the arm-none-eabi-gcc 14.3 shipped with STM32CubeIDE 2.0.0 and HAL drivers in `libs/` / the local Cube repository:

```bash
sh src/uart/tests/port/build.sh
```

Objects land in `src/uart/tests/port/out/`; inspect codegen with the same toolchain's `arm-none-eabi-objdump -d -C`. IDE clangd errors like "main.h not found" inside `src/uart/Uart.h` are expected — that header only compiles against an STM32 HAL via this matrix or the host fake HAL below.

### Host test suite (executable)

`src/uart/tests/host/` runs the driver against a fake HAL on the desktop — unlike the port matrix it EXECUTES the interleavings:

```bash
PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH" sh src/uart/tests/host/run.sh
```

The fake HAL models the real behaviours verified in the ST sources (IDLE/TC end reception before the callback; every RX error is blocking in DMA mode; an abort may raise the completion callback of the transfer it interrupts; aborts can return `HAL_TIMEOUT`), plus a PRIMASK where an interrupt raised while masked becomes **pending** and runs on restore, and a DMA ownership model that asserts DMA-owned memory is never handed to the consumer.

Tests are grouped by guarantee (Initialization, RxOwnership, RxDiscontinuity, TxOwnership, TeardownArbitration, FaultInjection, Watchdog), not by HAL function, so they survive refactoring inside the driver.

**Run both suites after any change to `uart/`.** The driver is frozen apart
from two admitted changes, both 2026-09-06 and both closed: the optional
ISR-side `WakeHandler`, whose cost was measured A/B on the H7S before
acceptance (`doc/UART_PARANOID_AUDIT.md` §9.2), and the read-only
thread-context `rx_progress()` accessor, whose zero cost is proved by
byte-identical port-matrix objects (§9.3). The public contract is closed:
anything further belongs in an adapter or the application, not in
`src/uart/Uart.h`. `src/adapters/freertos/FreeRtosWake.h` is the FreeRTOS glue on top of the
wake hook and is compiled in the host suite against the recording fake in
`src/adapters/tests/fake_freertos`; the port matrix pins the ISR thunk sizes, so
a change there must come with a measured reason.

### Shared, COBS, Modbus, CRC and adapter host tests

None of the four library layers owns a HAL, so their suites are ordinary host programs — no fake anything; the adapters suite is the exception, it runs the real driver on the host fake HAL and the FreeRTOS glue on a recording FreeRTOS fake. Run all five after a change to any of them; the protocol suites instantiate the shared storage and CRC libraries, and the parity suite instantiates both protocols:

```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH"
sh src/wire/tests/run.sh
sh src/cobs/tests/run.sh
sh src/modbus/rtu/tests/run.sh
sh src/crc/tests/run.sh
sh src/adapters/tests/run.sh
```

The Qt adapters have their own runner, because they need a Qt kit with
QtSerialPort — on this machine only the Qt 6.4.3 MinGW kit has that module,
with its own GCC 11.2, so the script uses that kit and not the 6.10.1 one the
application is built with:

```bash
sh src/adapters/qt/tests/run.sh
```

It runs `adapters/qt/SerialAdapter.h` and the QModbus-shaped
`adapters/qt/RtuClient.h` on a `QIODevice` stand-in for the port (202 checks,
a real event loop, no COM port), the compile-fail contract, and builds
`qmodbus_bench`, the PC side of the hardware comparison against QtSerialBus.
An additional 22-check test exercises its in-memory journal and one-shot
host-stall injection without opening a COM port.
That comparison itself — the board as a reference server against Qt's client
and ours, then the board as a client against Qt's server, one shared 55-step
script and one reference model (`src/modbus/rtu/tests/reference_model.h`)
for every party — is `src/adapters/qt/tests/hardware/h7s/run_qmodbus.py`,
rechecked by `verify_qmodbus.py`; the record and the findings are in that
directory's README. The reference model is checked on the host by the RTU
suite (`test_reference_model`).

Each runner first compiles its public headers independently and (for the protocols) verifies intentional compile-fail translation units with boundary-specific diagnostic markers (nine for COBS, eleven for RTU, one for the adapters): the `wire::Storage` contract, the CRC-in-Format limits, coordinator-only message/packet operations, serializer constraints, the physical absence of old API names, and for RTU the absence of `consume()` without a framing policy, the rejection of a half-written policy and of a non-RTU endpoint handed to `UartAdapter`.

`src/wire/tests/run.sh` (the shared layer):

- `test_scalar` — the native/BE/LE scalar codec both protocols serialize with.
- `test_block_pool` — `wire::detail::BlockPool`, the raw memory primitive under `wire::Pool`.
- `test_storage` — the storage contract (`doc/STORAGE.md`): one body run against `wire::Heap` and `wire::Pool`, bound to stand-in geometries and to the real `cobs::Endpoint<>::Geometry` / `modbus::rtu::Endpoint<>::Geometry`; alignment of every RX grant, independent quotas, release checking, and the negative `Geometry`/`ByteStorage`/`Storage` concept cases. Both pool suites run a second time under `-DNDEBUG`, because a guarantee that only holds in debug builds is not one.
- `test_protocol_storage` — one user-written memory specification pushed through both endpoints: over-grants, under-grants, growth failure, retained packets, exact descriptor return.
- `test_api_parity` — the deliberately shared API shape of the two protocols, and the type identity of `Layout`/`Storage`/`Message`/`Packet` between Bitwise and Table policies.

`src/cobs/tests/run.sh`:

- `test_decoder` / `test_codec_exhaustive` / `test_encoder` — the pure COBS codec against independent oracles.
- `test_geometry` — `payload_capacity_for_storage` is the exact inverse of `tx_storage_size_for_capacity` for every grant, on every header/CRC width.
- `test_receiver` — the internal RX vertical end to end: length-field codec, exact-allocation proof through a recording storage, queue/teardown behavior, and every way a declared length can disagree with a frame.
- `test_packet`, `test_message`, `test_endpoint` — the public ownership boundary (`Endpoint<Memory, Format>`, `Message`, `Packet`) over `wire::Heap` and `wire::Pool`, through delegates and a capture transport.
- `test_crc` — the CRC-bearing v2 frame: every built-in policy, sum and stateful policies, corruption of every payload/trailer bit, empty/maximum frames, the H1/H2 threshold, the explicit `Format<crc::NoCrc, 255>` legacy vectors, and the v1/v2 mixing hazard.
- `test_layout` — exact ABI snapshots; `check_arm_layout.sh` compiles the same file for Cortex-M.

`src/modbus/rtu/tests/run.sh` mirrors this for RTU (`test_crc`, `test_crc_geometry`, `test_packet`, `test_message`, `test_endpoint`, `test_fuzz`, `test_framing` — the `framing::Layout` rules and the standard function table against the specification's worked examples in both directions —, `test_stream` — the framed endpoint: every cut of a frame, several frames per chunk, every error class and its recovery, the builder-owned length prefix —, `test_layout`); `src/crc/tests/run.sh` checks the four default models and seven further catalogue models against their check values plus random inputs against bit-level oracles.

`src/adapters/tests/run.sh` covers the glue: `test_uart_integration` — the real UART driver on the host fake HAL, integrated through `UartAdapter` with both endpoint kinds, including the framed stale-frame rule at 9600 baud with multi-chunk frames, the DMA-progress check that keeps a bridge-split frame alive at 115200 when no IDLE/TC event has fired yet, and the adapter's bind/unbind lifecycle; `test_freertos_wake` — `FreeRtosWake` on the recording FreeRTOS fake (one notification per ISR event with work, coalescing, the yield request, a refused null task handle); both adapter headers compiled on their own; one compile-fail contract (a non-RTU endpoint handed to `UartAdapter`).

The scripts build with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` and add `-fsanitize=address,undefined` when the toolchain provides the runtime. MinGW does not, so for a sanitized run use WSL (the exact command is in each script header); every runner prints whether its build was sanitized. `WIRE_POOL_CHECKS` (on by default in EVERY build, `NDEBUG` included) compiles in the pool's double-free and foreign-pointer detection; a rejected free is counted and ignored rather than corrupting the free list. Set it to 0 explicitly, identically in every translation unit, to opt out.

Codegen guards that need the ARM toolchain or an ELF host: `src/crc/tests/check_arm_codegen.sh`, `python -B src/crc/tests/check_arm_matrix.py`, `src/wire/tests/check_arm_hotpath.sh`, `src/modbus/rtu/tests/check_arm_crc_codegen.sh`, and `src/wire/tests/check_shared_crc.sh` (ELF objects only: WSL or arm-none-eabi, not MinGW). MSVC x64/x86 builds run through `src/wire/tests/check_msvc.ps1`.

## Architecture

`doc/old/UART_COBS_ARCHITECTURE.md` is the archived original design sketch. It remains
useful as historical rationale, but its sample API and parts of its UART model
are not current. The implemented boundaries are:

- **Three layers**: byte transport (UART/TCP/…) → protocol endpoint (framing, integrity, packet lifetime) → application (`Message` / `Packet`). "UART handles bytes. COBS handles packets." Modbus RTU is a sibling endpoint over the same transport contract; its RX boundary is one complete burst candidate (`receive_adu`), COBS's is an arbitrary stream chunk (`consume`).
- The transport is bound as one owning `tiny::delegate` pair for busy state and `send(span)`; it has **no TX queue**, no knowledge of framing, CRC, or packet sizes. TX-busy policy (retry/drop/queue) belongs to layers above.
- Both endpoints are `Endpoint<Memory, Format>`. `Memory` is a `wire::Storage` specification (`wire::Heap` by default, `wire::Pool<Rx, Tx>` for deterministic storage, or a user type with a nested `template<class Geometry> class For`); the endpoint derives a three-number `Geometry` from its Format and binds `Memory::For<Geometry>`. Storage speaks physical bytes only and never sees a header, a length field or a CRC. Changing memory must not change the application-facing API or wire format.
- `Format` names the wire contract including the CRC policy from `crc/`: `cobs::Format<Crc = crc::Crc16Bitwise, RxMax = 255 - Crc::wire_size, TxMax = RxMax>` and `modbus::rtu::Format<Crc = crc::Crc16Bitwise, MaxAdu = 256>`. Equal-width Bitwise/Table policies share `Layout`, `Storage`, `Message` and `Packet` types. `Format<crc::NoCrc, 255>` is the byte-identical COBS v1 format.
- **RX and TX ownership are deliberately asymmetric**: RX packets use the intrusive shared `Packet` handle (refcount inside the protocol's private `RxBlock`, payload immutable once decoded); TX frames use exclusive ownership through one `wire::TxBlock` descriptor (`Message` owns until `send()` succeeds, then the endpoint holds it until DMA completes, then returns the descriptor exactly as granted).
- RX callbacks deliver arbitrary byte chunks (a frame may span chunks, or one chunk may hold several frames); the span is valid only during the callback. On errors COBS drops bytes until the next `0x00` delimiter to resynchronize.
- CRC is a protocol policy, never a UART feature. COBS covers the payload only (the length is checked structurally); RTU covers address, function and data.

For current COBS work, start with `doc/ARCHITECTURE.md` and follow its links
to `PROTOCOL.md` or `STORAGE.md`; for RTU, `src/modbus/ARCHITECTURE.md`.
`COBS_ENGINE.md` retains the reviewed decoder state machine and the in-place
overlap proof, but its storage/API examples predate the shared storage layer.
For current UART behavior, read `src/uart/Uart.h` and its executable host and
portability tests rather than treating the old sketch as an API contract.

## Reference material

`doc/old/` holds legacy STM32 UART driver code (HAL-based `UartEngine`, RS-485 wrapper, DMA variants) with a README in Ukrainian. It is prior art for the new design, not part of the build — don't extend it; the new architecture intentionally replaces its approach.
