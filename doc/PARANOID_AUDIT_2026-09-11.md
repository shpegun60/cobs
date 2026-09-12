<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# Cross-stack correctness and recovery audit, 2026-09-11

Baseline: `464ccc50d519ab53e8a4ee751597a95e563e8f88`.
Results below describe the working-tree corrections on top of that baseline,
not the unmodified commit. No commit, push, board reset or flash was performed
as part of this audit. Historical hardware JSON/JSONL records were not edited.

Follow-up: [the 2026-09-12 live-board regression](HARDWARE_REGRESSION_2026-09-12.md)
now verifies these fixes on H7S, including targeted real-DMA faults and slow
adapter deadlines in three optimization builds. The no-flash statements in
this document describe the original 11 September host/ARM audit, not that
separate follow-up session.

Scope: active COBS and RTU implementations, shared storage/scalar/CRC code,
UART ownership/cache/IRQ/recovery paths and their MCU integration. Qt and
FreeRTOS integrations were included in regression coverage; the Qt transaction
recovery work from the preceding revision was not redesigned. Archived code
and third-party implementations were not rewritten.

## Confirmed defects and corrections

### 1. An owned RTU count field could silently truncate a valid large message

`Format<Crc16Bitwise, 512>` and a private function using
`Layout::length_prefixed(1)` accepted a 300-byte body. TX returned `Sent`,
emitted 305 ADU bytes, but wrote count **44**. The equally configured stream
receiver failed CRC on the incorrectly delimited prefix instead of receiving
the message. The defect also matters with `NoCrc`: a truncated frame need not
be rejected by any checksum.

`Layout::store_count()` now returns a checked `bool`. It refuses a short
header or a count above the selected field's range without modifying bytes.
`Message::apply_framing()` propagates failure to `SendResult::Invalid` before
CRC calculation or transport invocation. The caller retains the writable
message and its original storage grant. Storage capacity cannot widen a
one-byte field; a two-byte prefix remains the choice for a large body.

Regression coverage includes 0/1/254/255/256/300-byte bodies, maximum ADUs,
NoCrc/Bitwise/Table, BE16/LE16 and the 65,535-byte ADU ceiling. A counting policy
proves that an invalid count never reaches the CRC. Direct codec tests cover
255/65,535 and one-past limits, short headers, `SIZE_MAX`, byte orders and the
last legal header positions.

The helper's return type is an intentional API correction: direct users of
`store_count()` must check the result. Normal Endpoint usage needs no changes.

### 2. Overflow in the Layout factory could produce an out-of-bounds read

`byte_count_at(SIZE_MAX, 1)` passed the old `offset + width <= 14` test after
unsigned overflow. Narrowing produced offset 255, then the stream receiver's
small header-size field wrapped. Consuming only address/function reached a
read outside the Endpoint. ASan reproduced a heap-buffer-overflow through
`Layout::load()` and `StreamReceiver::begin_frame()`.

The factory now checks `offset > max_header_size - width` after validating
width. Invalid offsets give `unsupported()` before a header is consumed or
allocated. Compile-time assertions and a heap-placed endpoint regression
cover `SIZE_MAX`, adjacent values and both field widths.

This was reachable through invalid arguments in a custom framing policy,
not directly by sending a special byte count to an ordinary standard framer.
Layouts must be constructed with the validated factories; manually corrupting
their public fields or passing undersized spans to preconditioned helpers is
not made safe by this change.

### 3. A frozen DMA progress value could retain an RTU frame forever

After a partial frame, a single unpublished byte was enough for every
deadline to extend: the adapter tested `rx_progress() != 0`, not new progress.
With a missing publication event and a stopped line, the same RX block could
remain held indefinitely.

An overdue snapshot now records the current chunk's progress. Only an increase
can extend the next deadline. Publication, gap and detach reset the baseline;
a new chunk may legitimately start with the same count as an earlier chunk.
The existing prepare/drain/finish ordering is preserved, including a
continuation published between the snapshot and the drain.

The real UART driver on the fake HAL reproduces and checks first progress,
frozen progress, subsequent advances, late-tail rejection, a new progress
domain, one expiry and full block reclamation. No ISR reads or timestamps
were added. The ARM adapter grows from 28 to 32 bytes (a 16-bit baseline plus
alignment); the UART and protocol Endpoint layouts do not grow.

### 4. RX watchdog could believe stale HAL software state indefinitely

An exhausted normal-mode RX DMA counter with a lost completion interrupt
still left HAL reporting `BUSY_RX`. The periodic audit treated it as healthy.
A cleared USART DMAR request with unchanged software state was equally blind.

The existing slow-path audit now also checks the hardware request and remaining
count: zero, an impossible count above ChunkSize, or disabled DMAR is bad.
Recovery uses the existing debounce and confirmed-stop ownership path. Old
untrusted bytes are discarded with an ordered gap; they are not fabricated
into a successful frame. Default supervision remains three bad checks at the
200-ms audit cadence, subject to the application servicing `proceed()`.

Tests prove that a quiet armed line does **not** time out, a completion inside
the debounce window is delivered normally, a stuck receiver restarts, the gap
precedes subsequent data, and DMA-owned storage is never exposed prematurely.
No fields, cache operations or instructions were added to the UART ISR paths.

### 5. An empty adapter delivery changed the stale-frame deadline

Although Uart never publishes an empty data chunk, `UartAdapter::on_rx()` is
also a public harness/transport entry point. An empty span was interpreted as
a new partial chunk. At 9600 baud it changed a full-chunk allowance of 325 ms
to 5 ms and expired a healthy frame before its continuation.

For a framed endpoint, empty input now leaves the deadline and progress
baseline untouched. Both the deadline invariant and a delayed complete-frame
regression failed before this correction and pass afterwards. Burst-candidate
semantics remain unchanged.

### Test correctness and documentation

The old stream pool-exhaustion test explicitly destroyed an automatic
`const Packet` and placement-constructed into that same storage through
`const_cast`. That lifetime reuse was invalid C++, even if sanitizers did not
complain. It now uses a mutable handle and the public `Packet::reset()`.

The UART random generator's IRQ-masked operation always wrote two bytes,
even when the current DMA buffer had only one byte left. That could create
a harness ownership violation unrelated to any driver defect. A targeted
256-seed, one-byte-left regression reproduced it; the generator now skips
that inapplicable operation. The long run below includes the corrected
generator and its own regression.

The framing/storage limits, checked helper, progress rule and recovery
preconditions are documented in the Modbus README and architecture. The
RtuLimits comment no longer claims that framed large ADUs need one UART burst.
The README explicitly distinguishes RTU's eventual aligned-chunk recovery
from COBS's delimiter resynchronization.

## Verification actually performed

- Full COBS GCC/WSL suite, ASan+UBSan and optimized builds: 19,983 decoder
  checks; 20,361 integrity checks; 960,800 exhaustive decoder cases across
  segmentation/prearm plans and 177,146 encoder/payload/headroom cases per
  build, plus geometry, storage-facing RX, Packet, Message and Endpoint suites.
- Full RTU GCC/WSL and MinGW suites: 445 Layout checks, 457 stream checks,
  CRC/geometry/ownership/candidate fuzz and compile-fail suites. Framing,
  stream and stream-fuzz tests run again under `-O3 -DNDEBUG`.
- New independent stream oracle: 2,000 frame trains for each of three policies
  and each of Heap/Pool, followed by 50,000 damaged-stream operations per
  policy. It covers all supported count shapes up to the 14-byte header,
  empty feeds, cuts/gluing, malformed declarations, CRC, gaps, expiry, skips,
  allocation failure and retained owners. It does not use Layout or the CRC
  implementation to construct or validate reference frames.
- UART strict, old-RxEvent fallback, registered and external callbacks,
  optimized and ASan+UBSan variants: 254 checks each, 267 with registered
  callbacks; ten invalid configurations rejected at the intended boundary.
  The sanitized suite additionally passed all 32 deterministic seeds at
  **1,000,000 events each**. These are simulated HAL interleavings, not
  32 million events generated on a physical MCU.
- MCU adapter: 100 integration checks under ASan+UBSan and `-O3 -DNDEBUG`;
  FreeRTOS recording fake: 17 checks. Integration examples compile and run.
- Shared wire/scalar/storage/API-parity and CRC suites pass, including release
  pool checks and the shared-table ELF guard: NoCrc/Bitwise have no table;
  two Table translation units link one 512-byte CRC16 table.
- MSVC x86 and x64 release builds/runs cover CRC, storage/parity/custom memory,
  COBS CRC/layout and RTU geometry/framing/stream/fuzz/layout. VsDevCmd emits
  an environment warning about missing `vswhere.exe`, but `cl` actually builds
  and every executable runs successfully. This is not a Clang validation.
- Qt 6.4.3 / MinGW 11.2 adapter/client suite: 202 checks; trace suite: 22.
  The QtSerialBus harness builds and answers `--help`; no COM port is opened.

### ARM and complete firmware builds

GNU Arm 14.3 code-generation matrix: M0/M0+/M3/M4/M7/M23/M33/M55,
`-Os/-O2/-O3`, little/big endian and default/strict alignment:
**96 scalar + 96 protocol objects**, plus **48 COBS codec objects** (the codec
loop is little-endian in this matrix). CRC-specific guards also pass.

The new count codecs have no helper calls and use alignment-safe byte stores.
Without forced inlining, GCC `-Os` materialized a Layout on the stack and
called its generic store helper; the focused inline annotation eliminates
that cost. An invalid constant offset reduces to returning false; the M7
disassembly is `movs r0, #0; bx lr`.

F1/G4/H7RS real-HAL compile matrix, G4 static analyzer and new RTU adapter
integration probes pass. The pinned G4 `-Os` gates remain:
RX thunk 100 bytes / 8-byte stack frame, TX thunk 94 bytes, arm 108 bytes,
publish 40 bytes, idle proceed 36 bytes. Probe-disabled and macro-stubbed
disassembly are identical. These are code-size/individual-stack-frame checks,
not cycle measurements or a cumulative stack proof.

Complete NUCLEO-H7S3L8 COBS and framed-RTU harnesses compile and link against
the local Cube scaffold. The Pool-only RTU image links no allocator symbol.
Building these ELF files does **not** constitute flashing or board execution.

## Reproduce

Run the host scripts from the repository root. Use WSL/GCC for sanitizers;
do not run WSL and MinGW copies of the same suite simultaneously because
they share that suite's `out/` directory.

```sh
CXX=g++ sh src/wire/tests/run.sh
CXX=g++ sh src/crc/tests/run.sh
CXX=g++ sh src/cobs/tests/run.sh
CXX=g++ sh src/modbus/rtu/tests/run.sh
CXX=g++ sh src/uart/tests/host/run.sh
CXX=g++ sh src/adapters/tests/run.sh
src/uart/tests/host/out/sanitizers.exe --steps 1000000
sh src/wire/tests/check_shared_crc.sh
sh doc/examples/build.sh
```

The configured Windows ARM/Qt tool paths are used by these Git Bash commands:

```sh
sh src/wire/tests/check_arm_codegen_matrix.sh
sh src/uart/tests/port/build.sh
sh src/adapters/qt/tests/run.sh
sh src/cobs/tests/hardware/h7s/build.sh
MODBUS_HW_FRAMER=1 sh src/modbus/rtu/tests/hardware/h7s/build.sh
```

MSVC: `powershell -NoProfile -ExecutionPolicy Bypass -File
src/wire/tests/check_msvc.ps1`. Local audit logs are in the ignored
`build/paranoid_review_20260911/` directory; the test sources and runners in
this patch are the portable reproduction artifacts.

## Boundaries that remain explicit

No additional production defect was confirmed in COBS, wire or CRC under
their documented contracts. That is a bounded audit result, not a proof that
all possible executions are correct. Valid frames retain their wire bytes;
CRC policy freedom, storage policy shape, delegates, Packet/Message ownership
and the UART/packet-memory separation are unchanged.

At completion of this host audit, the new runtime behavior still needed a
fresh live-board regression. That boundary is now covered by the linked
12 September follow-up, within its explicitly tested scenarios. No new CPU percentage, instruction-execution
count or UART-throughput result is claimed here. Cross-compiling for multiple
cores/endian modes does not execute on those platforms. Clang/emulated ARM
runtime were unavailable and were not substituted with misleading green runs.

The original lifetime, one-execution-domain, IRQ-priority and DMA-accessible
memory requirements still apply. Persistently disabled interrupts or broken
hardware are not repaired by this watchdog. A length-based RTU framer is not
a t1.5/t3.5 implementation or a general self-synchronizing decoder; NoCrc adds
no integrity check, and an arbitrary CRC policy is not semantically validated
by the library. Historical hardware evidence remains evidence for its recorded
source snapshots only.
